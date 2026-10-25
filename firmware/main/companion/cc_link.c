#include "cc_link.h"
#include "sys/boot_phase.h"
#include "net/fast_rules.h"
#include "cc_cmd.h"
#include "cc_watch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "libssh/libssh.h"
#include "cJSON.h"
#include "settings/settings.h"
#include "servers/server_store.h"
#include "servers/host_rules.h"
#include "net/tailscale.h"
#include "net/wifi_mgr.h"
#include "ssh/ssh_client.h"
#include "ssh/device_key.h"
#include "net/ts_host.h"
#include "companion/kh_carry.h"
#include "companion/cc_park.h"
#include "sys/link_guard.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

/* Threading: s_task is the only task that calls libssh on s_sess. cc_link_exec and
 * cc_link_paste queue a request and block on a semaphore until the task answers, so
 * console and UI callers run nothing from libssh on their own (small) stacks. The task
 * serves requests between polls; while there is no session it answers them at once
 * with ESP_ERR_INVALID_STATE. Every wait inside the task is bounded by a deadline, so a
 * caller can never wait forever. s_lock guards the small shared state (status, the last
 * session list, the subscribers and the two settings strings). */

static const char *TAG = "cc_link";

#define POLL_CAP        128            /* parse keeps active sessions first, so none fall off */
#define JSON_CAP        (256 * 1024)   /* `--all` keeps every finished session: room to grow */
#define SCREEN_CAP      (16 * 1024)
#define POLL_TIMEOUT_MS 15000
#define PASTE_BUDGET_MS 15000
#define MAX_SUBS        4
#define DEFAULT_CLAUDE  "~/.local/bin/claude"
#define STDIN_CHUNK     (16 * 1024)    /* cc_link_transcribe writes stdin in chunks this big, draining between them */
/* ... each as ssh_channel_write calls of at most this much: the port's libssh caps a buffer at 16384 bytes
   (BUFFER_SIZE_MAX, rounded up to a power of two), so a 16 KB data packet plus its header and MAC doesn't fit
   ("channel_write_common: Out of memory"); 8 KB plus the packet overhead does, for any cipher. */
#define SSH_WRITE_MAX   (8 * 1024)
#define VOICE_CHECK_MS  10000

typedef enum { LS_NO_MAC, LS_CONNECTING, LS_WAITING, LS_CONNECTED } link_state_t;
typedef enum { REQ_EXEC, REQ_PASTE, REQ_CHECK, REQ_POLL } req_type_t;

typedef struct {
    req_type_t type;
    const char *cmd;            /* exec command or attach command */
    const char *id;             /* paste: short session id */
    char *out; size_t cap;      /* exec stdout */
    int *exit_status;
    char *err; size_t err_cap;
    const char *payload; size_t payload_len;   /* paste bytes (ends with CR); exec: written to stdin, then EOF */
    int64_t exec_us, eof_us;    /* exec with a payload: when the exec was requested and the EOF sent (0 = not) */
    bool keep_tail;             /* exec: on overflow keep the newest bytes, not the first */
    int64_t deadline_us;
    esp_err_t result;
    SemaphoreHandle_t done;
} req_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;          /* req_t *; NULL = wake up */
static TaskHandle_t s_task;
static ssh_session s_sess;             /* only touched by s_task */
static volatile link_state_t s_state = LS_NO_MAC;
static char s_shown[48];               /* server name for the status line */
static int64_t s_retry_at_us;
static volatile bool s_drop, s_reconfig, s_reset_backoff;
static volatile int s_rate = 3;
static cc_session_t *s_list;           /* last successful poll */
static int s_list_n = -1;
static struct { cc_sessions_cb_t cb; void *ctx; } s_subs[MAX_SUBS];
static char *s_json;                   /* poll buffer (PSRAM) */
static char *s_screen;                 /* paste screen buffer (PSRAM) */
static cc_session_t *s_parse;          /* task only: the newest parse (PSRAM) */
static cc_link_fail_t s_fail;           /* why the last connect failed, other than reachability */
static char s_fail_text[160];          /* its text for the status line */
static int s_poll_fails;               /* consecutive failed refreshes while connected */
static bool s_poll_too_big;            /* the last refresh failed: output over JSON_CAP */
static volatile int s_floor;           /* cc_poll_set_floor: overrides s_rate when > 0 */
static uint32_t s_gen;                 /* successful results published (blocks, heartbeats, polls) */
static volatile int s_busy;            /* pastes and live checks in flight */
/* Forget host key (cc_link_forget_host_key), all under s_lock: */
static char s_pin_host[128];           /* the address the link pins: the one its last attempt used
                                          for the current server ("" after a server change) */
static int s_pin_port;
/* Requests made, acked by the task (torn down), and done (a connect after the ack pinned a key).
   A forget stays pending (done < req) past its caller's timeout, until such a connect. */
static uint32_t s_forget_req, s_forget_ack, s_forget_done;
static char s_forget_fp[80];           /* the key that connect pinned */
static char s_forget_expect[80];       /* a trust from the screen: the only key it pins ("" for a forget) */
static char s_forget_entry[128];       /* the trust's Host entry host string: another Host or an edit ends it (I2) */
static int64_t s_forget_deadline_us;   /* the trust expires then (KH_TRUST_TTL_MS, I2) */
static uint32_t s_forget_cancelled;    /* the request a cancel ended: its waiter gets ESP_ERR_INVALID_RESPONSE */
static uint32_t s_forget_pins;         /* forgets and trusts that pinned a key (cc_link_forget_done) */
static char s_unknown_fp[80];          /* the unknown key the last connect refused (cc_link_unknown_host_key) */
static volatile bool s_forget;         /* the task tears down and reconnects now */
/* The pin carry-over's source (fix round 1, M1): the key the link last met on the Host entry's
   tailnet address, with the entry's host string at the time, in NVS ("cc"/"carry"), under s_lock.
   It is the key itself, not a pointer to the address-keyed pin, which the terminal can rewrite for
   another device; and it stops counting once the entry's host is edited anywhere but
   cc_link_set_server_host. host[0] == 0: none. */
typedef struct { char host[128]; char ip[16]; uint8_t hash[32]; } carry_t;
static carry_t s_carry;
static hc_cache_t s_hc;   /* the host cache (fast check-in) */
static char s_lan_last[16];            /* the last LAN address the Host's name resolved to (M2) */
static void carry_store(const carry_t *c);
#define PIN_TARGETS 8
static int pin_targets(char hosts[PIN_TARGETS][128], int ports[PIN_TARGETS], bool resolve);

static const int s_backoff[] = { 1, 2, 4, 8, 16, 30 };

/* Task 8 */
static volatile bool s_parked;              /* cc_park_on(&s_park): no session, no reconnect */
static cc_park_t s_park;                    /* under s_lock: Radio off and the close-before-down guard (C1) */
static volatile bool s_in_connect;          /* the task is inside connect_once (I1) */
static volatile bool s_have_sess;           /* s_sess != NULL, for other tasks (I1) */
static volatile uint32_t s_host_gen;        /* cc_link_set_server_host's moves: a connect in flight doesn't
                                               overwrite the carry source moved meanwhile (review M4) */
static char s_ok_host[128];                 /* under s_lock: the last address whose key the link verified and
                                               authenticated on: the only take-over target (review M6) */
static volatile bool s_kick = true;         /* companion kick on|off: the stale-peer fix */
#define RLOG_N 8
typedef struct { uint32_t epoch; int64_t mono_ms; bool up; char why[24]; } rlog_t;
static rlog_t s_rlog[RLOG_N];               /* under s_lock */
static int s_rlog_n, s_rlog_head;

/* ---------- long-poll watch state ----------
 * One pty exec channel runs cc_cmd_watch's loop on the Host for as long as the link is up and
 * the effective interval is > 0. Its output is parsed into s_wbody (its own buffer, so the
 * refresh() calls of a paste or live check never overwrite a half-read block). The task owns
 * every field; the few the status line shows are copied under s_lock (s_wv). */
#define WATCH_HEARTBEAT_S   60
#define WATCH_QUIET_S       (3 * CC_WATCH_KEEPALIVE_S)
#define WATCH_LIFE_S        1800
#define WATCH_BANNER_MS     10000
#define WATCH_RETRY_US      (10 * 60 * 1000000LL)   /* after a fallback */
#define WATCH_ENDS_WINDOW_US (5 * 60 * 1000000LL)  /* 3 unasked ends in this long: fall back */
#define WATCH_WAIT_MS       250                     /* queue wait between channel reads */

typedef enum { WM_OFF, WM_STARTING, WM_ON, WM_FALLBACK, WM_PAUSED } watch_mode_t;

static volatile bool s_watch_want = true;   /* companion watch on|off */
static volatile bool s_watch_reset;         /* on: forget a fallback and retry now */
static volatile int s_banner_ms = WATCH_BANNER_MS;   /* next start only (test-fallback: 1) */
static ssh_channel s_wch;                   /* task only */
static cc_watch_t s_w;                      /* task only */
static char *s_wbody;                       /* s_w's body (PSRAM, JSON_CAP) */
static int s_w_interval;                    /* the running channel's I */
static int s_w_banner_ms;                   /* the running start's banner timeout */
static int64_t s_w_opened_us, s_w_live_us, s_w_ka_us;
static bool s_w_good;                       /* the channel's last block parsed (rc 0) */
static bool s_w_bad;                        /* ... failed (rc != 0, overflow, not JSON) */
static int64_t s_w_fallback_until_us;       /* fallback: retry the watch after this */
static int64_t s_w_ends[3];                 /* times of the last unasked channel ends */
static struct {                             /* for cc_watch_status_text, under s_lock */
    watch_mode_t mode;
    int interval;
    int64_t block_us, hb_us;
    char why[64];
} s_wv;

/* Before cc_link_start (or if it failed) there is no lock and no link task: getters then
   read the settings directly and the status is "no Host set up". */
static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* Fix round 1 (I2), under s_lock. A pending forget or trust: asked for and torn down for, not yet done. */
static bool forget_pending_locked(void) { return s_forget_done != s_forget_req && s_forget_ack == s_forget_req; }
static bool trust_pending_locked(void) { return s_forget_done != s_forget_req && s_forget_expect[0]; }
/* Ends a pending forget or trust without pinning anything; its waiter gets ESP_ERR_INVALID_RESPONSE. */
static void forget_cancel_locked(void)
{
    if (s_forget_done == s_forget_req) return;
    s_forget_cancelled = s_forget_req;
    s_forget_done = s_forget_req;
    s_forget_expect[0] = 0;
    s_forget_entry[0] = 0;
    s_forget_deadline_us = 0;
}
/* trust_only: a plain forget is left pending (it erased the pins itself; a Host switch ends both). */
static void forget_cancel(bool trust_only, const char *why)
{
    lock();
    bool had = trust_only ? trust_pending_locked() : s_forget_done != s_forget_req;
    if (had) forget_cancel_locked();
    unlock();
    if (had) ESP_LOGW(TAG, "%s: cancelled (%s)", trust_only ? "trust host key" : "forget host key", why);
}
static int64_t now_us(void) { return esp_timer_get_time(); }
static int64_t s_last_ok_ms;   /* esp_timer ms of the last successful result, 0 = none */

static void rlog_add(bool up, const char *why)
{
    lock();
    rlog_t *r = &s_rlog[s_rlog_head];
    r->epoch = (uint32_t)time(NULL);
    r->mono_ms = now_us() / 1000;
    r->up = up;
    strlcpy(r->why, why ? why : "", sizeof(r->why));
    s_rlog_head = (s_rlog_head + 1) % RLOG_N;
    if (s_rlog_n < RLOG_N) s_rlog_n++;
    unlock();
}

static void kick(void)
{
    void *none = NULL;
    if (s_queue) xQueueSend(s_queue, &none, 0);
}

static void set_state(link_state_t st)
{
    lock();
    s_state = st;
    unlock();
}

/* ---------- settings ---------- */

const char *cc_settings_claude_path(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, cc_path_valid(g_settings.claude_path) ? g_settings.claude_path : DEFAULT_CLAUDE, cap);
    unlock();
    return out;
}

const char *cc_settings_whisper_path(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, cc_path_valid(g_settings.whisper_path) ? g_settings.whisper_path : CC_WHISPER_DEFAULT, cap);
    unlock();
    return out;
}

const char *cc_settings_whisper_model(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, cc_path_valid(g_settings.whisper_model) ? g_settings.whisper_model : CC_WHISPER_MODEL_DEFAULT, cap);
    unlock();
    return out;
}

const char *cc_settings_server_name(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, g_settings.companion_server, cap);
    unlock();
    return out;
}

static int server_index(const char *name)
{
    if (!name || !name[0]) return -1;
    int n = server_store_count();
    for (int i = 0; i < n; i++) {
        const server_t *s = server_store_get(i);
        if (s && strcmp(s->name, name) == 0) return i;
    }
    return -1;
}

int cc_settings_server(void)
{
    char name[sizeof(g_settings.companion_server)];
    lock();
    strlcpy(name, g_settings.companion_server, sizeof(name));
    unlock();
    return server_index(name);
}

bool cc_link_is_companion_server(int idx)
{
    const server_t *s = server_store_get(idx);
    if (!s) return false;
    int ci = cc_settings_server();
    if (ci >= 0 && idx == ci) return true;
    /* C1 (hard rule 1): every address the link knows the Host by, and the entry's name after a MagicDNS lookup */
    hf_host_addrs_t *h = malloc(sizeof(*h));
    if (!h) return true;   /* fail closed: no shell when it can't tell */
    cc_link_host_addresses(h);
    char a[128];
    bool host = hf_is_host(s->host, h) || (tailscale_resolve(s->host, a, sizeof(a)) && hf_is_host(a, h));
    free(h);
    return host;
}

/* P4C-10 migration: the keys pinned for the Host's addresses (and the carry key) join the deny-list, so it is never
   empty while a pin exists. Idempotent (a known key only moves to the newest); runs at every start. */
static void host_keys_migrate(void)
{
    int ci = cc_settings_server();
    const server_t *c = ci >= 0 ? server_store_get(ci) : NULL;
    if (c) {
        const char *addrs[] = { c->host, g_settings.mac_lan_ip, g_settings.mac_ts_ip, s_carry.ip, s_hc.ip };
        for (unsigned i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            uint8_t h[32];
            if (addrs[i][0] && ssh_known_host_hash(addrs[i], c->port ? c->port : 22, h) == KH_PIN_FOUND) ssh_host_keys_add(h);
        }
    }
    if (s_carry.host[0]) ssh_host_keys_add(s_carry.hash);
    uint8_t l[HF_HOSTKEYS_MAX][32];
    int n = 0;
    hf_deny_read_t r = ssh_host_keys_load(l, &n);
    ESP_LOGI(TAG, "host keys: %d known for the Host%s", n, r == HF_DENY_ERROR ? " (the list can't be read)" : "");   /* a count only */
}

void cc_link_host_addresses(hf_host_addrs_t *h)
{
    hf_host_addrs_init(h);
    int ci = cc_settings_server();
    const server_t *c = ci >= 0 ? server_store_get(ci) : NULL;
    if (!c) return;   /* no Host set: nothing is the Host */
    char ip[128];
    hf_host_addrs_add_name(h, c->host);   /* the host and its first label ("studio" matches studio.<tailnet>.ts.net) */
    if (tailscale_resolve(c->host, ip, sizeof(ip))) hf_host_addrs_add(h, ip);
    char label[64];
    if (hf_first_label(c->host, label, sizeof(label)) && tailscale_resolve(label, ip, sizeof(ip)))
        hf_host_addrs_add(h, ip);   /* the peer by its machine name */
    lock();
    hf_host_addrs_add(h, g_settings.mac_lan_ip);
    hf_host_addrs_add(h, g_settings.mac_ts_ip);
    hf_host_addrs_add(h, s_carry.ip);
    hf_host_addrs_add(h, s_ok_host);
    hf_host_addrs_add(h, s_pin_host);
    hf_host_addrs_add(h, s_lan_last);
    if (cc_same_host(s_hc.entry, c->host)) hf_host_addrs_add(h, s_hc.ip);
    unlock();
}

bool cc_link_rename_server(const char *old_name, const char *new_name)
{
    if (!old_name || !new_name || !new_name[0]) return false;
    lock();
    bool ok = !strcmp(g_settings.companion_server, old_name);
    if (ok) strlcpy(g_settings.companion_server, new_name, sizeof(g_settings.companion_server));
    unlock();
    if (ok) settings_save();   /* the same entry and host string: the pin carry-over stays (L3 is for another Host) */
    return ok;
}

void cc_link_set_server(const char *name)
{
    lock();
    bool other = strcmp(g_settings.companion_server, name ? name : "") != 0;
    strlcpy(g_settings.companion_server, name ? name : "", sizeof(g_settings.companion_server));
    s_pin_host[0] = 0;   /* the old Host's address: a forget must not erase its pin */
    s_ok_host[0] = 0;    /* M6 */
    if (other) s_lan_last[0] = 0;
    if (other) forget_cancel_locked();   /* I2: a forget or trust was for the old Host, never the new one */
    unlock();
    if (other) carry_store(NULL);   /* another Host: the old one's key must not carry over (L3) */
    settings_save();
    s_reconfig = true;
    kick();
}

bool cc_link_set_claude_path(const char *path)
{
    if (!cc_path_valid(path)) return false;
    lock();
    strlcpy(g_settings.claude_path, path, sizeof(g_settings.claude_path));
    unlock();
    settings_save();
    return true;
}

static bool set_voice_path(char *field, size_t cap, const char *path)
{
    if (!cc_path_valid(path)) return false;
    lock();
    strlcpy(field, path, cap);
    unlock();
    settings_save();
    return true;
}

bool cc_link_set_whisper_path(const char *path)
{
    return set_voice_path(g_settings.whisper_path, sizeof(g_settings.whisper_path), path);
}

bool cc_link_set_whisper_model(const char *path)
{
    return set_voice_path(g_settings.whisper_model, sizeof(g_settings.whisper_model), path);
}

/* ---------- status ---------- */

bool cc_link_connected(void)
{
    return s_state == LS_CONNECTED;
}

const char *cc_link_status_text(char *b, size_t cap)
{
    if (!b || !cap) return "";
    if (!s_task) {
        strlcpy(b, "no Host set up", cap);
        return b;
    }
    lock();
    if (s_parked && s_state != LS_CONNECTED && s_state != LS_NO_MAC) {
        snprintf(b, cap, "parked (radio off): %s", s_shown);
        unlock();
        return b;
    }
    switch (s_state) {
    case LS_NO_MAC:
        strlcpy(b, "no Host set up", cap);
        break;
    case LS_CONNECTING:
        if (s_fail != CC_LINK_FAIL_NONE) snprintf(b, cap, "%s (retrying now)", s_fail_text);
        else snprintf(b, cap, "connecting to %s", s_shown);
        break;
    case LS_WAITING: {
        int64_t left = s_retry_at_us - now_us();
        int sec = left > 0 ? (int)((left + 999999) / 1000000) : 0;
        if (s_fail != CC_LINK_FAIL_NONE)
            snprintf(b, cap, "%s (retry in %ds)", s_fail_text, sec);
        else
            snprintf(b, cap, "connecting to %s (retry in %ds)", s_shown, sec);
        break;
    }
    case LS_CONNECTED:
        if (s_poll_too_big)
            snprintf(b, cap, "connected %s; session list over %d KB, not updating", s_shown, JSON_CAP / 1024);
        else if (s_poll_fails >= CC_POLL_FAILS_SHOWN)
            snprintf(b, cap, "connected %s; session list not updating (%d polls failed)", s_shown, s_poll_fails);
        else
            snprintf(b, cap, "connected %s", s_shown);
        break;
    }
    unlock();
    return b;
}

cc_link_fail_t cc_link_failure(void)
{
    lock();
    cc_link_fail_t f = s_state == LS_CONNECTED ? CC_LINK_FAIL_NONE : s_fail;
    unlock();
    return f;
}

int cc_poll_failures(bool *too_big)
{
    lock();
    int n = s_state == LS_CONNECTED ? s_poll_fails : 0;
    if (too_big) *too_big = n > 0 && s_poll_too_big;
    unlock();
    return n;
}

/* ---------- session lifecycle (task only) ---------- */

static void answer(req_t *r, esp_err_t e)
{
    r->result = e;
    xSemaphoreGive(r->done);
}

/* Fail everything queued; returns true if a wake-up asked for an early retry. */
static bool drain_queue_down(TickType_t wait)
{
    req_t *r;
    bool woke = false;
    while (xQueueReceive(s_queue, &r, wait) == pdTRUE) {
        if (r) answer(r, ESP_ERR_INVALID_STATE);
        else woke = true;
        wait = 0;
    }
    return woke;
}

static void watch_close(bool asked);

static const char *s_down_why = "dropped";   /* task only: why the next teardown happens */

static void teardown(void)
{
    watch_close(true);   /* the Tab5's own close: never counted toward a fallback */
    if (s_sess) rlog_add(false, s_down_why);
    s_down_why = "dropped";
    if (s_sess) {
        ssh_disconnect(s_sess);
        ssh_free(s_sess);
        s_sess = NULL;
        s_have_sess = false;
        ESP_LOGI(TAG, "link closed");
    }
    s_drop = false;
}

/* Records why a connect attempt failed (CC_LINK_FAIL_NONE: reachability, or success). */
static void set_fail(cc_link_fail_t f, const char *text)
{
    lock();
    s_fail = f;
    strlcpy(s_fail_text, f != CC_LINK_FAIL_NONE && text ? text : "", sizeof(s_fail_text));
    s_unknown_fp[0] = 0;   /* only set_fail_unknown sets it */
    unlock();
}
/* The refusal of an unknown key: the failure and its fingerprint under one lock, so no reader sees one without
   the other (fix round 1, M4). */
static void set_fail_unknown(const char *text, const char *fp)
{
    lock();
    s_fail = CC_LINK_FAIL_HOSTKEY;
    strlcpy(s_fail_text, text, sizeof(s_fail_text));
    strlcpy(s_unknown_fp, fp, sizeof(s_unknown_fp));
    unlock();
}

/* Plan 3 Task 3 Step 4: the Host's last looked-up address and the key met there, NVS "cc"/"hcache" only (L44:
   not also in the hib record; an NVS read is about 1 ms), under s_lock. The link connects
   to it first and looks the name up only when that connect fails or its pin doesn't match (fast_rules). */
static bool s_hc_off;   /* `companion hostcache off` (measurement), RAM only */

static void hc_load(void)
{
    void *b = NULL; size_t n = 0;
    memset(&s_hc, 0, sizeof(s_hc));
    if (nvs_get_blob_alloc("cc", "hcache", &b, &n)) {
        if (n == sizeof(s_hc)) memcpy(&s_hc, b, n);
        free(b);
    }
    s_hc.entry[sizeof(s_hc.entry) - 1] = 0;
    s_hc.ip[sizeof(s_hc.ip) - 1] = 0;
}

static void carry_load(void)
{
    void *b = NULL; size_t n = 0;
    if (nvs_get_blob_alloc("cc", "carry", &b, &n)) {
        if (n == sizeof(carry_t)) memcpy(&s_carry, b, n);
        free(b);
    }
    s_carry.host[sizeof(s_carry.host) - 1] = 0;
    s_carry.ip[sizeof(s_carry.ip) - 1] = 0;
}

/* Saves the carry source (NULL clears it) and keeps g_settings.mac_ts_ip, its address, in step. */
static void carry_store(const carry_t *c)
{
    carry_t z = { 0 };
    if (!c) c = &z;
    lock();
    bool same = memcmp(&s_carry, c, sizeof(*c)) == 0;
    s_carry = *c;
    bool ip_new = strcmp(g_settings.mac_ts_ip, c->ip) != 0;
    if (ip_new) strlcpy(g_settings.mac_ts_ip, c->ip, sizeof(g_settings.mac_ts_ip));
    unlock();
    if (!same) {
        if (c->host[0]) nvs_set_blob_ns("cc", "carry", c, sizeof(*c));
        else nvs_erase_key_ns("cc", "carry");
    }
    if (ip_new) settings_save();
    if (c->host[0]) forget_cancel(true, "a carry source appeared");   /* I2 */
}

/* Whether the pending trust may still be applied (I2): kh_trust_allowed over every pin target's pin (no lookup),
   the carry source, the entry the trust was asked for and its deadline. The link task, outside s_lock. */
static bool trust_still_allowed(const char *entry_host)
{
    char hosts[PIN_TARGETS][128]; int ports[PIN_TARGETS];
    kh_pin_t pins[PIN_TARGETS];
    int n = pin_targets(hosts, ports, false);
    uint8_t h[32];
    for (int i = 0; i < n; i++) pins[i] = ssh_known_host_hash(hosts[i], ports[i], h);
    lock();
    bool carry = s_carry.host[0] && strcmp(s_carry.host, entry_host) == 0;
    bool same = strcmp(s_forget_entry, entry_host) == 0;
    bool expired = now_us() >= s_forget_deadline_us;
    unlock();
    return kh_trust_allowed(pins, n, carry, same, expired);
}

/* A pin the connect just wrote (KHA_PIN_NEW, KHA_PIN_SILENT): its key joins the P4C-10 deny-list now, before the
   device key's auth (which can still fail on a first run), so a Terminal entry reaching the Host by an alias is
   refused from this moment (fix round 1, I1). A pin by another path than the trust ends a pending trust (I2). */
static void pinned_now(kh_result_t kh, const uint8_t *met, ssh_session s, bool by_forget)
{
    if (kh != KH_OK) return;
    if (met) { ssh_host_keys_add(met); ssh_host_key_type_note(s); }
    if (!by_forget) forget_cancel(true, "a pin appeared");
}

/* The Host's address (L2): a literal IPv4 as it is; a tailnet name through MagicDNS (Tailscale up);
   any other name through getaddrinfo, which asks mDNS for a .local name
   (CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES) and DNS for the rest; failing that, the fallback LAN
   address from the settings. *how gets "ip", "magicdns", "mdns", "dns" or "fallback". */
static bool mac_resolve(const char *host, char *out, size_t cap, const char **how)
{
    *how = "ip";
    if (ts_host_is_ipv4(host)) { strlcpy(out, host, cap); return true; }
    *how = "magicdns";
    if (tailscale_resolve(host, out, cap)) return true;
    if (!ts_host_needs_tailscale(host)) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
            char b[16];
            bool ok = inet_ntop(AF_INET, &sa->sin_addr, b, sizeof(b)) != NULL;
            freeaddrinfo(res);
            if (ok) {
                strlcpy(out, b, cap);
                *how = ts_host_is_mdns(host) ? "mdns" : "dns";
                return true;
            }
        } else if (res) freeaddrinfo(res);
    }
    char fb[sizeof(g_settings.mac_lan_ip)];
    lock();
    strlcpy(fb, g_settings.mac_lan_ip, sizeof(fb));
    unlock();
    *how = "fallback";
    if (fb[0] && ts_host_is_ipv4(fb)) { strlcpy(out, fb, cap); return true; }
    return false;
}

/* I1: parked while a connect runs (a hibernate, a Wi-Fi leave, a restart): the session is closed here,
   while the Wi-Fi still carries the close, and never handed to the loop. */
static bool parked_now(void)
{
    lock();
    bool p = !cc_park_keep_new_session(&s_park);
    unlock();
    return p;
}

static bool connect_once(const char *name)
{
    char why[sizeof(s_fail_text)];
    uint32_t gen = s_host_gen;
    int idx = server_index(name);
    const server_t *sv = idx >= 0 ? server_store_get(idx) : NULL;
    if (!sv) {
        ESP_LOGW(TAG, "no server named '%s' in the server list", name);
        snprintf(why, sizeof(why), "no Host entry named '%s' (Settings \xe2\x80\xba Host)", name);
        set_fail(CC_LINK_FAIL_NO_ENTRY, why);
        return false;
    }
    char host[128], user[64];
    int port = sv->port > 0 ? sv->port : 22;
    strlcpy(host, sv->host, sizeof(host));
    strlcpy(user, sv->user, sizeof(user));
    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "no Wi-Fi");
        set_fail(CC_LINK_FAIL_NONE, NULL);
        return false;
    }
    if (!tailscale_enabled() && ts_host_needs_tailscale(host)) {
        /* the switch is off: the link never starts Tailscale on its own (spec Goal 5) */
        ESP_LOGW(TAG, "%s is a tailnet host and Tailscale is off; not connecting", host);
        set_fail(CC_LINK_FAIL_TS_OFF, CC_LINK_TS_OFF_TEXT);
        return false;
    }
    char ip[16];
    const char *how;
    char entry_host[sizeof(host)];
    strlcpy(entry_host, host, sizeof(entry_host));
    hc_cache_t hc;   /* review m5: a copy taken under the lock (the console's debug commands write it too) */
    lock();
    bool fpend = s_forget_done != s_forget_req;   /* a forget asked for or pending: always look the name up */
    hc = s_hc;
    unlock();
    bool from_cache = !s_hc_off && hc_use(&hc, entry_host, tailscale_enabled(), fpend);
    char dead[16] = "";   /* the cached address that didn't answer: a lookup that gives it again fails at once */
    if (from_cache) {
        strlcpy(ip, hc.ip, sizeof(ip));
        how = "cache";
    }
lookup:
    if (!from_cache && !mac_resolve(entry_host, ip, sizeof(ip), &how)) {
        ESP_LOGW(TAG, "host: %s doesn't resolve and there is no fallback address", entry_host);
        snprintf(why, sizeof(why), "Host not found at %s (set its fallback address in Settings \xe2\x80\xba Host)", entry_host);
        set_fail(CC_LINK_FAIL_NOT_FOUND, why);   /* I2; retried with the backoff */
        return false;
    }
    if (dead[0] && strcmp(ip, dead) == 0) {
        ESP_LOGW(TAG, "host: %s -> %s (%s) again, which didn't answer", entry_host, ip, how);
        if (strcmp(how, "fallback") == 0) {
            snprintf(why, sizeof(why), "Host not at %s (fallback for %s)", ip, entry_host);
            set_fail(CC_LINK_FAIL_NOT_FOUND, why);
        } else set_fail(CC_LINK_FAIL_NONE, NULL);   /* unreachable */
        return false;
    }
    boot_phase_mark(BP_RESOLVED);
    bool via_fallback = strcmp(how, "fallback") == 0;
    if (strcmp(how, "ip") != 0) ESP_LOGI(TAG, "host: %s -> %s (%s)", entry_host, ip, how);
    if (strcmp(how, "ip") != 0 && strcmp(how, "magicdns") != 0) {
        lock();
        strlcpy(s_lan_last, ip, sizeof(s_lan_last));
        unlock();
    }
    strlcpy(host, ip, sizeof(host));
    if (tailscale_is_tailnet_ip(host) && !tailscale_enabled()) {
        ESP_LOGW(TAG, "%s is a tailnet address and Tailscale is off; not connecting", host);
        set_fail(CC_LINK_FAIL_TS_OFF, CC_LINK_TS_OFF_TEXT);
        return false;
    }
    if (tailscale_is_tailnet_ip(host) && !tailscale_prepare_peer(host, 20000))
        ESP_LOGW(TAG, "tunnel to %s not confirmed, trying anyway", host);

    /* a standing failure stays shown while the next attempt runs, so the line doesn't flicker */
    lock();
    strlcpy(s_pin_host, host, sizeof(s_pin_host));
    s_pin_port = port;
    unlock();
    if (parked_now()) return false;   /* I1: no new session once parked */
    ssh_session s = ssh_new();
    if (!s) {
        set_fail(CC_LINK_FAIL_NONE, NULL);   /* out of memory: no old reason may stick */
        return false;
    }
    long timeout = 10;
    int nodelay = 1, verb = SSH_LOG_NOLOG;
    ssh_options_set(s, SSH_OPTIONS_HOST, host);
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s, SSH_OPTIONS_USER, user);
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(s, SSH_OPTIONS_NODELAY, &nodelay);
    ssh_options_set(s, SSH_OPTIONS_COMPRESSION, "no");
    ssh_options_set(s, SSH_OPTIONS_LOG_VERBOSITY, &verb);
    if (ssh_connect(s) != SSH_OK) {
        ESP_LOGW(TAG, "connect %s:%d failed: %s", host, port, ssh_get_error(s));
        if (from_cache) {
            /* the cached address doesn't answer: look the name up now (Step 4) */
            ssh_free(s);
            from_cache = false;
            strlcpy(dead, host, sizeof(dead));
            ESP_LOGW(TAG, "host: cached %s for %s failed, looking it up", host, entry_host);
            lock();
            s_pin_host[0] = 0;
            unlock();
            goto lookup;
        }
        if (via_fallback) {
            /* the name didn't resolve and the fallback address doesn't answer: likely stale (I2) */
            snprintf(why, sizeof(why), "Host not at %s (fallback for %s)", host, entry_host);
            set_fail(CC_LINK_FAIL_NOT_FOUND, why);
        } else set_fail(CC_LINK_FAIL_NONE, NULL);   /* unreachable */
        ssh_free(s);
        /* the stale-peer fix: MicroLink can report a peer up on a dead keypair (the Host slept), so
           kick a fresh handshake now, before the backoff's next retry */
        if (s_kick && tailscale_is_tailnet_ip(host)) tailscale_kick_peer(host);
        return false;
    }
    boot_phase_mark(BP_KEX);
    if (parked_now()) {   /* I1: parked during the connect: close it now, before the Wi-Fi goes */
        ESP_LOGI(TAG, "parked during the connect: closing");
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    char fp[80];
    kh_result_t kh = ssh_known_host_check(s, host, port, fp, sizeof(fp));
    uint8_t met[32];
    bool have_met = ssh_server_key_hash(s, met);
    if (from_cache && hc_cached_key(kh, &hc, have_met ? met : NULL) == HC_LOOKUP) {
        /* the cached address has no matching pin, or its key isn't the one cached with it (another pinned
           machine on the Host's old address, review m2): nothing is pinned or refused on it; the lookup and
           its own check decide, exactly as without the cache (Step 4: the cache never weakens the pin) */
        ESP_LOGW(TAG, "host: cached %s for %s: key %s not its pin, looking it up", host, entry_host, fp);
        ssh_disconnect(s);
        ssh_free(s);
        from_cache = false;
        lock();
        s_pin_host[0] = 0;
        unlock();
        goto lookup;
    }
    bool tnet = tailscale_is_tailnet_ip(host);
    carry_t c;
    /* a pending trust is checked again here, not only when it was asked for (I2): it expires, it is for this
       entry only, and a pin or carry source that appeared meanwhile ends it */
    lock();
    bool trusting = forget_pending_locked() && s_forget_expect[0];
    unlock();
    if (trusting && !trust_still_allowed(entry_host)) forget_cancel(true, "no longer allowed at this connect");
    lock();
    /* the user's Forget host key: the next connect pins the key it meets as new; a trust from the screen only
       the key the user confirmed */
    bool forget_pending = kh_forget_applies(forget_pending_locked(), s_forget_expect, fp);
    c = s_carry;
    unlock();
    khs_t src = KHS_NONE;
    if (kh == KH_UNKNOWN && !tnet && !forget_pending && c.host[0]) {
        if (strcmp(c.host, entry_host) != 0) {
            ESP_LOGW(TAG, "kh: the carry source is for '%s', the entry is now '%s': not used", c.host, entry_host);
        } else {
            uint8_t h[32];
            src = !ssh_server_key_hash(s, h) ? KHS_ERROR : memcmp(h, c.hash, sizeof(h)) == 0 ? KHS_EQUAL : KHS_DIFFERENT;
        }
    }
    /* the whole decision is the pure kh_carry_action (host-tested); this only acts on it */
    switch (kh_carry_action(kh, tnet, forget_pending, src)) {
    case KHA_NORMAL:
        break;
    case KHA_PIN_NEW:
        /* first contact over the tailnet, or after the user's forget, with no screen to ask on */
        ESP_LOGW(TAG, "pinning new host key for %s:%d %s", host, port, fp);
        kh = ssh_known_host_trust(s, host, port) ? KH_OK : KH_ERROR;
        pinned_now(kh, have_met ? met : NULL, s, forget_pending);
        break;
    case KHA_PIN_SILENT:
        kh = ssh_known_host_trust(s, host, port) ? KH_OK : KH_ERROR;
        if (kh == KH_OK) ESP_LOGW(TAG, "kh: carried tailnet pin to %s (from %s) %s", host, c.ip, fp);
        pinned_now(kh, have_met ? met : NULL, s, false);
        break;
    case KHA_REFUSE_CHANGED:
        ESP_LOGE(TAG, "kh: %s's key %s differs from the tailnet key of %s", host, fp, c.ip);
        kh = KH_CHANGED;
        break;
    case KHA_PIN_ERROR:
        kh = KH_PIN_ERROR;
        break;
    case KHA_REFUSE_UNKNOWN:
        ESP_LOGE(TAG, "kh: new host key for %s:%d %s, and no tailnet key to carry; not connecting", host, port, fp);
        /* the fingerprint, to compare with ssh-keygen -lf on the Host before forgetting (M3) */
        snprintf(why, sizeof(why), "new host key %s for %s; check and trust it in Settings \xe2\x80\xba Host", fp, host);
        set_fail_unknown(why, fp);   /* Settings › Host offers to trust it (polish 1) */
        lock();
        s_pin_host[0] = 0;   /* M1 */
        s_ok_host[0] = 0;
        unlock();
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    if (kh == KH_CHANGED) {
        snprintf(why, sizeof(why), "host key changed for %s; not connecting", host);
        set_fail(CC_LINK_FAIL_HOSTKEY, why);
    } else if (kh == KH_PIN_ERROR) {
        /* never treated as unknown: re-pinning over a pin we can't read could accept any key */
        snprintf(why, sizeof(why), "can't read the host key pin for %s; not connecting", host);
        set_fail(CC_LINK_FAIL_HOSTKEY, why);
    } else if (kh != KH_OK) set_fail(CC_LINK_FAIL_NONE, NULL);   /* server key unreadable: retried as unreachable */
    if (kh != KH_OK) {
        lock();
        s_pin_host[0] = 0;   /* M1: never hand a refused address to a take-over */
        s_ok_host[0] = 0;
        unlock();
        ESP_LOGE(TAG, "host key %s for %s:%d (%s); not connecting",
                 kh == KH_CHANGED ? "CHANGED" : kh == KH_PIN_ERROR ? "pin unreadable" : "unreadable", host, port, fp);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    boot_phase_mark(BP_HOSTKEY);
    ssh_key key = NULL;
    int rc = SSH_AUTH_ERROR;
    if (device_key_ensure() == ESP_OK && device_key_get(&key) == ESP_OK) {
        int n = 0;
        do {
            rc = ssh_userauth_publickey(s, NULL, key);
            if (rc == SSH_AUTH_AGAIN) vTaskDelay(pdMS_TO_TICKS(100));
        } while (rc == SSH_AUTH_AGAIN && ++n < 200 && !parked_now());
        ssh_key_free(key);
    }
    if (rc != SSH_AUTH_SUCCESS && parked_now()) {   /* I1: parked while the auth was pending: not an auth failure */
        ESP_LOGI(TAG, "parked during the auth: closing");
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    if (rc != SSH_AUTH_SUCCESS) {
        ESP_LOGW(TAG, "device key auth failed: %s", ssh_get_error(s));
        snprintf(why, sizeof(why), "%s refused the device key", name);
        set_fail(CC_LINK_FAIL_AUTH, why);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    boot_phase_mark(BP_AUTH);
    if (parked_now()) {
        /* I1: parked while the auth ran (go_down's wait, a Wi-Fi leave): the session is closed here, while
           the Wi-Fi still carries the close, instead of being handed to the loop after the park was seen */
        ESP_LOGI(TAG, "parked during the auth: closing the new session");
        ssh_disconnect(s);
        ssh_free(s);
        set_fail(CC_LINK_FAIL_NONE, NULL);
        return false;
    }
    s_sess = s;
    s_have_sess = true;
    s_drop = false;
    set_fail(CC_LINK_FAIL_NONE, NULL);
    lock();
    /* a pending forget is done by the first connect after the task's teardown (a trust: one that met its key) */
    bool forget_done = kh_forget_applies(forget_pending_locked(), s_forget_expect, fp);
    if (forget_done) {
        s_forget_done = s_forget_req;
        s_forget_pins++;
        strlcpy(s_forget_fp, fp, sizeof(s_forget_fp));
        s_forget_expect[0] = 0;
        s_forget_entry[0] = 0;
        s_forget_deadline_us = 0;
    }
    s_poll_fails = 0;
    s_poll_too_big = false;
    strlcpy(s_ok_host, host, sizeof(s_ok_host));   /* M6: key checked and authenticated */
    bool moved = s_host_gen != gen;                /* M4: `companion server-host` ran meanwhile */
    unlock();
    if (have_met) { ssh_host_keys_add(met); ssh_host_key_type_note(s); }   /* P4C-10: the Host's key and its type */
    if (tnet && moved) ESP_LOGW(TAG, "kh: the Host entry moved during this connect; the carry source is kept");
    if (tnet && !moved) {
        /* remember the key met on the entry's tailnet address: it carries over when the entry moves to the LAN */
        carry_t nc = { 0 };
        strlcpy(nc.host, entry_host, sizeof(nc.host));
        strlcpy(nc.ip, host, sizeof(nc.ip));
        if (ssh_server_key_hash(s, nc.hash)) carry_store(&nc);
    }
    if (forget_done) ESP_LOGW(TAG, "forget host key: new host key pinned: %s", fp);
    if (!from_cache && have_met && strcmp(how, "ip") != 0 && strcmp(how, "magicdns") != 0 &&
        hc_on_resolved(&hc, entry_host, host, met)) {
        lock();
        s_hc = hc;
        unlock();
        nvs_set_blob_ns("cc", "hcache", &hc, sizeof(hc));
        ESP_LOGI(TAG, "host: cached %s for %s", host, entry_host);
    }
    s_w_fallback_until_us = 0;   /* a fallback is retried on the next connect */
    memset(s_w_ends, 0, sizeof(s_w_ends));
    ESP_LOGI(TAG, "connected to %s (%s:%d)", name, host, port);
    rlog_add(true, "connected");
    return true;
}

/* ---------- channels (task only) ---------- */

static void first_line(char *s)
{
    cc_strip_ansi(s);
    char *p = s;
    while (*p == '\n' || *p == '\r' || *p == ' ') p++;
    memmove(s, p, strlen(p) + 1);
    s[strcspn(s, "\r\n")] = 0;
}

static ssh_channel open_channel(void)
{
    ssh_channel ch = ssh_channel_new(s_sess);
    if (!ch) { s_drop = true; return NULL; }
    if (ssh_channel_open_session(ch) != SSH_OK) {
        ESP_LOGW(TAG, "channel open failed: %s", ssh_get_error(s_sess));
        ssh_channel_free(ch);
        s_drop = true;
        return NULL;
    }
    return ch;
}

static void close_channel(ssh_channel ch)
{
    if (!ch) return;
    if (ssh_channel_is_open(ch)) {
        ssh_channel_send_eof(ch);
        ssh_channel_close(ch);
    }
    ssh_channel_free(ch);
}

/* Appends stderr to err (up to err_cap), drops the rest. */
static void read_stderr(ssh_channel ch, char *err, size_t err_cap, size_t *elen)
{
    char tmp[256];
    int n;
    while ((n = ssh_channel_read_nonblocking(ch, tmp, sizeof(tmp), 1)) > 0) {
        if (err && err_cap > 1 && *elen < err_cap - 1) {
            size_t k = (size_t)n;
            if (k > err_cap - 1 - *elen) k = err_cap - 1 - *elen;
            memcpy(err + *elen, tmp, k);
            *elen += k;
            err[*elen] = 0;
        }
    }
}

/* Appends n bytes of stdout to r->out (keeping the first or, with keep_tail, the newest bytes). */
static void exec_take(req_t *r, char *tmp, size_t k, size_t *olen, bool *truncated)
{
    if (!r->out || r->cap <= 1) return;
    size_t room = r->cap - 1;
    if (r->keep_tail && k > room) { memmove(tmp, tmp + (k - room), room); k = room; *truncated = true; }
    if (*olen + k > room) {
        *truncated = true;
        if (r->keep_tail) {
            size_t drop = *olen + k - room;
            memmove(r->out, r->out + drop, *olen - drop);
            *olen -= drop;
        } else {
            k = room - *olen;
        }
    }
    memcpy(r->out + *olen, tmp, k);
    *olen += k;
    r->out[*olen] = 0;
}

static void watch_keepalive(void);

/* Writes r->payload to the channel's stdin in STDIN_CHUNK pieces, draining stdout and stderr between them (so the
   remote never blocks on a full pipe), then sends EOF. Stops at the deadline or a park. A write that fails is a
   failure unless the remote has already ended its side (its pre-check exited: the exit status still comes back). */
static void exec_write_stdin(req_t *r, ssh_channel ch, size_t *olen, size_t *elen, bool *truncated,
                             bool *failed, bool *timed_out, bool *parked)
{
    char tmp[512];
    size_t off = 0;
    while (off < r->payload_len) {
        if (now_us() > r->deadline_us) { *timed_out = true; return; }
        if (s_parked) { *parked = true; return; }   /* I1 */
        size_t end = r->payload_len - off > STDIN_CHUNK ? off + STDIN_CHUNK : r->payload_len;
        while (off < end) {
            size_t k = end - off > SSH_WRITE_MAX ? SSH_WRITE_MAX : end - off;
            int w = ssh_channel_write(ch, r->payload + off, (uint32_t)k);
            if (w == SSH_ERROR || w <= 0) {
                if (ssh_channel_is_eof(ch) || !ssh_channel_is_open(ch)) {
                    ESP_LOGW(TAG, "exec: the remote ended stdin after %u of %u bytes", (unsigned)off,
                             (unsigned)r->payload_len);
                    return;   /* no EOF to send: the read loop picks up the exit status */
                }
                ESP_LOGW(TAG, "exec: stdin write failed at %u of %u bytes: %s", (unsigned)off,
                         (unsigned)r->payload_len, ssh_get_error(s_sess));
                *failed = true;
                return;
            }
            off += (size_t)w;
        }
        int n;
        while ((n = ssh_channel_read_nonblocking(ch, tmp, sizeof(tmp), 0)) > 0) exec_take(r, tmp, (size_t)n, olen, truncated);
        if (n == SSH_ERROR) {
            ESP_LOGW(TAG, "exec: read failed during the upload: %s", ssh_get_error(s_sess));
            *failed = true;
            return;
        }
        read_stderr(ch, r->err, r->err_cap, elen);
        watch_keepalive();
    }
    if (ssh_channel_send_eof(ch) != SSH_OK) {
        ESP_LOGW(TAG, "exec: EOF failed: %s", ssh_get_error(s_sess));
        *failed = true;
        return;
    }
    r->eof_us = now_us();
}

static esp_err_t do_exec(req_t *r)
{
    if (r->out && r->cap) r->out[0] = 0;
    if (r->err && r->err_cap) r->err[0] = 0;
    if (r->exit_status) *r->exit_status = -1;
    r->exec_us = r->eof_us = 0;
    if (s_parked) return ESP_ERR_INVALID_STATE;   /* I1 */
    ssh_channel ch = open_channel();
    if (!ch) return ESP_ERR_INVALID_STATE;
    r->exec_us = now_us();
    if (ssh_channel_request_exec(ch, r->cmd) != SSH_OK) {
        ESP_LOGW(TAG, "exec request failed: %s", ssh_get_error(s_sess));
        close_channel(ch);
        s_drop = true;
        return ESP_FAIL;
    }
    size_t olen = 0, elen = 0;
    bool truncated = false, failed = false, timed_out = false, parked = false;
    char tmp[512];
    if (r->payload && r->payload_len > 0)
        exec_write_stdin(r, ch, &olen, &elen, &truncated, &failed, &timed_out, &parked);
    int64_t ka_us = now_us();
    while (!failed && !timed_out && !parked) {
        int n = ssh_channel_read_timeout(ch, tmp, sizeof(tmp), 0, 50);
        if (n == SSH_ERROR) { failed = true; break; }
        if (n > 0) exec_take(r, tmp, (size_t)n, &olen, &truncated);
        read_stderr(ch, r->err, r->err_cap, &elen);
        if (n == 0 && ssh_channel_is_eof(ch)) break;
        if (now_us() > r->deadline_us) { timed_out = true; break; }
        if (s_parked) { parked = true; break; }   /* I1: the link is going down: give up now */
        if (r->payload && now_us() - ka_us > 1000000) { watch_keepalive(); ka_us = now_us(); }
    }
    if (!failed && !timed_out && !parked && r->exit_status) {
        uint32_t code = 0;
        char *sig = NULL;
        int core = 0;
        if (ssh_channel_get_exit_state(ch, &code, &sig, &core) == SSH_OK) *r->exit_status = (int)code;
        ssh_string_free_char(sig);
    }
    close_channel(ch);
    if (r->err && r->err_cap) first_line(r->err);
    if (failed) { s_drop = true; return ESP_FAIL; }
    if (parked) return ESP_ERR_INVALID_STATE;   /* the park's teardown closes the session */
    /* A channel that never finishes means the link is gone (Wi-Fi lost without an event)
       or wedged; start over either way. */
    if (timed_out) { s_drop = true; return ESP_ERR_TIMEOUT; }
    return truncated ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

/* Reads pty output into s_screen (keeping the newest bytes) for ms, or until quiet for
   quiet_ms (when > 0). Returns false if the channel ended or failed. */
static bool pump(ssh_channel ch, size_t *len, int ms, int quiet_ms)
{
    int64_t start = now_us(), last = start;
    char tmp[512];
    while (now_us() - start < (int64_t)ms * 1000) {
        int n = ssh_channel_read_timeout(ch, tmp, sizeof(tmp), 0, 50);
        if (n == SSH_ERROR) return false;
        if (n > 0) {
            if (*len + n > SCREEN_CAP - 1) {
                size_t drop = *len + n - (SCREEN_CAP - 1);
                if (drop > *len) drop = *len;
                memmove(s_screen, s_screen + drop, *len - drop);
                *len -= drop;
            }
            size_t k = (size_t)n > SCREEN_CAP - 1 ? SCREEN_CAP - 1 : (size_t)n;
            memcpy(s_screen + *len, tmp, k);
            *len += k;
            s_screen[*len] = 0;
            last = now_us();
        } else if (ssh_channel_is_eof(ch)) {
            return false;
        }
        char sink[64];
        while (ssh_channel_read_nonblocking(ch, sink, sizeof(sink), 1) > 0) {}
        if (quiet_ms > 0 && now_us() - last >= (int64_t)quiet_ms * 1000) return true;
    }
    return true;
}

static bool send_bytes(ssh_channel ch, const char *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = ssh_channel_write(ch, b + off, n - off);
        if (w == SSH_ERROR) return false;
        off += (size_t)w;
    }
    return true;
}

/* Last non-empty line of the stripped screen, for error messages. */
static void last_line(char *plain, char *out, size_t cap)
{
    size_t n = strlen(plain);
    while (n && (plain[n - 1] == '\n' || plain[n - 1] == '\r' || plain[n - 1] == ' ')) plain[--n] = 0;
    char *p = plain + n;
    while (p > plain && p[-1] != '\n' && p[-1] != '\r') p--;
    strlcpy(out, p, cap);
}

static int refresh(int64_t deadline_us);

/* I1: a park counts as the deadline: the paste stops between steps and detaches with Ctrl+Z. */
static bool late(const req_t *r)
{
    return now_us() > r->deadline_us || s_parked;
}

/* Every phase checks the request's deadline: nothing is typed once it has passed, and
   typing that is cut short ends with Ctrl+Z only (the next paste's Ctrl+U clears the draft). */
static esp_err_t do_paste(req_t *r)
{
    if (r->err && r->err_cap) r->err[0] = 0;
    /* `claude attach` wakes a stopped session, so a paste checks it is still live right now,
       not as of the last poll. */
    int n = refresh(r->deadline_us);
    if (n < 0) {
        strlcpy(r->err, late(r) ? "timed out" : "could not check the session", r->err_cap);
        return late(r) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    const cc_session_t *hit = NULL;
    for (int i = 0; i < n; i++) {
        if (s_parse[i].id[0] && strcmp(s_parse[i].id, r->id) == 0) { hit = &s_parse[i]; break; }
    }
    if (!hit) {
        strlcpy(r->err, "no such session", r->err_cap);
        return ESP_ERR_NOT_FOUND;
    }
    /* A stopped entry stays listed (often still "done") but has no pid. Open (REQ_CHECK) may wake it:
       `claude attach` starts it again. Typing into one still needs it running. */
    if (!hit->background || (r->type != REQ_CHECK && (hit->state == CC_ENDED || !hit->live))) {
        strlcpy(r->err, hit->background ? "session has stopped" : "not a background session", r->err_cap);
        return ESP_ERR_NOT_FOUND;
    }
    if (late(r)) { strlcpy(r->err, "timed out", r->err_cap); return ESP_ERR_TIMEOUT; }
    if (r->type == REQ_CHECK) return ESP_OK;
    ssh_channel ch = open_channel();
    if (!ch) return ESP_ERR_INVALID_STATE;
    if (ssh_channel_request_pty_size(ch, "xterm-256color", 120, 40) != SSH_OK ||
        ssh_channel_request_exec(ch, r->cmd) != SSH_OK) {
        ESP_LOGW(TAG, "attach request failed: %s", ssh_get_error(s_sess));
        close_channel(ch);
        s_drop = true;
        return ESP_FAIL;
    }
    size_t len = 0;
    s_screen[0] = 0;
    /* 1. let the TUI draw: quiet for 1.0 s, at most 3 s */
    bool alive = pump(ch, &len, 3000, 1000);
    cc_strip_ansi(s_screen);
    if (!alive) {
        /* attach exited at once: its output says why (no such session, not running ...) */
        last_line(s_screen, r->err, r->err_cap);
        if (!r->err[0]) strlcpy(r->err, "attach ended", r->err_cap);
        close_channel(ch);
        return ESP_FAIL;
    }
    /* 2. fail closed: type only when the bottom of the screen is the empty input line.
       Anything else (a permission dialog takes CR as "yes", a spinner, a draft, a screen we
       don't know) means type nothing and detach. The dialog matcher is a second opinion. */
    bool approval = cc_screen_is_permission_prompt(s_screen);
    bool dialog = approval || !cc_screen_input_ready(s_screen);
    if (dialog || late(r)) {
        if (dialog) {
            size_t sn = strlen(s_screen);
            ESP_LOGD(TAG, "not an input prompt; screen tail: %s", s_screen + (sn > 300 ? sn - 300 : 0));
        }
        send_bytes(ch, "\x1a", 1);
        len = 0;
        pump(ch, &len, 1000, 0);
        close_channel(ch);
        if (!dialog) {
            strlcpy(r->err, "timed out", r->err_cap);
            return ESP_ERR_TIMEOUT;
        }
        /* a permission dialog, or another screen (a draft, queued messages) */
        strlcpy(r->err, approval ? "needs approval \xe2\x80\x94 open the session"
                                 : "not at the input prompt \xe2\x80\x94 open the session", r->err_cap);
        return approval ? CC_PASTE_NEEDS_APPROVAL : CC_PASTE_NOT_READY;
    }
    len = 0;
    /* 3. clear any draft, 4. bracketed paste, then CR on its own */
    bool ok = send_bytes(ch, "\x15\x15\x15\x15", 4) && pump(ch, &len, 200, 0);
    bool cut = false;
    if (ok && !(cut = late(r))) ok = send_bytes(ch, r->payload, r->payload_len - 1) && pump(ch, &len, 150, 0);
    if (ok && !cut && !(cut = late(r))) ok = send_bytes(ch, r->payload + r->payload_len - 1, 1);
    /* 5. let it submit, 6. detach with Ctrl+Z (never ← or a bare ESC) */
    if (ok && !cut) pump(ch, &len, 1000, 0);
    send_bytes(ch, "\x1a", 1);
    pump(ch, &len, 1000, 0);
    close_channel(ch);
    if (!ok) {
        strlcpy(r->err, "link lost while typing", r->err_cap);
        s_drop = true;
        return ESP_FAIL;
    }
    if (cut) {
        strlcpy(r->err, "timed out while typing", r->err_cap);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static int refresh(int64_t deadline_us);

static void handle(req_t *r)
{
    if (!s_sess || s_drop || s_parked) { answer(r, ESP_ERR_INVALID_STATE); return; }
    if (now_us() > r->deadline_us) { answer(r, ESP_ERR_TIMEOUT); return; }
    if (r->type == REQ_POLL) { answer(r, refresh(r->deadline_us) >= 0 ? ESP_OK : ESP_FAIL); return; }
    answer(r, r->type == REQ_EXEC ? do_exec(r) : do_paste(r));   /* paste and check */
}

static int refresh_once(int64_t deadline_us, bool *too_big);

/* Publishes s_parse[0..n) as the newest session list: copies it for snapshots, stamps the
   last-ok time, counts it and calls the subscribers (on this task). */
static void publish(int n)
{
    struct { cc_sessions_cb_t cb; void *ctx; } subs[MAX_SUBS];
    lock();
    memcpy(s_list, s_parse, sizeof(cc_session_t) * n);
    s_list_n = n;
    s_last_ok_ms = now_us() / 1000;
    s_gen++;
    memcpy(subs, s_subs, sizeof(subs));
    unlock();
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].cb) subs[i].cb(s_parse, n, subs[i].ctx);
    }
}

/* Runs `claude agents --json --all`, parses it into s_parse and publishes it, and counts
   consecutive failures for the status line. Returns the count, or -1 on failure. */
static int refresh(int64_t deadline_us)
{
    bool too_big = false;
    int n = refresh_once(deadline_us, &too_big);
    lock();
    /* the loop never repeats unchanged output, so a watch failure stays shown until its
       next good block, even when a paste's own refresh works */
    if (n >= 0) { s_poll_fails = s_w_bad ? CC_POLL_FAILS_SHOWN : 0; s_poll_too_big = s_w_bad && s_poll_too_big; }
    else { s_poll_fails++; s_poll_too_big = too_big; }
    unlock();
    return n;
}

static int refresh_once(int64_t deadline_us, bool *too_big)
{
    char cmd[128], claude[64];
    if (!cc_cmd_agents(cc_settings_claude_path(claude, sizeof(claude)), cmd, sizeof(cmd))) return -1;
    int status = -1;
    char err[160];
    int64_t poll_deadline = now_us() + (int64_t)POLL_TIMEOUT_MS * 1000;
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .out = s_json, .cap = JSON_CAP, .exit_status = &status,
                .err = err, .err_cap = sizeof(err),
                .deadline_us = deadline_us < poll_deadline ? deadline_us : poll_deadline };
    static int s_heap_logs;
    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t e = do_exec(&r);
    if (e == ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "poll: agents output is over %d KB; session list not updated", JSON_CAP / 1024);
        *too_big = true;
        return -1;
    }
    if (e != ESP_OK || status != 0) {
        ESP_LOGW(TAG, "poll failed: %s, exit %d: %s", esp_err_to_name(e), status, err);
        return -1;
    }
    int n = cc_sessions_parse(s_json, s_parse, POLL_CAP);
    if (s_heap_logs < 2) {   /* the first two polls: show what a poll costs internal RAM */
        s_heap_logs++;
        ESP_LOGI(TAG, "poll: %u bytes JSON, %d sessions; internal free %u -> %u (min ever %u)",
                 (unsigned)strlen(s_json), n, (unsigned)int_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
    if (n < 0) {
        ESP_LOGW(TAG, "poll: output is not a JSON array");
        return -1;
    }
    publish(n);
    return n;
}

/* ---------- long-poll watch (task only) ---------- */

static int eff_rate(void)
{
    int f = s_floor;
    return f > 0 ? f : s_rate;
}

static void wv_set_mode(watch_mode_t m, int interval)
{
    lock();
    s_wv.mode = m;
    s_wv.interval = interval;
    if (m == WM_STARTING) s_wv.block_us = s_wv.hb_us = 0;
    unlock();
}

/* Closes the channel. EOF + close, so sshd hangs up the pty and the loop gets SIGHUP.
   asked: the Tab5's own close (an interval change, off, a link teardown), never counted. */
static void watch_close(bool asked)
{
    if (!s_wch) return;
    close_channel(s_wch);
    s_wch = NULL;
    s_w_good = s_w_bad = false;
    ESP_LOGI(TAG, "watch: closed%s", asked ? "" : " (ended by the Host or the link)");
}

static void watch_fallback(const char *why)
{
    watch_close(true);
    s_w_fallback_until_us = now_us() + WATCH_RETRY_US;
    ESP_LOGW(TAG, "watch: falling back to polling: %s", why);
    lock();
    s_wv.mode = WM_FALLBACK;
    strlcpy(s_wv.why, why, sizeof(s_wv.why));
    unlock();
}

/* A channel end the Tab5 didn't ask for (EOF, BYE eof, BYE quiet, a read error): three in
   5 min fall back to polling. Returns true when it fell back. */
static bool watch_unasked_end(const char *what)
{
    watch_close(false);
    int64_t t = now_us();
    memmove(s_w_ends, s_w_ends + 1, sizeof(s_w_ends[0]) * 2);
    s_w_ends[2] = t;
    ESP_LOGW(TAG, "watch: channel ended: %s", what);
    if (s_w_ends[0] && t - s_w_ends[0] <= WATCH_ENDS_WINDOW_US) {
        watch_fallback("the channel ended 3 times in 5 min");
        return true;
    }
    return false;
}

static void watch_start(int interval)
{
    char claude[64], cmd[1024];
    if (interval > 120) interval = 120;   /* cc_cmd_watch's range; s_w_interval keeps the same I */
    cc_watch_params_t p = { interval, WATCH_HEARTBEAT_S, WATCH_QUIET_S, WATCH_LIFE_S };
    if (!cc_cmd_watch(cc_settings_claude_path(claude, sizeof(claude)), &p, cmd, sizeof(cmd))) {
        watch_fallback("bad watch command");
        return;
    }
    int64_t opened = now_us();         /* the banner deadline counts the requests too */
    ssh_channel ch = open_channel();   /* a failure here is the link's: s_drop */
    if (!ch) return;
    if (ssh_channel_request_pty_size(ch, "xterm-256color", 200, 50) != SSH_OK) {
        ESP_LOGW(TAG, "watch: pty request failed: %s", ssh_get_error(s_sess));
        close_channel(ch);
        watch_fallback("pty request failed");
        return;
    }
    if (ssh_channel_request_exec(ch, cmd) != SSH_OK) {
        ESP_LOGW(TAG, "watch: exec request failed: %s", ssh_get_error(s_sess));
        close_channel(ch);
        watch_fallback("exec request failed");
        return;
    }
    s_wch = ch;
    cc_watch_init(&s_w, s_wbody, JSON_CAP);
    s_w_interval = interval;
    s_w_banner_ms = s_banner_ms;
    s_banner_ms = WATCH_BANNER_MS;
    s_w_good = s_w_bad = false;
    s_w_opened_us = opened;
    s_w_live_us = now_us();
    s_w_ka_us = 0;   /* the first keepalive goes at once */
    wv_set_mode(WM_STARTING, interval);
    ESP_LOGI(TAG, "watch: starting, I=%ds", interval);
}

static void watch_block(void)
{
    int64_t t = now_us();
    lock();
    s_wv.block_us = t;
    unlock();
    int n = -1;
    if (s_w.rc == 0) n = cc_sessions_parse(s_w.body, s_parse, POLL_CAP);
    if (n >= 0) {
        s_w_good = true;
        s_w_bad = false;
        lock();
        s_poll_fails = 0;
        s_poll_too_big = false;
        unlock();
        publish(n);
        return;
    }
    char first[120];
    strlcpy(first, s_w.body, sizeof(first));
    first_line(first);
    if (s_w.rc != 0) ESP_LOGW(TAG, "watch: agents exited %d: %s", s_w.rc, first);
    else ESP_LOGW(TAG, "watch: output is not a JSON array");
    s_w_good = false;
    s_w_bad = true;
    lock();
    s_poll_fails = CC_POLL_FAILS_SHOWN;   /* unchanged output is never repeated: show it now */
    s_poll_too_big = false;
    unlock();
}

/* Unchanged list, confirmed fresh: stamp it and tell the subscribers (the keyboard light's
   staleness rule reads the last-ok time). Only after a good block on this channel. */
static void watch_heartbeat(void)
{
    lock();
    s_wv.hb_us = now_us();
    int n = s_w_good ? s_list_n : -1;
    if (n > 0) memcpy(s_parse, s_list, sizeof(cc_session_t) * n);
    unlock();
    if (n >= 0) publish(n);
}

/* Returns false when the channel is gone (closed here, or fallen back). */
static bool watch_event(cc_watch_ev_t ev)
{
    s_w_live_us = now_us();
    switch (ev) {
    case CC_WATCH_BANNER:
        wv_set_mode(WM_ON, s_w_interval);
        ESP_LOGI(TAG, "watch: on, I=%ds", s_w_interval);
        break;
    case CC_WATCH_BLOCK:
        watch_block();
        break;
    case CC_WATCH_OVERFLOW:
        ESP_LOGE(TAG, "watch: agents output is over %d KB; session list not updated", JSON_CAP / 1024);
        s_w_good = false;
        s_w_bad = true;
        lock();
        s_poll_fails = CC_POLL_FAILS_SHOWN;
        s_poll_too_big = true;
        unlock();
        break;
    case CC_WATCH_HEARTBEAT:
        watch_heartbeat();
        break;
    case CC_WATCH_BYE:
        if (strcmp(s_w.bye, "life") == 0) {
            ESP_LOGI(TAG, "watch: loop reached its lifetime; restarting");
            watch_close(true);
        } else {
            char what[32];
            snprintf(what, sizeof(what), "BYE %s", s_w.bye);
            watch_unasked_end(what);
        }
        return false;
    default:
        break;
    }
    return true;
}

/* Reads what has arrived without blocking, writes the keepalive, checks liveness. */
static void watch_service(void)
{
    char tmp[1024];
    int64_t t0 = now_us();
    /* the banner deadline is checked before reading, so a deadline that has passed wins over
       a banner that is already buffered (test-fallback's 1 ms) */
    lock();
    bool starting = s_wv.mode == WM_STARTING;
    unlock();
    if (starting && t0 - s_w_opened_us >= (int64_t)s_w_banner_ms * 1000) {
        char why[48];
        if (s_w_banner_ms < 1000) snprintf(why, sizeof(why), "no banner in %d ms", s_w_banner_ms);
        else snprintf(why, sizeof(why), "no banner in %d s", s_w_banner_ms / 1000);
        watch_fallback(why);
        return;
    }
    for (;;) {
        int n = ssh_channel_read_nonblocking(s_wch, tmp, sizeof(tmp), 0);
        if (n == SSH_EOF || (n == 0 && ssh_channel_is_eof(s_wch))) { watch_unasked_end("EOF"); return; }
        if (n < 0) {
            watch_unasked_end("read error");
            if (!ssh_is_connected(s_sess)) s_drop = true;
            return;
        }
        if (n == 0) break;
        size_t off = 0;
        while (off < (size_t)n) {
            size_t used = 0;
            cc_watch_ev_t ev = cc_watch_feed(&s_w, tmp + off, (size_t)n - off, &used);
            off += used;
            if (ev != CC_WATCH_NONE && !watch_event(ev)) return;
        }
        if (now_us() - t0 > 200000) break;   /* serve requests between big reads */
    }
    /* drain stderr (a pty merges it into stdout, but don't let it fill a window) */
    while (ssh_channel_read_nonblocking(s_wch, tmp, sizeof(tmp), 1) > 0) {}
    int64_t t = now_us();
    if (!s_w_ka_us || t - s_w_ka_us >= (int64_t)CC_WATCH_KEEPALIVE_S * 1000000) {
        if (!send_bytes(s_wch, "\n", 1)) { watch_unasked_end("keepalive write failed"); return; }
        s_w_ka_us = t;
    }
    lock();
    watch_mode_t m = s_wv.mode;
    unlock();
    int64_t dead_us = (int64_t)(2 * WATCH_HEARTBEAT_S + s_w_interval) * 1000000;
    if (m == WM_ON && t - s_w_live_us > dead_us) {
        ESP_LOGW(TAG, "watch: no block or heartbeat for %ds; the link is dead",
                 2 * WATCH_HEARTBEAT_S + s_w_interval);
        s_drop = true;
    }
}

/* A long stdin exec (cc_link_transcribe, up to 60 s) blocks the task: keep the watch's quiet timer (3 K) from
   ending the loop. Writes the keepalive only when due; a failed write is left for watch_service to notice. */
static void watch_keepalive(void)
{
    if (!s_wch) return;
    int64_t t = now_us();
    if (s_w_ka_us && t - s_w_ka_us < (int64_t)CC_WATCH_KEEPALIVE_S * 1000000) return;
    if (send_bytes(s_wch, "\n", 1)) s_w_ka_us = t;
}

/* One step of the connected loop while the watch should run (interval > 0). Returns false
   when the watch is not wanted or in fallback, so the caller polls instead. */
static bool watch_step(int interval)
{
    if (interval > 120) interval = 120;   /* the loop's range; compared with s_w_interval below */
    if (s_watch_reset) {
        s_watch_reset = false;
        s_w_fallback_until_us = 0;
        memset(s_w_ends, 0, sizeof(s_w_ends));
    }
    if (!s_watch_want || interval <= 0) {
        watch_close(true);
        wv_set_mode(s_watch_want ? WM_PAUSED : WM_OFF, interval);
        return false;
    }
    if (s_w_fallback_until_us) {
        if (now_us() < s_w_fallback_until_us) return false;
        s_w_fallback_until_us = 0;
        ESP_LOGI(TAG, "watch: retrying after the fallback");
    }
    if (s_wch && s_w_interval != interval) {
        ESP_LOGI(TAG, "watch: interval %ds -> %ds; restarting", s_w_interval, interval);
        watch_close(true);
    }
    if (!s_wch) {
        watch_start(interval);
        if (!s_wch) return s_w_fallback_until_us == 0;   /* a channel open failure: s_drop */
    }
    watch_service();
    return true;
}

static void link_task(void *arg)
{
    (void)arg;
    int attempt = 0;
    int64_t last_poll = 0, last_alive = now_us();
    for (;;) {
        if (s_reconfig) {
            s_reconfig = false;
            teardown();
            attempt = 0;
            lock();
            s_list_n = -1;
            s_pin_host[0] = 0;
            s_ok_host[0] = 0;
            unlock();
            set_fail(CC_LINK_FAIL_NONE, NULL);
        }
        if (s_reset_backoff) { s_reset_backoff = false; attempt = 0; }
        if (s_forget) {
            /* cc_link_forget_host_key cleared the pins: drop the session (the Tab5's own
               close, so the watch fallback doesn't count it) and connect again now */
            s_forget = false;
            teardown();
            attempt = 0;
            set_fail(CC_LINK_FAIL_NONE, NULL);
            lock();
            s_forget_ack = s_forget_req;
            unlock();
        }
        if (s_parked) {
            /* Radio off: the Tab5's own close (never counted toward the watch fallback), and no
               reconnect until unparked */
            if (s_sess) { s_down_why = "parked"; teardown(); }
            attempt = 0;
            if (s_state != LS_WAITING) set_state(LS_WAITING);
            drain_queue_down(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_sess && s_drop) {
            teardown();
            attempt = 0;     /* first retry right away, then back off */
        }
        if (!s_sess) {
            char name[sizeof(g_settings.companion_server)];
            lock();
            strlcpy(name, g_settings.companion_server, sizeof(name));
            strlcpy(s_shown, name, sizeof(s_shown));
            unlock();
            if (!name[0]) {
                set_state(LS_NO_MAC);
                drain_queue_down(pdMS_TO_TICKS(1000));
                continue;
            }
            set_state(LS_CONNECTING);
            drain_queue_down(0);
            s_in_connect = true;
            bool ok = connect_once(name);
            s_in_connect = false;
            if (ok) {
                set_state(LS_CONNECTED);
                attempt = 0;
                last_poll = 0;
                last_alive = now_us();
                continue;
            }
            int wait = s_backoff[attempt < 5 ? attempt : 5];
            if (attempt < 5) attempt++;
            lock();
            s_retry_at_us = now_us() + (int64_t)wait * 1000000;
            s_state = LS_WAITING;
            unlock();
            while (now_us() < s_retry_at_us && !s_reconfig && !s_reset_backoff && !s_forget && !s_parked) {
                int64_t left_ms = (s_retry_at_us - now_us()) / 1000;
                drain_queue_down(pdMS_TO_TICKS(left_ms > 0 ? left_ms : 1));
            }
            continue;
        }
        /* connected: the watch channel feeds the list; Plan 1's fixed poll is the fallback */
        int rate = eff_rate();
        if (watch_step(rate)) {
            last_alive = now_us();
            req_t *r = NULL;
            if (!s_drop && s_wch &&
                xQueueReceive(s_queue, &r, pdMS_TO_TICKS(WATCH_WAIT_MS)) == pdTRUE && r)
                handle(r);
            continue;
        }
        int64_t t = now_us();
        if (rate > 0 && t - last_poll >= (int64_t)rate * 1000000) {
            refresh(INT64_MAX);
            last_poll = now_us();
            last_alive = last_poll;
            continue;
        }
        if (rate <= 0 && t - last_alive > 30 * 1000000LL) {
            /* paused: still notice a dead link */
            if (ssh_send_ignore(s_sess, "") != SSH_OK || !ssh_is_connected(s_sess)) s_drop = true;
            last_alive = t;
            continue;
        }
        int64_t until = rate > 0 ? last_poll + (int64_t)rate * 1000000 - t : 1000000;
        if (until > 1000000) until = 1000000;
        req_t *r = NULL;
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(until > 1000 ? until / 1000 : 1)) == pdTRUE && r)
            handle(r);
    }
}

/* ---------- public calls ---------- */

static void busy(int d)
{
    lock();
    s_busy += d;
    unlock();
}


static esp_err_t submit(req_t *r)
{
    if (!s_queue || !cc_link_connected()) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCurrentTaskHandle() == s_task) {   /* a subscriber calling back in */
        if (!s_sess || s_drop) return ESP_ERR_INVALID_STATE;
        if (r->type == REQ_POLL) return refresh(r->deadline_us) >= 0 ? ESP_OK : ESP_FAIL;
        return r->type == REQ_EXEC ? do_exec(r) : do_paste(r);
    }
    r->done = xSemaphoreCreateBinary();
    if (!r->done) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_queue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
        vSemaphoreDelete(r->done);
        return ESP_ERR_TIMEOUT;
    }
    /* The task answers every request: at once when the link is down, by the deadline
       otherwise (every wait in it is bounded). */
    xSemaphoreTake(r->done, portMAX_DELAY);
    vSemaphoreDelete(r->done);
    return r->result;
}

esp_err_t cc_link_exec(const char *cmd, char *out, size_t cap, int *exit_status,
                       char *err, size_t err_cap, int timeout_ms)
{
    if (out && cap) out[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!cmd || timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .out = out, .cap = cap, .exit_status = exit_status,
                .err = err, .err_cap = err_cap, .deadline_us = now_us() + (int64_t)timeout_ms * 1000 };
    return submit(&r);
}

esp_err_t cc_link_exec_tail(const char *cmd, char *out, size_t cap, int *exit_status,
                            char *err, size_t err_cap, int timeout_ms)
{
    if (out && cap) out[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!cmd || timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .out = out, .cap = cap, .exit_status = exit_status,
                .err = err, .err_cap = err_cap, .keep_tail = true,
                .deadline_us = now_us() + (int64_t)timeout_ms * 1000 };
    return submit(&r);
}

esp_err_t cc_link_paste(const char *id, const char *text, char *err, size_t err_cap)
{
    char local_err[8];
    if (!err || !err_cap) { err = local_err; err_cap = sizeof(local_err); }
    err[0] = 0;
    char cmd[128], claude[64];
    if (!cc_cmd_attach(cc_settings_claude_path(claude, sizeof(claude)), id, cmd, sizeof(cmd))) {
        strlcpy(err, "bad session id", err_cap);
        return ESP_ERR_INVALID_ARG;
    }
    size_t cap = (text ? strlen(text) : 0) + 32;
    char *payload = malloc(cap);
    if (!payload) return ESP_ERR_NO_MEM;
    size_t n = cc_paste_build(text, payload, cap);
    if (n == 0) {
        free(payload);
        strlcpy(err, "empty text", err_cap);
        return ESP_ERR_INVALID_ARG;
    }
    req_t r = { .type = REQ_PASTE, .cmd = cmd, .id = id, .payload = payload, .payload_len = n,
                .err = err, .err_cap = err_cap, .deadline_us = now_us() + (int64_t)PASTE_BUDGET_MS * 1000 };
    busy(1);
    esp_err_t e = submit(&r);
    busy(-1);
    free(payload);
    return e;
}

esp_err_t cc_link_check_live(const char *id, char *err, size_t err_cap)
{
    char local_err[8];
    if (!err || !err_cap) { err = local_err; err_cap = sizeof(local_err); }
    err[0] = 0;
    if (!cc_id_valid(id)) {
        strlcpy(err, "bad session id", err_cap);
        return ESP_ERR_INVALID_ARG;
    }
    req_t r = { .type = REQ_CHECK, .id = id, .err = err, .err_cap = err_cap,
                .deadline_us = now_us() + (int64_t)POLL_TIMEOUT_MS * 1000 };
    busy(1);
    esp_err_t e = submit(&r);
    busy(-1);
    return e;
}

esp_err_t cc_link_voice_check(int *exit_status)
{
    int st = -1;
    if (exit_status) *exit_status = -1;
    char cmd[256], w[64], m[64];
    if (!cc_cmd_voice_check(cc_settings_whisper_path(w, sizeof(w)), cc_settings_whisper_model(m, sizeof(m)), cmd, sizeof(cmd)))
        return ESP_ERR_INVALID_ARG;
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .exit_status = &st,
                .deadline_us = now_us() + (int64_t)VOICE_CHECK_MS * 1000 };
    busy(1);
    esp_err_t e = submit(&r);
    busy(-1);
    if (exit_status) *exit_status = st;
    ESP_LOGI(TAG, "voice check: %s, exit %d", esp_err_to_name(e), st);
    return e;
}

esp_err_t cc_link_transcribe(const void *wav, size_t len, char *out, size_t cap, int *exit_status,
                             int *upload_ms, int timeout_ms)
{
    int st = -1;
    if (out && cap) out[0] = 0;
    if (exit_status) *exit_status = -1;
    if (upload_ms) *upload_ms = -1;
    if (!wav || !len || !out || cap < 2 || timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    char cmd[320], w[64], m[64];
    if (!cc_cmd_transcribe(cc_settings_whisper_path(w, sizeof(w)), cc_settings_whisper_model(m, sizeof(m)), cmd, sizeof(cmd)))
        return ESP_ERR_INVALID_ARG;
    int64_t t0 = now_us();
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .out = out, .cap = cap, .exit_status = &st,
                .payload = wav, .payload_len = len, .deadline_us = t0 + (int64_t)timeout_ms * 1000 };
    busy(1);
    esp_err_t e = submit(&r);
    busy(-1);
    if (exit_status) *exit_status = st;
    int up = r.exec_us && r.eof_us ? (int)((r.eof_us - r.exec_us) / 1000) : -1;
    if (upload_ms) *upload_ms = up;
    /* lengths, durations and the exit status only: never the transcript (Plan 5 rule 3) */
    ESP_LOGI(TAG, "transcribe: %u B up in %d ms, %d ms in all, %s, exit %d, %u B back", (unsigned)len, up,
             (int)((now_us() - t0) / 1000), esp_err_to_name(e), st, (unsigned)strlen(out));
    return e;
}

void cc_poll_set_rate(int seconds)
{
    s_rate = seconds < 0 ? 0 : seconds;
    kick();
}

void cc_poll_set_floor(int seconds)
{
    s_floor = seconds < 0 ? 0 : seconds;
    kick();
}

uint32_t cc_poll_generation(void)
{
    if (!s_lock) return 0;
    lock();
    uint32_t g = s_gen;
    unlock();
    return g;
}

bool cc_link_busy(void)
{
    return s_busy > 0;
}

void cc_link_watch_enable(bool on)
{
    s_watch_want = on;
    if (on) s_watch_reset = true;   /* forget a fallback and try now */
    kick();
}

bool cc_link_watch_wanted(void) { return s_watch_want; }

void cc_link_watch_resume(bool on)
{
    s_watch_want = on;
    kick();
}

void cc_link_watch_test_fallback(void)
{
    s_banner_ms = 1;
}

static int ago_s(int64_t then_us, int64_t now)
{
    return (int)((now - then_us) / 1000000);
}

const char *cc_watch_status_text(char *b, size_t cap)
{
    if (!b || !cap) return "";
    if (!s_task || !s_watch_want) {
        strlcpy(b, "watch: off", cap);
        return b;
    }
    lock();
    watch_mode_t m = s_wv.mode;
    int iv = s_wv.interval;
    int64_t blk = s_wv.block_us, hb = s_wv.hb_us;
    char why[sizeof(s_wv.why)];
    strlcpy(why, s_wv.why, sizeof(why));
    bool up = s_state == LS_CONNECTED;
    unlock();
    int64_t t = now_us();
    int rate = eff_rate();
    if (!up) {
        strlcpy(b, "watch: starting (link down)", cap);
    } else if (m == WM_FALLBACK) {
        snprintf(b, cap, "watch: fallback (%s), polling every %ds", why, rate);
    } else if (m == WM_PAUSED) {
        strlcpy(b, "watch: off (polling paused)", cap);
    } else if (m == WM_ON) {
        char a[32], h[32];
        if (blk) snprintf(a, sizeof(a), "last block %ds ago", ago_s(blk, t));
        else strlcpy(a, "no block yet", sizeof(a));
        if (hb) snprintf(h, sizeof(h), "heartbeat %ds ago", ago_s(hb, t));
        else strlcpy(h, "no heartbeat yet", sizeof(h));
        snprintf(b, cap, "watch: on I=%ds, %s, %s", iv, a, h);
    } else {
        strlcpy(b, "watch: starting", cap);
    }
    return b;
}

void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx)
{
    if (!s_lock || !cb) return;
    lock();
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].cb) { s_subs[i].cb = cb; s_subs[i].ctx = ctx; break; }
    }
    unlock();
}

int64_t cc_poll_last_ok_ms(void)
{
    if (!s_lock) return 0;
    lock();
    int64_t t = s_last_ok_ms;
    unlock();
    return t;
}

int cc_poll_snapshot(cc_session_t *out, int cap)
{
    if (!s_lock) return -1;
    lock();
    int n = s_list_n;
    if (n > cap) n = cap;
    if (n > 0) memcpy(out, s_list, sizeof(cc_session_t) * n);
    unlock();
    return n;
}

/* The host:port pins the link and the terminal use for the companion's Host: the entry's host
   string itself, the entry's host after a MagicDNS lookup, the carry source's tailnet address, the
   fallback LAN address, the last LAN address the name resolved to, (resolve: a fresh lookup of the
   name now, which can block for seconds) and, last, the address the link last tried. Deduplicated.
   Returns how many it filled (0: no Host set up). */
static int add_target(char hosts[PIN_TARGETS][128], int ports[PIN_TARGETS], int n, const char *h, int port)
{
    if (!h || !h[0] || n >= PIN_TARGETS) return n;
    for (int i = 0; i < n; i++)
        if (ports[i] == port && strcmp(hosts[i], h) == 0) return n;
    strlcpy(hosts[n], h, sizeof(hosts[n]));
    ports[n] = port;
    return n + 1;
}

static int pin_targets(char hosts[PIN_TARGETS][128], int ports[PIN_TARGETS], bool resolve)
{
    char name[sizeof(g_settings.companion_server)];
    lock();
    strlcpy(name, g_settings.companion_server, sizeof(name));
    unlock();
    int n = 0;
    int idx = name[0] ? server_index(name) : -1;
    const server_t *sv = idx >= 0 ? server_store_get(idx) : NULL;
    if (!sv) return 0;
    int port = sv->port > 0 ? sv->port : 22;
    char host[128];
    strlcpy(host, sv->host, sizeof(host));
    n = add_target(hosts, ports, n, host, port);
    char ip[16];
    if (tailscale_resolve(host, ip, sizeof(ip))) n = add_target(hosts, ports, n, ip, port);
    char tsip[16], fb[16], last[16], pin[128];
    int pin_port;
    lock();
    strlcpy(tsip, s_carry.ip[0] ? s_carry.ip : g_settings.mac_ts_ip, sizeof(tsip));
    strlcpy(fb, g_settings.mac_lan_ip, sizeof(fb));
    strlcpy(last, s_lan_last, sizeof(last));
    strlcpy(pin, s_pin_host, sizeof(pin));
    pin_port = s_pin_port;
    unlock();
    n = add_target(hosts, ports, n, tsip, port);
    n = add_target(hosts, ports, n, fb, port);
    n = add_target(hosts, ports, n, last, port);
    char cached[16];   /* review M3: the Host address cache's address, which may differ from all the above */
    lock();
    strlcpy(cached, s_hc.ip, sizeof(cached));
    unlock();
    n = add_target(hosts, ports, n, cached, port);
    if (resolve) {
        const char *how;
        if (mac_resolve(host, ip, sizeof(ip), &how)) n = add_target(hosts, ports, n, ip, port);
    }
    if (pin[0]) {
        /* last, so cc_link_debug_corrupt_host_key and the fingerprint lookup find the link's own pin */
        for (int i = 0; i < n; i++)
            if (ports[i] == pin_port && strcmp(hosts[i], pin) == 0) {
                for (int j = i; j < n - 1; j++) { strlcpy(hosts[j], hosts[j + 1], sizeof(hosts[j])); ports[j] = ports[j + 1]; }
                n--;
                break;
            }
        n = add_target(hosts, ports, n, pin, pin_port);
    }
    return n;
}

/* The pending forget both callers share: expect NULL forgets (erases every pin first), else trusts only that key. */
static esp_err_t forget_run(const char *expect, char *fp_out, size_t fp_cap, int timeout_ms)
{
    if (fp_out && fp_cap) fp_out[0] = 0;
    if (!s_task || xTaskGetCurrentTaskHandle() == s_task) return ESP_ERR_INVALID_STATE;
    char hosts[PIN_TARGETS][128]; int ports[PIN_TARGETS];
    int n = pin_targets(hosts, ports, true);   /* every LAN and tailnet pin, resolved now (M2) */
    if (n == 0) return ESP_ERR_INVALID_STATE;
    char entry[128] = "";
    if (expect) {
        /* a trust only adds a pin where there is none: never over one, nor one it can't read, nor over a carry
           source for this entry (a key unlike it is a changed key, refused as today); checked again when it is
           applied (trust_still_allowed, I2) */
        int ci = cc_settings_server();
        const server_t *sv = ci >= 0 ? server_store_get(ci) : NULL;
        if (!sv) return ESP_ERR_INVALID_STATE;
        strlcpy(entry, sv->host, sizeof(entry));
        kh_pin_t pins[PIN_TARGETS];
        uint8_t h[32];
        for (int i = 0; i < n; i++) pins[i] = ssh_known_host_hash(hosts[i], ports[i], h);
        lock();
        bool carry = s_carry.host[0] && strcmp(s_carry.host, entry) == 0;
        unlock();
        if (!kh_trust_allowed(pins, n, carry, true, false)) return ESP_ERR_NOT_ALLOWED;
    } else {
        for (int i = 0; i < n; i++) ssh_known_host_forget(hosts[i], ports[i]);
        carry_store(NULL);   /* the old key is no longer trusted: nothing carries it over */
    }
    cc_link_host_cache_clear();   /* review M3: the cached address was the old key's too */
    lock();
    s_ok_host[0] = 0;    /* M6: the forgotten key's address is no take-over target */
    strlcpy(s_forget_expect, expect ? expect : "", sizeof(s_forget_expect));
    strlcpy(s_forget_entry, entry, sizeof(s_forget_entry));
    s_forget_deadline_us = expect ? now_us() + (int64_t)KH_TRUST_TTL_MS * 1000 : 0;
    uint32_t want = ++s_forget_req;
    unlock();
    s_forget = true;
    kick();
    busy(+1);   /* the power ladder holds while the forget runs */
    int64_t end = now_us() + (int64_t)timeout_ms * 1000;
    esp_err_t r;
    for (;;) {
        lock();
        bool done = (int32_t)(s_forget_done - want) >= 0;
        bool cancelled = done && s_forget_cancelled == want;
        if (done && !cancelled && fp_out && fp_cap) strlcpy(fp_out, s_forget_fp, fp_cap);
        unlock();
        if (cancelled) { r = ESP_ERR_INVALID_RESPONSE; break; }   /* I2: ended without a pin */
        if (done) { r = ESP_OK; break; }
        if (now_us() >= end) { r = ESP_ERR_TIMEOUT; break; }   /* stays pending: cc_link_forget_done */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    busy(-1);
    return r;
}

esp_err_t cc_link_forget_host_key(char *fp_out, size_t fp_cap, int timeout_ms)
{
    return forget_run(NULL, fp_out, fp_cap, timeout_ms);
}

esp_err_t cc_link_trust_host_key(const char *fp, char *fp_out, size_t fp_cap, int timeout_ms)
{
    if (fp_out && fp_cap) fp_out[0] = 0;
    if (!fp || strncmp(fp, "SHA256:", 7) != 0 || !fp[7] || strlen(fp) >= sizeof(s_forget_expect)) return ESP_ERR_NOT_ALLOWED;
    return forget_run(fp, fp_out, fp_cap, timeout_ms);
}

bool cc_link_unknown_host_key(char *fp_out, size_t fp_cap)
{
    lock();
    bool have = s_fail == CC_LINK_FAIL_HOSTKEY && s_unknown_fp[0];
    if (fp_out && fp_cap) strlcpy(fp_out, have ? s_unknown_fp : "", fp_cap);
    unlock();
    return have;
}

uint32_t cc_link_forget_done(char *fp_out, size_t fp_cap)
{
    lock();
    uint32_t n = s_forget_pins;
    if (fp_out && fp_cap) strlcpy(fp_out, s_forget_fp, fp_cap);
    unlock();
    return n;
}

bool cc_link_pinned_fingerprint(char *fp_out, size_t fp_cap)
{
    if (fp_out && fp_cap) fp_out[0] = 0;
    char hosts[PIN_TARGETS][128]; int ports[PIN_TARGETS];
    int n = pin_targets(hosts, ports, false);
    for (int i = n - 1; i >= 0; i--)   /* the link's own address first */
        if (ssh_known_host_fingerprint(hosts[i], ports[i], fp_out, fp_cap)) return true;
    return false;
}

esp_err_t cc_link_debug_corrupt_host_key(char *host_out, size_t host_cap)
{
    if (host_out && host_cap) host_out[0] = 0;
    char hosts[PIN_TARGETS][128]; int ports[PIN_TARGETS];
    int n = pin_targets(hosts, ports, false);
    if (n == 0) return ESP_ERR_INVALID_STATE;
    /* the last target is the link's own pin when it has connected, else the resolved entry */
    if (!ssh_known_host_set_debug(hosts[n - 1], ports[n - 1])) return ESP_FAIL;
    if (host_out && host_cap) snprintf(host_out, host_cap, "%s:%d", hosts[n - 1], ports[n - 1]);
    s_drop = true;   /* reconnect, so the next connect meets the wrong pin */
    kick();
    return ESP_OK;
}

/* Applies a change to s_park; logs and wakes the task when the parked state flips. */
static void park_apply(void (*change)(cc_park_t *p, void *arg), void *arg, const char *why)
{
    lock();
    bool was = cc_park_on(&s_park);
    change(&s_park, arg);
    bool now = cc_park_on(&s_park);
    s_parked = now;
    unlock();
    if (was == now) return;
    if (!now) s_reset_backoff = true;   /* connect at once */
    ESP_LOGI(TAG, "%s", now ? why : "unparked: connecting");
    kick();
}
static void ch_radio(cc_park_t *p, void *arg) { cc_park_set_radio(p, *(bool *)arg); }
static void ch_take(cc_park_t *p, void *arg) { (void)arg; cc_park_guard_take(p); }
static void ch_drop(cc_park_t *p, void *arg) { (void)arg; cc_park_guard_drop(p); }

void cc_link_park(bool parked)
{
    park_apply(ch_radio, &parked, "parked (radio off)");
}

bool cc_link_idle(void)
{
    return cc_park_idle(s_have_sess, s_in_connect);
}

bool cc_link_close_now(int timeout_ms)
{
    park_apply(ch_take, NULL, "parked: closing before the link goes down");
    int64_t end = now_us() + (int64_t)timeout_ms * 1000;
    while (!cc_link_idle() && now_us() < end) vTaskDelay(pdMS_TO_TICKS(10));
    bool idle = cc_link_idle();
    if (!idle) ESP_LOGW(TAG, "close: still busy after %d ms", timeout_ms);
    return idle;
}

void cc_link_reopen(void)
{
    park_apply(ch_drop, NULL, "");
}

/* The close-before-down guard's closer (C1): parks the link (a nested guard park, so a Radio-off park
   survives the reopen) when its session isn't excluded by tailnet_only. */
static bool link_on_tailnet(void)
{
    lock();
    bool t = tailscale_is_tailnet_ip(s_pin_host);
    unlock();
    return t;
}
static bool lg_link_close(bool tailnet_only, void *ctx)
{
    (void)ctx;
    if (tailnet_only && !link_on_tailnet()) return false;
    park_apply(ch_take, NULL, "parked: closing before the link goes down");
    return true;
}
static bool lg_link_idle(bool tailnet_only, void *ctx)
{
    (void)ctx;
    if (tailnet_only && !link_on_tailnet()) return true;
    return cc_link_idle();
}
static void lg_link_reopen(void *ctx) { (void)ctx; cc_link_reopen(); }

bool cc_link_parked(void) { return s_parked; }

esp_err_t cc_link_poll_now(int timeout_ms)
{
    if (timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    req_t r = { .type = REQ_POLL, .deadline_us = now_us() + (int64_t)timeout_ms * 1000 };
    return submit(&r);
}

void cc_link_set_kick(bool on) { s_kick = on; }
bool cc_link_kick_on(void) { return s_kick; }

const char *cc_link_reconnect_log(char *b, size_t cap)
{
    if (!b || !cap) return "";
    b[0] = 0;
    rlog_t r[RLOG_N];
    int n, head;
    lock();
    memcpy(r, s_rlog, sizeof(r));
    n = s_rlog_n;
    head = s_rlog_head;
    unlock();
    if (!n) { strlcpy(b, "reconnect log: empty", cap); return b; }
    int64_t now = now_us() / 1000;
    size_t len = 0;
    for (int i = 0; i < n && len + 1 < cap; i++) {
        const rlog_t *e = &r[(head - n + i + RLOG_N) % RLOG_N];
        char when[24] = "-";
        time_t t = (time_t)e->epoch;
        struct tm tm;
        if (t > 1600000000 && localtime_r(&t, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        int w = snprintf(b + len, cap - len, "%s%s %s %s (%.1f s ago)", i ? "\n" : "", when, e->up ? "up" : "down", e->why,
                         (now - e->mono_ms) / 1000.0);
        if (w < 0) break;
        len += (size_t)w;
        if (len >= cap) { len = cap - 1; break; }
    }
    return b;
}

bool cc_link_mac_address(char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = 0;
    lock();
    strlcpy(out, s_pin_host, cap);
    char name[sizeof(g_settings.companion_server)];
    strlcpy(name, g_settings.companion_server, sizeof(name));
    unlock();
    if (out[0]) return true;
    if (!name[0]) return false;
    int idx = server_index(name);
    const server_t *sv = idx >= 0 ? server_store_get(idx) : NULL;
    if (!sv) return false;
    strlcpy(out, sv->host, cap);
    return out[0] != 0;
}

bool cc_link_verified_address(char *out, size_t cap)
{
    if (!out || !cap) return false;
    lock();
    strlcpy(out, s_ok_host, cap);
    unlock();
    return out[0] != 0;
}

bool cc_link_resolve_test(const char *host, char *out, size_t cap, const char **how)
{
    if (!host || !out || !cap || !how) return false;
    int64_t t0 = now_us();
    bool ok = mac_resolve(host, out, cap, how);
    ESP_LOGI(TAG, "resolve %s: %s in %lld ms", host, ok ? *how : "failed", (long long)((now_us() - t0) / 1000));
    return ok;
}

bool cc_link_mac_path(char *out, size_t cap)
{
    if (!out || !cap) return false;
    lock();
    bool up = s_state == LS_CONNECTED && s_pin_host[0];
    if (up) snprintf(out, cap, "%s %s", tailscale_is_tailnet_ip(s_pin_host) ? "tailnet" : "lan", s_pin_host);
    unlock();
    if (!up) strlcpy(out, "none", cap);
    return up;
}

/* A host the entry may hold: 1 to 127 printable characters, no spaces. */
static bool host_ok(const char *h)
{
    size_t n = h ? strlen(h) : 0;
    if (n == 0 || n >= sizeof(((server_t *)0)->host)) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)h[i] <= ' ' || (unsigned char)h[i] >= 0x7f) return false;
    return true;
}

esp_err_t cc_link_set_server_host(const char *host, const char *fallback)
{
    if (!host_ok(host)) return ESP_ERR_INVALID_ARG;
    const char *fb = fallback && strcmp(fallback, "none") == 0 ? "" : fallback;
    if (fb && !ts_host_lan_fallback_valid(fb)) return ESP_ERR_INVALID_ARG;
    int idx = cc_settings_server();
    const server_t *sv = idx >= 0 ? server_store_get(idx) : NULL;
    if (!sv) return ESP_ERR_INVALID_STATE;
    lock();
    s_host_gen++;   /* M4: a tailnet connect in flight must not store the old host as the carry source */
    unlock();
    server_t e = *sv;
    /* the address whose pin carries over: the link's last tailnet address, else the old entry's */
    char tsip[sizeof(g_settings.mac_ts_ip)] = "";
    lock();
    if (tailscale_is_tailnet_ip(s_pin_host)) strlcpy(tsip, s_pin_host, sizeof(tsip));
    unlock();
    if (!tsip[0] && tailscale_is_tailnet_ip(e.host)) strlcpy(tsip, e.host, sizeof(tsip));
    if (!tsip[0]) tailscale_resolve(e.host, tsip, sizeof(tsip));
    /* the carry source moves with the entry (this is the one host edit it survives, M1): the saved key
       if it is for this entry, else the key pinned now under the tailnet address */
    carry_t c;
    lock();
    c = s_carry;
    unlock();
    /* only a key met in a verified tailnet session (N3): no matching saved key means none; the next
       tailnet connect captures it */
    bool have = c.host[0] && strcmp(c.host, e.host) == 0 && (!tsip[0] || strcmp(c.ip, tsip) == 0);
    strlcpy(e.host, host, sizeof(e.host));
    if (!server_store_update(idx, &e)) return ESP_FAIL;
    if (have) {
        strlcpy(c.host, host, sizeof(c.host));
        carry_store(&c);
    } else carry_store(NULL);
    lock();
    if (fb) strlcpy(g_settings.mac_lan_ip, fb, sizeof(g_settings.mac_lan_ip));
    s_lan_last[0] = 0;
    s_pin_host[0] = 0;   /* M2: the old host's address is no take-over target, even before the reconfig */
    s_ok_host[0] = 0;
    unlock();
    settings_save();
    ESP_LOGI(TAG, "Host entry host -> %s, fallback %s, carry source %s", host,
             g_settings.mac_lan_ip[0] ? g_settings.mac_lan_ip : "none", have ? c.ip : "none");
    s_reconfig = true;
    kick();
    return ESP_OK;
}

bool cc_link_set_lan_fallback(const char *ip)
{
    if (!ts_host_lan_fallback_valid(ip)) return false;
    lock();
    strlcpy(g_settings.mac_lan_ip, ip, sizeof(g_settings.mac_lan_ip));
    unlock();
    settings_save();
    s_reset_backoff = true;   /* try it now */
    kick();
    return true;
}

const char *cc_link_tailnet_pin_source(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, s_carry.ip, cap);
    unlock();
    return out;
}

const char *cc_link_lan_fallback(char *out, size_t cap)
{
    if (!out || !cap) return "";
    lock();
    strlcpy(out, g_settings.mac_lan_ip, cap);
    unlock();
    return out;
}

void cc_link_on_wifi(bool connected)
{
    if (connected) s_reset_backoff = true;
    else s_drop = true;
    kick();
}

int libssh_esp32_init(void);

/* cJSON nodes for a long `--all` list would otherwise land in internal RAM
   (SPIRAM_MALLOC_ALWAYSINTERNAL keeps small blocks there). */
static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

esp_err_t cc_link_start(void)
{
    if (s_task) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(req_t *));
    s_list = heap_caps_calloc(POLL_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_parse = heap_caps_calloc(POLL_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    s_screen = heap_caps_malloc(SCREEN_CAP, MALLOC_CAP_SPIRAM);
    s_wbody = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_queue || !s_list || !s_parse || !s_json || !s_screen || !s_wbody) return ESP_ERR_NO_MEM;
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);
    carry_load();
    hc_load();
    ssh_host_keys_init();   /* the deny-list's lock, before anything reads or adds */
    host_keys_migrate();
    libssh_esp32_init();   /* once only */
    static const lg_closer_t closer = { "cc_link", lg_link_close, lg_link_idle, lg_link_reopen, NULL };
    lg_register(&closer);   /* C1: every path that takes the link down closes it first */
    if (xTaskCreatePinnedToCore(link_task, "cc_link", 24576, NULL, 4, &s_task, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void cc_link_host_cache_text(char *out, size_t cap)
{
    hc_cache_t c;
    lock();
    c = s_hc;
    unlock();
    if (c.entry[0]) snprintf(out, cap, "host cache: %s -> %s%s", c.entry, c.ip, s_hc_off ? " (off)" : "");
    else snprintf(out, cap, "host cache: none%s", s_hc_off ? " (off)" : "");
}
void cc_link_host_cache_enable(bool on) { s_hc_off = !on; }
bool cc_link_host_cache_test_set(const char *ip)
{
    /* debug: point the cache at another address (RAM only) to exercise the fallback to the lookup */
    if (!ts_host_is_ipv4(ip)) return false;
    lock();
    bool ok = s_hc.entry[0] != 0;
    if (ok) strlcpy(s_hc.ip, ip, sizeof(s_hc.ip));
    unlock();
    return ok;
}
void cc_link_host_cache_clear(void)
{
    lock();
    memset(&s_hc, 0, sizeof(s_hc));
    unlock();
    nvs_erase_key_ns("cc", "hcache");
}
