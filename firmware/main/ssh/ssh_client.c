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
};

static void set_state(ssh_client_t *c, ssh_state_t st, const char *msg)
{
    c->state = st;
    if (msg) strlcpy(c->err, msg, sizeof(c->err));
    if (c->state_cb) c->state_cb(st, msg, c->user);
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

static int verify_host(ssh_client_t *c)
{
    char fp[80];
    kh_result_t r = ssh_known_host_check(c->session, c->p.host, c->p.port, fp, sizeof(fp));
    if (r == KH_OK) return 0;
    if (r == KH_PIN_ERROR) { ESP_LOGE(TAG, "host key pin for %s:%d unreadable; not connecting", c->p.host, c->p.port); return -2; }
    if (!kh_needs_answer(r)) return -1;
    set_state(c, SSH_ST_HOSTKEY, fp);
    /* read once: ssh_client_stop clears it, and no callback means nobody to ask (reject) */
    ssh_hostkey_cb_t cb = c->hostkey_cb;
    bool answer = cb && !c->stop && cb(c->p.host, c->p.port, fp, r == KH_CHANGED, c->user);
    if (kh_accept(r, cb != NULL, answer) && !c->stop && ssh_known_host_trust(c->session, c->p.host, c->p.port)) return 0;
    return -1;
}

/* libssh returns SSH_AUTH_AGAIN while a call is pending (slow relayed links);
 * the *same* call must be repeated until it completes, otherwise the next API
 * fails with "Wrong state during pending SSH call". */
#define AUTH_RETRY(expr) ({ int _rc; int _n = 0; do { _rc = (expr); if (_rc == SSH_AUTH_AGAIN) vTaskDelay(pdMS_TO_TICKS(100)); } while (_rc == SSH_AUTH_AGAIN && ++_n < 600 && !c->stop); _rc; })

static int authenticate(ssh_client_t *c)
{
    int rc;
    rc = AUTH_RETRY(ssh_userauth_none(c->session, NULL));
    if (rc == SSH_AUTH_SUCCESS) return 0;
    int methods = ssh_userauth_list(c->session, NULL);
    if (c->p.use_key) {
        /* Empty path, "device" or a path that does not exist selects the Tab5's own key (NVS); an existing file is loaded as before. */
        struct stat st;
        bool dev = !c->p.keypath[0] || strcmp(c->p.keypath, "device") == 0 ||
                   (stat(c->p.keypath, &st) != 0 && errno == ENOENT);
        ssh_key key = NULL;
        if (dev) {
            rc = (device_key_ensure() == ESP_OK && device_key_get(&key) == ESP_OK) ? SSH_OK : SSH_ERROR;
            if (rc != SSH_OK) snprintf(c->err, sizeof(c->err), "Device key unavailable");
        } else {
            rc = ssh_pki_import_privkey_file(c->p.keypath, c->p.pass[0] ? c->p.pass : NULL, NULL, NULL, &key);
            if (rc != SSH_OK) snprintf(c->err, sizeof(c->err), "Key import failed: %s", ssh_get_error(c->session));
        }
        if (rc == SSH_OK) {
            rc = AUTH_RETRY(ssh_userauth_publickey(c->session, NULL, key));
            ssh_key_free(key);
            if (rc == SSH_AUTH_SUCCESS) return 0;
            ESP_LOGW(TAG, "publickey auth failed: %s", ssh_get_error(c->session));
        } else {
            ESP_LOGW(TAG, "%s", c->err);
            return -1;
        }
    }
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
    if (!c->session) { set_state(c, SSH_ST_ERROR, "Failed to create SSH session"); goto done; }
    if (!tailscale_enabled() && ts_host_needs_tailscale(c->p.host)) {
        /* the switch is off: fail at once, never start Tailscale on our own (spec Goal 5) */
        ESP_LOGW(TAG, "%s needs Tailscale, which is off", c->p.host);
        set_state(c, SSH_ST_ERROR, TS_OFF_MESSAGE);
        goto done;
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
    if (ssh_connect(c->session) != SSH_OK) {
        char m[160]; snprintf(m, sizeof(m), "Connect failed: %s", ssh_get_error(c->session));
        set_state(c, SSH_ST_ERROR, m); goto done;
    }
    if (c->stop) goto done;
    ESP_LOGI(TAG, "connected; checking host key");
    int hv = verify_host(c);
    if (hv != 0) { set_state(c, SSH_ST_ERROR, hv == -2 ? "Can't read the saved host key; not connecting" : "Host key rejected"); goto done; }
    set_state(c, SSH_ST_AUTH, NULL);
    if (authenticate(c) != 0) {
        char m[160]; snprintf(m, sizeof(m), "Authentication failed: %s", ssh_get_error(c->session));
        set_state(c, SSH_ST_ERROR, m); goto done;
    }
    c->channel = ssh_channel_new(c->session);
    if (!c->channel || ssh_channel_open_session(c->channel) != SSH_OK) { set_state(c, SSH_ST_ERROR, "Failed to create SSH channel"); goto done; }
    ssh_channel_request_pty_size(c->channel, "xterm-256color", c->p.cols, c->p.rows);
    if (c->p.cmd[0]) {
        /* An explicit command on a pty: never a login shell (it would start the user's tmux). */
        if (ssh_channel_request_exec(c->channel, c->p.cmd) != SSH_OK) { set_state(c, SSH_ST_ERROR, "Command request failed"); goto done; }
    } else if (ssh_channel_request_shell(c->channel) != SSH_OK) { set_state(c, SSH_ST_ERROR, "Shell request failed"); goto done; }
    set_state(c, SSH_ST_CONNECTED, NULL);
    ssh_set_blocking(c->session, 0);

    uint8_t *buf = malloc(4096);
    while (!c->stop && ssh_channel_is_open(c->channel) && !ssh_channel_is_eof(c->channel)) {
        bool did = false;
        /* outgoing */
        size_t n = 0;
        uint8_t *item = xRingbufferReceiveUpTo(c->tx, &n, 0, 1024);
        if (item) {
            size_t off = 0;
            while (off < n && !c->stop) {
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
        if (r > 0) { if (c->data_cb) c->data_cb(buf, r, c->user); did = true; }
        else if (r == SSH_ERROR) break;
        int r2 = ssh_channel_read_nonblocking(c->channel, buf, 4096, 1);
        if (r2 > 0) { if (c->data_cb) c->data_cb(buf, r2, c->user); did = true; }
        if (!did) {
            /* wait for socket activity or tx data */
            ssh_channel chans[2] = { c->channel, NULL }, outch[2];
            struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
            ssh_channel_select(chans, NULL, NULL, &tv);
            (void)outch;
        }
    }
    free(buf);
    if (c->p.cmd[0] && !c->stop) {
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
    set_state(c, SSH_ST_CLOSED, c->stop || c->p.cmd[0] ? NULL : "Server closed this connection");
done:
    if (c->channel) { ssh_channel_send_eof(c->channel); ssh_channel_close(c->channel); ssh_channel_free(c->channel); }
    if (c->session) { ssh_disconnect(c->session); ssh_free(c->session); }
    /* wait for owner to release */
    while (!c->stop) vTaskDelay(pdMS_TO_TICKS(50));
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

void ssh_client_stop(ssh_client_t *c)
{
    if (!c) return;
    c->state_cb = NULL; c->data_cb = NULL; c->hostkey_cb = NULL;
    c->stop = true;
}

ssh_state_t ssh_client_state(ssh_client_t *c) { return c ? c->state : SSH_ST_IDLE; }
const char *ssh_client_error(ssh_client_t *c) { return c ? c->err : ""; }
int ssh_client_exit_status(ssh_client_t *c) { return c ? c->exit_status : -1; }
