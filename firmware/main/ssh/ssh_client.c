#include "ssh_client.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "libssh/libssh.h"
#include "libssh/callbacks.h"
#include "mbedtls/sha256.h"
#include "settings/settings.h"
#include "nvs.h"
#include "net/tailscale.h"
#include "net/ts_host.h"
#include "ssh/device_key.h"
#include "sys/link_guard.h"
#include "servers/host_rules.h"
#include "ssh/cb_gate.h"
#include "storage/sd.h"
#include "i18n/i18n.h"

static const char *TAG = "ssh";
int libssh_esp32_init(void);

struct ssh_client {
    ssh_params_t p;
    ssh_data_cb_t data_cb; ssh_state_cb_t state_cb; ssh_hostkey_cb_t hostkey_cb; void *user;
    ssh_session session; ssh_channel channel;
    RingbufHandle_t tx;
    volatile bool stop;
    volatile int new_cols, new_rows; volatile bool resize_pending;
    ssh_state_t state;
    volatile int exit_status;
    char err[160];
    TaskHandle_t task;
    bool ext_stack;     /* created WithCaps (stack in PSRAM) */
    volatile bool kill;     /* C1: the link is going down: close now (the owner still holds c) */
    volatile bool closed;   /* the session is disconnected and freed (the object may still live) */
    bool sd_held;           /* B6: ssh_task holds the SD card for this entry's key file (sd_mount) */
    cb_gate_t gate;         /* I1: every callback runs inside it; ssh_client_stop waits for the one in flight */
};

/* C1: the live clients, so a path that takes the link down can close them first (the SSH Terminal and the
   dashboard's take-over). A client is listed from its start until its task frees it. */
#define LIVE_MAX 8
static ssh_client_t *s_live[LIVE_MAX];
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;
static void live_add(ssh_client_t *c)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < LIVE_MAX && !ok; i++) if (!s_live[i]) { s_live[i] = c; ok = true; }
    taskEXIT_CRITICAL(&s_live_mux);
    if (!ok) ESP_LOGE(TAG, "more than %d SSH clients: this one isn't closed before a link-down", LIVE_MAX);
}
static void live_del(ssh_client_t *c)
{
    taskENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < LIVE_MAX; i++) if (s_live[i] == c) s_live[i] = NULL;
    taskEXIT_CRITICAL(&s_live_mux);
}
#define GONE(c) ((c)->stop || (c)->kill || (c)->gate.detached)

/* I1: the callback gate, under s_live_mux (a callback itself runs outside it). */
static void gate_lock(void *ctx) { (void)ctx; taskENTER_CRITICAL(&s_live_mux); }
static void gate_unlock(void *ctx) { (void)ctx; taskEXIT_CRITICAL(&s_live_mux); }
static void gate_nap(void *ctx, int ms) { (void)ctx; vTaskDelay(pdMS_TO_TICKS(ms)); }
static const cb_gate_ops_t s_gate_ops = { gate_lock, gate_unlock, gate_nap, NULL };
/* How long ssh_client_stop waits for a callback in flight. The owner's callbacks hold their own short lock, or wait
   in ui_async for the LVGL lock (bounded, UI_ASYNC_LOCK_MS, 1 s) and then for a host-key answer the owner cancels
   before it stops: so 3 s is never reached unless something is stuck, and then the owner keeps its context. */
#define STOP_WAIT_MS 3000

static void set_state(ssh_client_t *c, ssh_state_t st, const char *msg)
{
    c->state = st;
    if (msg) strlcpy(c->err, msg, sizeof(c->err));
    if (!cb_gate_enter(&c->gate, &s_gate_ops)) return;   /* the owner stopped: nobody to tell */
    if (c->state_cb) c->state_cb(st, msg, c->user);
    cb_gate_exit(&c->gate, &s_gate_ops);
}
static void deliver(ssh_client_t *c, const uint8_t *d, int n)
{
    if (!cb_gate_enter(&c->gate, &s_gate_ops)) return;
    if (c->data_cb) c->data_cb(d, (size_t)n, c->user);
    cb_gate_exit(&c->gate, &s_gate_ops);
}

/* ---- known hosts in NVS: key = 16-char hex of sha256(host:port), value = 32-byte key hash ---- */
static void kh_key(const char *host, int port, char *out)
{
    char hp[160]; snprintf(hp, sizeof(hp), "%s:%d", host, port);
    unsigned char d[32]; mbedtls_sha256((const unsigned char *)hp, strlen(hp), d, 0);
    for (int i = 0; i < 7; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[14] = 0;
}

void ssh_known_hosts_clear(void)
{
    nvs_handle_t h;
    if (nvs_open("kh", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
}

/* Reads the pin for key: found (out and len set; free it), none, or a read error. */
static kh_pin_t kh_read(const char *key, void **out, size_t *len)
{
    *out = NULL; *len = 0;
    nvs_handle_t h;
    esp_err_t e = nvs_open("kh", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return KH_PIN_NONE;   /* no pin was ever written */
    if (e != ESP_OK) { ESP_LOGE(TAG, "host key pins: nvs_open %s", esp_err_to_name(e)); return KH_PIN_READ_FAILED; }
    size_t n = 0;
    e = nvs_get_blob(h, key, NULL, &n);
    kh_pin_t r = KH_PIN_READ_FAILED;
    if (e == ESP_ERR_NVS_NOT_FOUND) r = KH_PIN_NONE;
    else if (e == ESP_OK && n > 0 && n <= 64) {
        void *b = malloc(n);
        if (b && nvs_get_blob(h, key, b, &n) == ESP_OK) { *out = b; *len = n; r = KH_PIN_FOUND; }
        else free(b);
    }
    nvs_close(h);
    if (r == KH_PIN_READ_FAILED) ESP_LOGE(TAG, "host key pin %s unreadable (%s, %u bytes)", key, esp_err_to_name(e), (unsigned)n);
    return r;
}

static void fp_format(const unsigned char *hash, size_t hlen, char *fp_out, size_t fp_cap)
{
    char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, (unsigned char *)hash, hlen);
    strlcpy(fp_out, fp ? fp : "?", fp_cap);
    ssh_string_free_char(fp);
}

kh_result_t ssh_known_host_check(ssh_session session, const char *host, int port, char *fp_out, size_t fp_cap)
{
    if (fp_out && fp_cap) fp_out[0] = 0;
    ssh_key srv_key = NULL;
    if (ssh_get_server_publickey(session, &srv_key) != SSH_OK) return KH_ERROR;
    unsigned char *hash = NULL; size_t hlen = 0;
    int rc = ssh_get_publickey_hash(srv_key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
    ssh_key_free(srv_key);
    if (rc != 0) return KH_ERROR;
    if (fp_out && fp_cap) fp_format(hash, hlen, fp_out, fp_cap);
    char key[16]; kh_key(host, port, key);
    void *stored = NULL; size_t slen = 0;
    kh_pin_t pin = kh_read(key, &stored, &slen);
    kh_result_t r = kh_classify(pin, stored, slen, hash, hlen);
    free(stored);
    ssh_clean_pubkey_hash(&hash);
    return r;
}

bool ssh_known_host_fingerprint(const char *host, int port, char *fp_out, size_t fp_cap)
{
    if (fp_out && fp_cap) fp_out[0] = 0;
    if (!host || !host[0] || !fp_out || !fp_cap) return false;
    char key[16]; kh_key(host, port, key);
    void *stored = NULL; size_t slen = 0;
    if (kh_read(key, &stored, &slen) != KH_PIN_FOUND) return false;
    fp_format(stored, slen, fp_out, fp_cap);
    free(stored);
    return true;
}

bool ssh_server_key_hash(ssh_session session, uint8_t out[32])
{
    ssh_key srv_key = NULL;
    if (ssh_get_server_publickey(session, &srv_key) != SSH_OK) return false;
    unsigned char *hash = NULL; size_t hlen = 0;
    int rc = ssh_get_publickey_hash(srv_key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
    ssh_key_free(srv_key);
    bool ok = rc == 0 && hlen == 32;
    if (ok) memcpy(out, hash, 32);
    ssh_clean_pubkey_hash(&hash);
    return ok;
}

kh_pin_t ssh_known_host_hash(const char *host, int port, uint8_t out[32])
{
    if (!host || !host[0]) return KH_PIN_NONE;
    char key[16]; kh_key(host, port, key);
    void *stored = NULL; size_t slen = 0;
    kh_pin_t r = kh_read(key, &stored, &slen);
    if (r == KH_PIN_FOUND) {
        if (slen == 32) memcpy(out, stored, 32);
        else r = KH_PIN_READ_FAILED;
    }
    free(stored);
    return r;
}

bool ssh_known_host_trust(ssh_session session, const char *host, int port)
{
    ssh_key srv_key = NULL;
    if (ssh_get_server_publickey(session, &srv_key) != SSH_OK) return false;
    unsigned char *hash = NULL; size_t hlen = 0;
    int rc = ssh_get_publickey_hash(srv_key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
    ssh_key_free(srv_key);
    if (rc != 0) return false;
    char key[16]; kh_key(host, port, key);
    bool ok = nvs_set_blob_ns("kh", key, hash, hlen);
    ssh_clean_pubkey_hash(&hash);
    return ok;
}

bool ssh_known_host_forget(const char *host, int port)
{
    if (!host || !host[0]) return false;
    char key[16]; kh_key(host, port, key);
    nvs_handle_t h;
    if (nvs_open("kh", NVS_READWRITE, &h) != ESP_OK) return false;
    bool had = nvs_erase_key(h, key) == ESP_OK;
    if (had) nvs_commit(h);
    nvs_close(h);
    if (had) ESP_LOGW(TAG, "forgot the host key pin for %s:%d", host, port);
    return had;
}

#if CONFIG_COMPANION_DEBUG_COMMANDS
bool ssh_known_host_set_debug(const char *host, int port)
{
    if (!host || !host[0]) return false;
    unsigned char wrong[32];
    memset(wrong, 0xa5, sizeof(wrong));
    char key[16]; kh_key(host, port, key);
    bool ok = nvs_set_blob_ns("kh", key, wrong, sizeof(wrong));
    ESP_LOGW(TAG, "debug: host key pin for %s:%d set to a wrong value", host, port);
    return ok;
}
#endif

/* ---- P4C-10: every key the companion link ever pinned (NVS "cc"/"hostkeys", n x 32 bytes, FIFO of 8) ----
   The read has three results (I-new-2): no list yet, a list, or an error (the namespace won't open, the blob is
   corrupt or the wrong size). An error refuses interactive shells (hf_shell_gate) and is never written over
   (hf_hostkeys_may_write), so a bad read can't erase the history. The mutex is made once by ssh_host_keys_init
   (cc_link_start, before the link task); before that every read is an error. */
static StaticSemaphore_t s_hk_buf;
static SemaphoreHandle_t s_hk_mx;

void ssh_host_keys_init(void)
{
    if (!s_hk_mx) s_hk_mx = xSemaphoreCreateMutexStatic(&s_hk_buf);
}

static hf_deny_read_t hk_read(uint8_t out[HF_HOSTKEYS_MAX][32], int *n)
{
    *n = 0;
    nvs_handle_t h;
    esp_err_t e = nvs_open("cc", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return HF_DENY_NONE;
    if (e != ESP_OK) return HF_DENY_ERROR;
    size_t len = 0;
    e = nvs_get_blob(h, "hostkeys", NULL, &len);
    hf_deny_read_t r = HF_DENY_ERROR;
    if (e == ESP_ERR_NVS_NOT_FOUND) r = HF_DENY_NONE;
    else if (e == ESP_OK && len % 32 == 0 && len <= HF_HOSTKEYS_MAX * 32) {
        if (nvs_get_blob(h, "hostkeys", out, &len) == ESP_OK) { *n = (int)(len / 32); r = HF_DENY_FOUND; }
    }
    nvs_close(h);
    return r;
}

hf_deny_read_t ssh_host_keys_load(uint8_t out[HF_HOSTKEYS_MAX][32], int *n)
{
    *n = 0;
    if (!s_hk_mx) return HF_DENY_ERROR;
    xSemaphoreTake(s_hk_mx, portMAX_DELAY);
    hf_deny_read_t r = hk_read(out, n);
    xSemaphoreGive(s_hk_mx);
    return r;
}

void ssh_host_keys_add(const uint8_t hash[32])
{
    if (!s_hk_mx) { ESP_LOGE(TAG, "host keys: not ready, not saved"); return; }
    uint8_t list[HF_HOSTKEYS_MAX][32], before[HF_HOSTKEYS_MAX][32];
    int n = 0;
    xSemaphoreTake(s_hk_mx, portMAX_DELAY);
    hf_deny_read_t r = hk_read(list, &n);
    if (!hf_hostkeys_may_write(r)) {
        ESP_LOGE(TAG, "host keys: the list can't be read; not written over");
    } else {
        memcpy(before, list, sizeof(before));
        int m = hf_hostkeys_add(list, n, hash);
        if (m != n || memcmp(before, list, (size_t)m * 32)) {
            if (!nvs_set_blob_ns("cc", "hostkeys", list, (size_t)m * 32)) ESP_LOGE(TAG, "host keys: not saved");
            else ESP_LOGI(TAG, "host keys: %d known for the Host", m);   /* a count, never a key */
        }
    }
    xSemaphoreGive(s_hk_mx);
}

/* The type of the key the link last authenticated on (NVS "cc"/"hostktype"): an interactive session prefers it, so
   the Host presents the key the deny-list holds even if its sshd prefers a newer type. */
void ssh_host_key_type_note(ssh_session session)
{
    ssh_key k = NULL;
    if (ssh_get_server_publickey(session, &k) != SSH_OK) return;
    const char *t = ssh_key_type_to_char(ssh_key_type(k));
    char *old = NULL;
    if (t && (!nvs_get_str_alloc("cc", "hostktype", &old) || strcmp(old, t))) nvs_set_str_ns("cc", "hostktype", t);
    free(old);
    ssh_key_free(k);
}

static int verify_host(ssh_client_t *c)
{
    char fp[80];
    kh_result_t r = ssh_known_host_check(c->session, c->p.host, c->p.port, fp, sizeof(fp));
    if (r == KH_OK) return 0;
    if (r == KH_PIN_ERROR) { ESP_LOGE(TAG, "host key pin for %s:%d unreadable; not connecting", c->p.host, c->p.port); return -2; }
    if (!kh_needs_answer(r)) return -1;
    set_state(c, SSH_ST_HOSTKEY, fp);
    /* inside the gate (I1): a stopped owner isn't asked, and no callback means nobody to ask (reject) */
    bool in = cb_gate_enter(&c->gate, &s_gate_ops);
    ssh_hostkey_cb_t cb = in ? c->hostkey_cb : NULL;
    bool answer = cb && !GONE(c) && cb(c->p.host, c->p.port, fp, r == KH_CHANGED, c->user);
    if (in) cb_gate_exit(&c->gate, &s_gate_ops);
    if (kh_accept(r, cb != NULL, answer) && !GONE(c) && ssh_known_host_trust(c->session, c->p.host, c->p.port)) return 0;
    return -1;
}

/* libssh returns SSH_AUTH_AGAIN while a call is pending (slow relayed links);
 * the *same* call must be repeated until it completes, otherwise the next API
 * fails with "Wrong state during pending SSH call". */
#define AUTH_RETRY(expr) ({ int _rc; int _n = 0; do { _rc = (expr); if (_rc == SSH_AUTH_AGAIN) vTaskDelay(pdMS_TO_TICKS(100)); } while (_rc == SSH_AUTH_AGAIN && ++_n < 600 && !GONE(c)); _rc; })

static int authenticate(ssh_client_t *c)
{
    int rc;
    c->err[0] = 0;   /* set_state copied "tunnel" or a fingerprint here: only authenticate's own reason counts */
    rc = AUTH_RETRY(ssh_userauth_none(c->session, NULL));
    if (rc == SSH_AUTH_SUCCESS) return 0;
    int methods = ssh_userauth_list(c->session, NULL);
    /* B6, M10: the plan (host_rules.c, host-tested): an empty or "device" path is the Tab5's own key (NVS); a path
       under /sdcard/ is that file, and a missing file or any other path fails: never the device key for it
       (ssh_task checked it before connecting; this is the race). A key entry never tries a password, so a key file's
       passphrase (c->p.pass) is never sent to the server. */
    struct stat st;
    hf_auth_plan_t plan = hf_auth_plan(c->p.use_key, c->p.keypath, stat(c->p.keypath, &st) == 0);
    if (c->p.use_key) {
        ssh_key key = NULL;
        if (plan.key == HF_KEY_FILE_MISSING || plan.key == HF_KEY_BAD_PATH) {
            snprintf(c->err, sizeof(c->err), "%s", tr(plan.key == HF_KEY_BAD_PATH ? STR_HF_KEY_BAD_PATH : STR_HF_KEY_FILE_MISSING));
            rc = SSH_ERROR;
        } else if (plan.key == HF_KEY_DEVICE) {
            rc = (device_key_ensure() == ESP_OK && device_key_get(&key) == ESP_OK) ? SSH_OK : SSH_ERROR;
            if (rc != SSH_OK) snprintf(c->err, sizeof(c->err), "%s", tr(STR_SSH_DEVICE_KEY_UNAVAILABLE));
        } else {
            rc = ssh_pki_import_privkey_file(c->p.keypath, c->p.pass[0] ? c->p.pass : NULL, NULL, NULL, &key);
            if (rc != SSH_OK) snprintf(c->err, sizeof(c->err), "%s %s", tr(STR_SSH_KEY_IMPORT_FAILED), ssh_get_error(c->session));
        }
        if (rc != SSH_OK) { ESP_LOGW(TAG, "%s", c->err); return -1; }
        rc = AUTH_RETRY(ssh_userauth_publickey(c->session, NULL, key));
        ssh_key_free(key);
        if (rc == SSH_AUTH_SUCCESS) return 0;
        ESP_LOGW(TAG, "publickey auth failed: %s", ssh_get_error(c->session));
        snprintf(c->err, sizeof(c->err), "%s", tr(STR_HF_KEY_REFUSED));
        return -1;   /* M10: no password fallback for a key entry */
    }
    if (!plan.try_password) return -1;
    if ((methods & SSH_AUTH_METHOD_PASSWORD) || methods == 0) {
        rc = AUTH_RETRY(ssh_userauth_password(c->session, NULL, c->p.pass));
        if (rc == SSH_AUTH_SUCCESS) return 0;
    }
    if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
        rc = AUTH_RETRY(ssh_userauth_kbdint(c->session, NULL, NULL));
        int guard = 0;
        while (rc == SSH_AUTH_INFO && guard++ < 6) {
            int n = ssh_userauth_kbdint_getnprompts(c->session);
            for (int i = 0; i < n; i++) ssh_userauth_kbdint_setanswer(c->session, i, c->p.pass);
            rc = AUTH_RETRY(ssh_userauth_kbdint(c->session, NULL, NULL));
        }
        if (rc == SSH_AUTH_SUCCESS) return 0;
    }
    return -1;
}

static void ssh_task(void *arg)
{
    ssh_client_t *c = arg;
    set_state(c, SSH_ST_CONNECTING, NULL);
    c->session = ssh_new();
    if (!c->session) { set_state(c, SSH_ST_ERROR, tr(STR_SSH_NO_SESSION)); goto done; }
    const char *refused = ts_host_term_refusal(c->p.host, tailscale_enabled());
    if (refused) {
        /* the switch is off: fail at once, never start Tailscale on our own (spec Goal 5) */
        ESP_LOGW(TAG, "%s needs Tailscale, which is off", c->p.host);
        set_state(c, SSH_ST_ERROR, refused);
        goto done;
    }
    if (c->p.use_key && hf_key_source(c->p.keypath, true) != HF_KEY_DEVICE) {
        /* B6: a key file on the SD card: mount the card on demand, and fail before connecting if the file is gone
           (or the path isn't on the card, M9) */
        hf_key_src_t src = hf_key_source(c->p.keypath, true);
        if (src == HF_KEY_FILE) {
            c->sd_held = sd_mount();   /* released once authenticate has the key in RAM (or at done) */
            struct stat st;
            src = hf_key_source(c->p.keypath, stat(c->p.keypath, &st) == 0);
        }
        if (src != HF_KEY_FILE) {
            ESP_LOGW(TAG, "key file for this host not usable; not connecting");
            set_state(c, SSH_ST_ERROR, tr(src == HF_KEY_BAD_PATH ? STR_HF_KEY_BAD_PATH : STR_HF_KEY_FILE_MISSING));
            goto done;
        }
    }
    {
        /* MagicDNS: resolve tailnet peer names from the MicroLink peer list. */
        char ip[16];
        if (tailscale_resolve(c->p.host, ip, sizeof(ip))) { ESP_LOGI(TAG, "tailnet %s -> %s", c->p.host, ip); strlcpy(c->p.host, ip, sizeof(c->p.host)); }
        /* Plain sockets do not wake the WireGuard session; do it explicitly and
         * wait for the tunnel (DISCO or DERP) before libssh connects. */
        if (tailscale_is_tailnet_ip(c->p.host)) {
            set_state(c, SSH_ST_CONNECTING, "tunnel");
            if (!tailscale_prepare_peer(c->p.host, 60000)) ESP_LOGW(TAG, "tunnel to %s not confirmed, trying anyway", c->p.host);
        }
    }
    ESP_LOGI(TAG, "connecting to %s:%d", c->p.host, c->p.port);
    ssh_options_set(c->session, SSH_OPTIONS_HOST, c->p.host);
    ssh_options_set(c->session, SSH_OPTIONS_PORT, &c->p.port);
    ssh_options_set(c->session, SSH_OPTIONS_USER, c->p.user);
    long timeout = 40;   /* DERP-relayed tailnet peers can be slow */
    ssh_options_set(c->session, SSH_OPTIONS_TIMEOUT, &timeout);
    int nodelay = 1;
    ssh_options_set(c->session, SSH_OPTIONS_NODELAY, &nodelay);
    ssh_options_set(c->session, SSH_OPTIONS_COMPRESSION, "no");
    int verb = SSH_LOG_NOLOG;
    ssh_options_set(c->session, SSH_OPTIONS_LOG_VERBOSITY, &verb);
    if (!c->p.cmd[0]) {
        /* key-type hardening: "^<the type the link last met>", libssh's defaults after it (a reorder only) */
        char *t = NULL;
        char pref[48];
        if (nvs_get_str_alloc("cc", "hostktype", &t) && hf_hostkeys_pref(t, pref, sizeof(pref)) &&
            ssh_options_set(c->session, SSH_OPTIONS_HOSTKEYS, pref) != SSH_OK)
            ESP_LOGW(TAG, "host key preference not set; libssh's defaults apply");
        free(t);
    }
    if (ssh_connect(c->session) != SSH_OK) {
        char m[160]; snprintf(m, sizeof(m), "%s %s", tr(STR_SSH_CANT_CONNECT), ssh_get_error(c->session));
        set_state(c, SSH_ST_ERROR, m); goto done;
    }
    if (GONE(c)) goto done;
    /* The steps after the key exchange, in hf_conn_steps' order (host-tested: an interactive session's deny-list
       check comes first, before the known-hosts check and before any authentication is sent; libssh's ssh_connect
       returns before any userauth request). */
    hf_step_t steps[4];
    int nsteps = hf_conn_steps(!c->p.cmd[0], steps, 4);
    if (!nsteps) { set_state(c, SSH_ST_ERROR, tr(STR_HF_HOSTKEYS_UNCHECKED)); goto done; }
    for (int si = 0; si < nsteps; si++) {
        if (GONE(c)) goto done;
        if (steps[si] == HF_STEP_DENY_CHECK) {
            /* P4C-10 (hard rule 1): never a login shell on the Host, by whatever address or name it was reached */
            uint8_t met[32];
            uint8_t (*keys)[32] = malloc(HF_HOSTKEYS_MAX * 32);
            int n = 0;
            hf_deny_read_t rd = keys ? ssh_host_keys_load(keys, &n) : HF_DENY_ERROR;
            bool have = ssh_server_key_hash(c->session, met);
            hf_gate_t g = hf_shell_gate(true, rd, have ? met : NULL, (const uint8_t (*)[32])keys, n);
            free(keys);
            if (g == HF_GATE_REFUSE_HOST) {
                ESP_LOGW(TAG, "this server presents the Host's key: no login shell");
                set_state(c, SSH_ST_ERROR, tr(STR_HOSTS_NOT_LISTED));
                goto done;
            }
            if (g == HF_GATE_REFUSE_UNCHECKED) {
                ESP_LOGW(TAG, "the Host's keys couldn't be checked: no login shell");
                set_state(c, SSH_ST_ERROR, tr(STR_HF_HOSTKEYS_UNCHECKED));
                goto done;
            }
        } else if (steps[si] == HF_STEP_VERIFY_HOST) {
            ESP_LOGI(TAG, "connected; checking host key");
            int hv = verify_host(c);
            if (hv != 0) { set_state(c, SSH_ST_ERROR, tr(hv == -2 ? STR_SSH_PIN_UNREADABLE : STR_SSH_HOSTKEY_REJECTED)); goto done; }
        } else {
            set_state(c, SSH_ST_AUTH, NULL);
            int auth = authenticate(c);
            if (c->sd_held) { sd_unmount(); c->sd_held = false; }   /* M1: the key file was read (or never will be) */
            if (auth != 0) {
                char m[200];
                if (c->err[0]) snprintf(m, sizeof(m), "%s", c->err);   /* the key's own reason (B6, M10) */
                else snprintf(m, sizeof(m), "%s %s", tr(STR_SSH_AUTH_FAILED), ssh_get_error(c->session));
                set_state(c, SSH_ST_ERROR, m); goto done;
            }
        }
    }
    if (GONE(c)) goto done;   /* C1: closed while the auth ran */
    c->channel = ssh_channel_new(c->session);
    if (!c->channel || ssh_channel_open_session(c->channel) != SSH_OK) { set_state(c, SSH_ST_ERROR, tr(STR_SSH_NO_CHANNEL)); goto done; }
    ssh_channel_request_pty_size(c->channel, "xterm-256color", c->p.cols, c->p.rows);
    if (c->p.cmd[0]) {
        /* An explicit command on a pty: never a login shell (it would start the user's tmux). */
        if (ssh_channel_request_exec(c->channel, c->p.cmd) != SSH_OK) { set_state(c, SSH_ST_ERROR, tr(STR_SSH_CMD_REFUSED)); goto done; }
    } else if (ssh_channel_request_shell(c->channel) != SSH_OK) { set_state(c, SSH_ST_ERROR, tr(STR_SSH_SHELL_REFUSED)); goto done; }
    set_state(c, SSH_ST_CONNECTED, NULL);
    ssh_set_blocking(c->session, 0);

    uint8_t *buf = malloc(4096);
    while (!GONE(c) && ssh_channel_is_open(c->channel) && !ssh_channel_is_eof(c->channel)) {
        bool did = false;
        /* outgoing */
        size_t n = 0;
        uint8_t *item = xRingbufferReceiveUpTo(c->tx, &n, 0, 1024);
        if (item) {
            size_t off = 0;
            while (off < n && !GONE(c)) {
                int w = ssh_channel_write(c->channel, item + off, n - off);
                if (w == SSH_ERROR) break;
                if (w > 0) off += w; else vTaskDelay(1);
            }
            vRingbufferReturnItem(c->tx, item);
            did = true;
        }
        if (c->resize_pending) { c->resize_pending = false; ssh_channel_change_pty_size(c->channel, c->new_cols, c->new_rows); }
        /* incoming (stdout + stderr) */
        int r = ssh_channel_read_nonblocking(c->channel, buf, 4096, 0);
        if (r > 0) { deliver(c, buf, r); did = true; }
        else if (r == SSH_ERROR) break;
        int r2 = ssh_channel_read_nonblocking(c->channel, buf, 4096, 1);
        if (r2 > 0) { deliver(c, buf, r2); did = true; }
        if (!did) {
            /* wait for socket activity or tx data */
            ssh_channel chans[2] = { c->channel, NULL }, outch[2];
            struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
            ssh_channel_select(chans, NULL, NULL, &tv);
            (void)outch;
        }
    }
    free(buf);
    if (c->p.cmd[0] && !GONE(c)) {
        /* the exit-status message can trail the EOF a little */
        /* bounded: if the exit-status never comes (link lost at EOF), don't sit on the
           40 s session timeout before returning to the dashboard */
        long short_timeout = 2;
        ssh_options_set(c->session, SSH_OPTIONS_TIMEOUT, &short_timeout);
        ssh_set_blocking(c->session, 1);
        uint32_t code = 0; char *sig = NULL; int core = 0;
        if (ssh_channel_get_exit_state(c->channel, &code, &sig, &core) == SSH_OK) c->exit_status = (int)code;
        ssh_string_free_char(sig);
    }
    if (c->kill && !c->stop) { c->exit_status = -1; set_state(c, SSH_ST_ERROR, tr(STR_SSH_NET_DOWN)); }
    else set_state(c, SSH_ST_CLOSED, c->stop || c->p.cmd[0] ? NULL : tr(STR_SSH_SERVER_CLOSED));
done:
    if (c->sd_held) { sd_unmount(); c->sd_held = false; }   /* an exit before authenticate */
    if (c->kill && !c->stop && c->state != SSH_ST_ERROR && c->state != SSH_ST_CLOSED)
        set_state(c, SSH_ST_ERROR, tr(STR_SSH_NET_DOWN));
    if (c->channel) { ssh_channel_send_eof(c->channel); ssh_channel_close(c->channel); ssh_channel_free(c->channel); }
    if (c->session) { ssh_disconnect(c->session); ssh_free(c->session); }
    c->closed = true;   /* C1: the Host has seen the close (or there was no session) */
    if (c->kill) ESP_LOGI(TAG, "closed %s:%d before the link went down", c->p.host, c->p.port);
    /* wait for owner to release */
    while (!c->stop) vTaskDelay(pdMS_TO_TICKS(50));
    live_del(c);
    vRingbufferDelete(c->tx);
    bool ext = c->ext_stack;
    free(c);
    if (ext) vTaskDeleteWithCaps(NULL);
    else vTaskDelete(NULL);
}

ssh_client_t *ssh_client_start(const ssh_params_t *p, ssh_data_cb_t data_cb, ssh_state_cb_t state_cb,
                               ssh_hostkey_cb_t hostkey_cb, void *user)
{
    static bool inited = false;
    if (!inited) { libssh_esp32_init(); inited = true; }
    ssh_client_t *c = calloc(1, sizeof(*c));
    c->p = *p; c->data_cb = data_cb; c->state_cb = state_cb; c->hostkey_cb = hostkey_cb; c->user = user;
    if (c->p.port <= 0) c->p.port = 22;
    if (c->p.cols <= 0) c->p.cols = 80;
    if (c->p.rows <= 0) c->p.rows = 24;
    c->tx = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
    c->state = SSH_ST_IDLE;
    c->exit_status = -1;
    live_add(c);
    /* Internal RAM is tight with the companion link's own SSH task running, and it is
       fragmented: fall back to a PSRAM stack (allowed by SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY).
       A task on a PSRAM stack must never run with the cache disabled. Flash reads (NVS host
       pins, the device key) keep the cache on only because code and rodata execute from
       PSRAM (SPIRAM_XIP_FROM_PSRAM), and pinning a new host key writes flash, which is safe
       only while no flash mmap is live (the app has none). */
#if !CONFIG_SPIRAM_XIP_FROM_PSRAM
#error "the SSH task's PSRAM stack fallback needs CONFIG_SPIRAM_XIP_FROM_PSRAM"
#endif
    if (xTaskCreatePinnedToCore(ssh_task, "ssh", 24576, c, 5, &c->task, 1) != pdPASS) {
        c->ext_stack = true;
        if (xTaskCreatePinnedToCoreWithCaps(ssh_task, "ssh", 24576, c, 5, &c->task, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "no memory for the SSH task");
            live_del(c);
            vRingbufferDelete(c->tx); free(c); return NULL;
        }
        ESP_LOGI(TAG, "SSH task stack in PSRAM");
    }
    return c;
}

void ssh_client_send(ssh_client_t *c, const uint8_t *data, size_t len)
{
    if (!c || !len || c->state != SSH_ST_CONNECTED) return;
    xRingbufferSend(c->tx, data, len, pdMS_TO_TICKS(100));
}

void ssh_client_resize(ssh_client_t *c, int cols, int rows)
{
    if (!c) return;
    c->new_cols = cols; c->new_rows = rows; c->resize_pending = true;
    c->p.cols = cols; c->p.rows = rows;
}

bool ssh_client_stop(ssh_client_t *c)
{
    if (!c) return true;
    /* I1: no callback starts after the detach, and the one in flight finishes before this returns. A connect or an
       auth in progress sees GONE and bails, as it does for stop. */
    bool idle = cb_gate_detach_wait(&c->gate, &s_gate_ops, STOP_WAIT_MS);
    if (!idle) ESP_LOGE(TAG, "a callback is still running after %d ms: its owner keeps its context", STOP_WAIT_MS);
    c->stop = true;   /* last: ssh_task frees c once it sees this, so nothing here reads c after it */
    return idle;
}

ssh_state_t ssh_client_state(ssh_client_t *c) { return c ? c->state : SSH_ST_IDLE; }
const char *ssh_client_error(ssh_client_t *c) { return c ? c->err : ""; }
int ssh_client_exit_status(ssh_client_t *c) { return c ? c->exit_status : -1; }

/* ---- C1: close before the link goes down ---- */

static bool on_tailnet(const ssh_client_t *c)
{
    return tailscale_is_tailnet_ip(c->p.host) || ts_host_needs_tailscale(c->p.host);
}

/* Asks every live client (tailnet_only: those to a tailnet host) to close. Their tasks disconnect within
   one 20 ms select; a connect or a host-key prompt in progress closes once it returns. */
static bool lg_term_close(bool tailnet_only, void *ctx)
{
    (void)ctx;
    int n = 0;
    taskENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < LIVE_MAX; i++) {
        ssh_client_t *c = s_live[i];
        if (c && !c->closed && (!tailnet_only || on_tailnet(c))) { c->kill = true; n++; }
    }
    taskEXIT_CRITICAL(&s_live_mux);
    if (n) ESP_LOGI(TAG, "closing %d SSH terminal session%s before the link goes down", n, n == 1 ? "" : "s");
    return false;   /* nothing to reopen: a closed terminal stays closed */
}

static bool lg_term_idle(bool tailnet_only, void *ctx)
{
    (void)ctx; (void)tailnet_only;
    bool idle = true;
    taskENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < LIVE_MAX && idle; i++) {
        ssh_client_t *c = s_live[i];
        if (c && c->kill && !c->closed) idle = false;   /* asked to close and not yet disconnected */
    }
    taskEXIT_CRITICAL(&s_live_mux);
    return idle;
}

void ssh_client_guard_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    static const lg_closer_t closer = { "ssh_terminal", lg_term_close, lg_term_idle, NULL, NULL };
    lg_register(&closer);
}
