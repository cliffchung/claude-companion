#include "cc_link.h"
#include "cc_cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "libssh/libssh.h"
#include "settings/settings.h"
#include "servers/server_store.h"
#include "net/tailscale.h"
#include "net/wifi_mgr.h"
#include "ssh/ssh_client.h"
#include "ssh/device_key.h"

/* Threading: s_task is the only task that calls libssh on s_sess. cc_link_exec and
 * cc_link_paste queue a request and block on a semaphore until the task answers, so
 * console and UI callers run nothing from libssh on their own (small) stacks. The task
 * serves requests between polls; while there is no session it answers them at once
 * with ESP_ERR_INVALID_STATE. Every wait inside the task is bounded by a deadline, so a
 * caller can never wait forever. s_lock guards the small shared state (status, the last
 * session list, the subscribers and the two settings strings). */

static const char *TAG = "cc_link";

#define POLL_CAP        32
#define JSON_CAP        (32 * 1024)
#define SCREEN_CAP      (16 * 1024)
#define POLL_TIMEOUT_MS 15000
#define PASTE_BUDGET_MS 12000
#define MAX_SUBS        4
#define DEFAULT_CLAUDE  "~/.local/bin/claude"

typedef enum { LS_NO_MAC, LS_CONNECTING, LS_WAITING, LS_CONNECTED } link_state_t;
typedef enum { REQ_EXEC, REQ_PASTE } req_type_t;

typedef struct {
    req_type_t type;
    const char *cmd;            /* exec command or attach command */
    const char *id;             /* paste: short session id */
    char *out; size_t cap;      /* exec stdout */
    int *exit_status;
    char *err; size_t err_cap;
    const char *payload; size_t payload_len;   /* paste bytes (ends with CR) */
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
static cc_session_t s_parse[POLL_CAP]; /* task only: the newest parse */

static const int s_backoff[] = { 1, 2, 4, 8, 16, 30 };

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static int64_t now_us(void) { return esp_timer_get_time(); }

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

const char *cc_settings_claude_path(void)
{
    return cc_path_valid(g_settings.claude_path) ? g_settings.claude_path : DEFAULT_CLAUDE;
}

const char *cc_settings_server_name(void)
{
    return g_settings.companion_server;
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

void cc_link_set_server(const char *name)
{
    lock();
    strlcpy(g_settings.companion_server, name ? name : "", sizeof(g_settings.companion_server));
    unlock();
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

/* ---------- status ---------- */

bool cc_link_connected(void)
{
    return s_state == LS_CONNECTED;
}

const char *cc_link_status_text(void)
{
    static char bufs[4][96];
    static int next;
    lock();
    char *b = bufs[next];
    next = (next + 1) % 4;
    switch (s_state) {
    case LS_NO_MAC:
        strlcpy(b, "no Mac set up", sizeof(bufs[0]));
        break;
    case LS_CONNECTING:
        snprintf(b, sizeof(bufs[0]), "connecting to %s", s_shown);
        break;
    case LS_WAITING: {
        int64_t left = s_retry_at_us - now_us();
        int sec = left > 0 ? (int)((left + 999999) / 1000000) : 0;
        snprintf(b, sizeof(bufs[0]), "connecting to %s (retry in %ds)", s_shown, sec);
        break;
    }
    case LS_CONNECTED:
        snprintf(b, sizeof(bufs[0]), "connected %s", s_shown);
        break;
    }
    unlock();
    return b;
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

static void teardown(void)
{
    if (s_sess) {
        ssh_disconnect(s_sess);
        ssh_free(s_sess);
        s_sess = NULL;
        ESP_LOGI(TAG, "link closed");
    }
    s_drop = false;
}

static bool connect_once(const char *name)
{
    int idx = server_index(name);
    const server_t *sv = idx >= 0 ? server_store_get(idx) : NULL;
    if (!sv) {
        ESP_LOGW(TAG, "no server named '%s' in the server list", name);
        return false;
    }
    char host[128], user[64];
    int port = sv->port > 0 ? sv->port : 22;
    strlcpy(host, sv->host, sizeof(host));
    strlcpy(user, sv->user, sizeof(user));
    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "no Wi-Fi");
        return false;
    }
    char ip[16];
    if (tailscale_resolve(host, ip, sizeof(ip))) strlcpy(host, ip, sizeof(host));
    if (tailscale_is_tailnet_ip(host) && !tailscale_prepare_peer(host, 20000))
        ESP_LOGW(TAG, "tunnel to %s not confirmed, trying anyway", host);

    ssh_session s = ssh_new();
    if (!s) return false;
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
        ssh_free(s);
        return false;
    }
    char fp[80];
    kh_result_t kh = ssh_known_host_check(s, host, port, fp, sizeof(fp));
    if (kh == KH_UNKNOWN) {
        /* First contact with no screen to ask on: pin it, like the terminal's default. */
        ESP_LOGW(TAG, "pinning new host key for %s:%d %s", host, port, fp);
        if (!ssh_known_host_trust(s, host, port)) kh = KH_ERROR;
        else kh = KH_OK;
    }
    if (kh != KH_OK) {
        ESP_LOGE(TAG, "host key %s for %s:%d (%s); not connecting",
                 kh == KH_CHANGED ? "CHANGED" : "unreadable", host, port, fp);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    ssh_key key = NULL;
    int rc = SSH_AUTH_ERROR;
    if (device_key_ensure() == ESP_OK && device_key_get(&key) == ESP_OK) {
        int n = 0;
        do {
            rc = ssh_userauth_publickey(s, NULL, key);
            if (rc == SSH_AUTH_AGAIN) vTaskDelay(pdMS_TO_TICKS(100));
        } while (rc == SSH_AUTH_AGAIN && ++n < 200);
        ssh_key_free(key);
    }
    if (rc != SSH_AUTH_SUCCESS) {
        ESP_LOGW(TAG, "device key auth failed: %s", ssh_get_error(s));
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    s_sess = s;
    s_drop = false;
    ESP_LOGI(TAG, "connected to %s (%s:%d)", name, host, port);
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

static esp_err_t do_exec(req_t *r)
{
    if (r->out && r->cap) r->out[0] = 0;
    if (r->err && r->err_cap) r->err[0] = 0;
    if (r->exit_status) *r->exit_status = -1;
    ssh_channel ch = open_channel();
    if (!ch) return ESP_ERR_INVALID_STATE;
    if (ssh_channel_request_exec(ch, r->cmd) != SSH_OK) {
        ESP_LOGW(TAG, "exec request failed: %s", ssh_get_error(s_sess));
        close_channel(ch);
        s_drop = true;
        return ESP_FAIL;
    }
    size_t olen = 0, elen = 0;
    bool truncated = false, failed = false, timed_out = false;
    char tmp[512];
    for (;;) {
        int n = ssh_channel_read_timeout(ch, tmp, sizeof(tmp), 0, 50);
        if (n == SSH_ERROR) { failed = true; break; }
        if (n > 0) {
            size_t k = (size_t)n;
            if (r->out && r->cap > 1) {
                if (olen + k > r->cap - 1) { k = r->cap - 1 - olen; truncated = true; }
                memcpy(r->out + olen, tmp, k);
                olen += k;
                r->out[olen] = 0;
            }
        }
        read_stderr(ch, r->err, r->err_cap, &elen);
        if (n == 0 && ssh_channel_is_eof(ch)) break;
        if (now_us() > r->deadline_us) { timed_out = true; break; }
    }
    if (!failed && !timed_out && r->exit_status) {
        uint32_t code = 0;
        char *sig = NULL;
        int core = 0;
        if (ssh_channel_get_exit_state(ch, &code, &sig, &core) == SSH_OK) *r->exit_status = (int)code;
        ssh_string_free_char(sig);
    }
    close_channel(ch);
    if (r->err && r->err_cap) first_line(r->err);
    if (failed) { s_drop = true; return ESP_FAIL; }
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

static int refresh(void);

static esp_err_t do_paste(req_t *r)
{
    if (r->err && r->err_cap) r->err[0] = 0;
    /* `claude attach` wakes a stopped session, so check it is still live right now,
       not as of the last poll. */
    int n = refresh();
    if (n < 0) {
        strlcpy(r->err, "could not check the session", r->err_cap);
        return ESP_FAIL;
    }
    const cc_session_t *hit = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(s_parse[i].id, r->id) == 0) { hit = &s_parse[i]; break; }
    }
    if (!hit) {
        strlcpy(r->err, "no such session", r->err_cap);
        return ESP_ERR_NOT_FOUND;
    }
    /* A stopped entry stays listed (often still "done") but has no pid. */
    if (hit->state == CC_ENDED || !hit->live || !hit->background) {
        strlcpy(r->err, hit->background ? "session has stopped" : "not a background session", r->err_cap);
        return ESP_ERR_NOT_FOUND;
    }
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
    /* 2. a permission dialog takes CR as "yes": type nothing, detach */
    if (cc_screen_is_permission_prompt(s_screen)) {
        size_t n = strlen(s_screen);
        ESP_LOGD(TAG, "permission screen tail: %s", s_screen + (n > 300 ? n - 300 : 0));
        send_bytes(ch, "\x1a", 1);
        len = 0;
        pump(ch, &len, 1000, 0);
        close_channel(ch);
        strlcpy(r->err, "needs approval \xe2\x80\x94 open the session", r->err_cap);
        return ESP_ERR_NOT_ALLOWED;
    }
    len = 0;
    /* 3. clear any draft, 4. bracketed paste, then CR on its own */
    bool ok = send_bytes(ch, "\x15\x15\x15\x15", 4) && pump(ch, &len, 200, 0) &&
              send_bytes(ch, r->payload, r->payload_len - 1) && pump(ch, &len, 150, 0) &&
              send_bytes(ch, r->payload + r->payload_len - 1, 1);
    /* 5. let it submit, 6. detach with Ctrl+Z (never ← or a bare ESC) */
    if (ok) pump(ch, &len, 1000, 0);
    send_bytes(ch, "\x1a", 1);
    pump(ch, &len, 1000, 0);
    close_channel(ch);
    if (!ok) {
        strlcpy(r->err, "link lost while typing", r->err_cap);
        s_drop = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void handle(req_t *r)
{
    if (!s_sess || s_drop) { answer(r, ESP_ERR_INVALID_STATE); return; }
    if (now_us() > r->deadline_us) { answer(r, ESP_ERR_TIMEOUT); return; }
    answer(r, r->type == REQ_EXEC ? do_exec(r) : do_paste(r));
}

/* Runs `claude agents --json --all`, parses it into s_parse and publishes it.
   Returns the count, or -1 on failure. */
static int refresh(void)
{
    char cmd[128];
    if (!cc_cmd_agents(cc_settings_claude_path(), cmd, sizeof(cmd))) return -1;
    int status = -1;
    char err[160];
    req_t r = { .type = REQ_EXEC, .cmd = cmd, .out = s_json, .cap = JSON_CAP, .exit_status = &status,
                .err = err, .err_cap = sizeof(err), .deadline_us = now_us() + (int64_t)POLL_TIMEOUT_MS * 1000 };
    esp_err_t e = do_exec(&r);
    if (e != ESP_OK || status != 0) {
        ESP_LOGW(TAG, "poll failed: %s, exit %d: %s", esp_err_to_name(e), status, err);
        return -1;
    }
    int n = cc_sessions_parse(s_json, s_parse, POLL_CAP);
    if (n < 0) {
        ESP_LOGW(TAG, "poll: output is not a JSON array");
        return -1;
    }
    struct { cc_sessions_cb_t cb; void *ctx; } subs[MAX_SUBS];
    lock();
    memcpy(s_list, s_parse, sizeof(cc_session_t) * n);
    s_list_n = n;
    memcpy(subs, s_subs, sizeof(subs));
    unlock();
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].cb) subs[i].cb(s_parse, n, subs[i].ctx);
    }
    return n;
}

static void do_poll(void)
{
    refresh();
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
            unlock();
        }
        if (s_reset_backoff) { s_reset_backoff = false; attempt = 0; }
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
            if (connect_once(name)) {
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
            while (now_us() < s_retry_at_us && !s_reconfig && !s_reset_backoff) {
                int64_t left_ms = (s_retry_at_us - now_us()) / 1000;
                drain_queue_down(pdMS_TO_TICKS(left_ms > 0 ? left_ms : 1));
            }
            continue;
        }
        /* connected */
        int rate = s_rate;
        int64_t t = now_us();
        if (rate > 0 && t - last_poll >= (int64_t)rate * 1000000) {
            do_poll();
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

static esp_err_t submit(req_t *r)
{
    if (!s_queue || !cc_link_connected()) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCurrentTaskHandle() == s_task) {   /* a subscriber calling back in */
        if (!s_sess || s_drop) return ESP_ERR_INVALID_STATE;
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

esp_err_t cc_link_paste(const char *id, const char *text, char *err, size_t err_cap)
{
    char local_err[8];
    if (!err || !err_cap) { err = local_err; err_cap = sizeof(local_err); }
    err[0] = 0;
    char cmd[128];
    if (!cc_cmd_attach(cc_settings_claude_path(), id, cmd, sizeof(cmd))) {
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
    esp_err_t e = submit(&r);
    free(payload);
    return e;
}

void cc_poll_set_rate(int seconds)
{
    s_rate = seconds < 0 ? 0 : seconds;
    kick();
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

void cc_link_on_wifi(bool connected)
{
    if (connected) s_reset_backoff = true;
    else s_drop = true;
    kick();
}

int libssh_esp32_init(void);

esp_err_t cc_link_start(void)
{
    if (s_task) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(req_t *));
    s_list = heap_caps_calloc(POLL_CAP, sizeof(cc_session_t), MALLOC_CAP_SPIRAM);
    s_json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    s_screen = heap_caps_malloc(SCREEN_CAP, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_queue || !s_list || !s_json || !s_screen) return ESP_ERR_NO_MEM;
    libssh_esp32_init();   /* ssh_init is reference counted */
    if (xTaskCreatePinnedToCore(link_task, "cc_link", 24576, NULL, 4, &s_task, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
