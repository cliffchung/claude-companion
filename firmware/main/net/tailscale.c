#include "tailscale.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "microlink.h"
#include "microlink_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip.h"
#include <errno.h>
#include "settings/settings.h"
#include "net/wifi_mgr.h"
#include "net/ts_host.h"

static const char *TAG = "tailscale";
static microlink_t *s_ml = NULL;
static ts_state_t s_state = TS_OFF;
static ts_event_cb_t s_cb; static void *s_cb_user;
static char s_ip[16] = "";
static char s_err[96] = "";
static bool s_started = false;
static volatile bool s_fast_resume;

/* Lifecycle (Plan 3 Task 1, fix round 1: L15). tailscale.c is MicroLink's only owner. s_ctl
   serializes init, start, the stop hand-off and the free (s_ml, s_started, s_zombie, s_stopping,
   s_worker, s_hold); it is never held across a long wait. A caller that uses the instance outside
   s_ctl (a check-in's handshake wait, the tunnel wait before a connect) takes a reference with
   ml_get() and drops it with ml_put(), and stops waiting once s_ml is no longer its instance.
   A switch-off detaches the instance in one locked step (s_ml = NULL, s_zombie = it, s_stopping)
   and hands microlink_stop (up to 15 s) and the free to a short-lived worker task, so the caller
   (the LVGL task, the console) never waits. While a zombie exists nothing starts a second
   instance: a start is refused, and the worker starts MicroLink itself once the zombie is freed
   if the switch was turned on again meanwhile. */
static SemaphoreHandle_t s_ctl;
static int s_users;                 /* references taken by ml_get (atomic) */
static microlink_t *s_zombie;       /* detached: stopping or stopped, not yet freed */
static bool s_stopping;             /* microlink_stop hasn't returned for s_zombie yet */
static TaskHandle_t s_worker;       /* the stop-and-free worker, while it runs */
static bool s_hold;                 /* tailscale_forget_node runs: nothing starts */

static void ctl_lock(void) { if (s_ctl) xSemaphoreTake(s_ctl, portMAX_DELAY); }
static void ctl_unlock(void) { if (s_ctl) xSemaphoreGive(s_ctl); }

static microlink_t *ml_get(void)
{
    ctl_lock();
    microlink_t *ml = g_settings.ts_enable ? s_ml : NULL;
    if (ml) __atomic_add_fetch(&s_users, 1, __ATOMIC_SEQ_CST);
    ctl_unlock();
    return ml;
}
static void ml_put(microlink_t *ml) { if (ml) __atomic_sub_fetch(&s_users, 1, __ATOMIC_SEQ_CST); }
/* still the live instance (the switch wasn't turned off meanwhile) */
static bool ml_live(const microlink_t *ml) { return ml && ml == s_ml && g_settings.ts_enable; }
/* microlink_rebind on ml only while it is still live, checked under s_ctl (M6) */
static bool rebind_if_live(microlink_t *ml)
{
    ctl_lock();
    bool live = ml_live(ml);
    if (live) microlink_rebind(ml);
    ctl_unlock();
    return live;
}

static void notify(void) { if (s_cb) s_cb(s_cb_user); }

/* ctl held */
static bool reap_locked(void)
{
    if (!s_zombie) return true;
    if (s_stopping) return false;   /* microlink_stop still runs on it */
    if (__atomic_load_n(&s_users, __ATOMIC_SEQ_CST) > 0) return false;
    if (!microlink_destroy_finish(s_zombie)) return false;
    s_zombie = NULL;
    ESP_LOGI(TAG, "the stopped MicroLink instance is freed");
    return true;
}

static bool start_locked(void);
static void worker_task(void *arg);

/* ctl held. Starts the stop worker for a detached instance if none runs; a failed create is retried by
   the next start, switch-off or forget (N2), so the instance is never left without someone to stop it. */
static void ensure_worker_locked(void)
{
    if (!s_zombie || s_worker) return;
    if (xTaskCreate(worker_task, "ts_stop", 6144, NULL, 3, &s_worker) != pdPASS) {
        s_worker = NULL;
        ESP_LOGE(TAG, "no memory for the stop worker; retried on the next start, switch-off or forget");
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    ctl_lock();
    microlink_t *z = s_zombie;
    bool stop = s_stopping;
    ctl_unlock();
    if (z && stop) microlink_stop(z);   /* up to 15 s, outside s_ctl */
    ctl_lock();
    s_stopping = false;
    ctl_unlock();
    bool done = false;
    for (int i = 0; i < 600 && !done; i++) {   /* 300 s */
        ctl_lock();
        done = reap_locked();
        /* review M5: a late state callback that raced teardown can't leave "Connected" behind */
        if (done && !s_ml) { s_state = TS_OFF; s_ip[0] = 0; }
        if (done && g_settings.ts_enable && !s_hold && !s_started && wifi_mgr_is_connected()) {
            ESP_LOGI(TAG, "switched on again while stopping: starting now");
            start_locked();
        }
        /* cleared in the same locked step that saw the result (N1): a switch-off after this point
           sees no worker and starts a new one for its instance */
        if (done || i == 599) s_worker = NULL;
        ctl_unlock();
        if (!done) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!done) ESP_LOGE(TAG, "a stopped MicroLink instance is still in use after 300 s: it stays allocated");
    notify();
    vTaskDelete(NULL);
}

static ts_stop_hook_t s_stop_hook;
void tailscale_set_stop_hook(ts_stop_hook_t hook) { s_stop_hook = hook; }

static void teardown_now(void *ctx);
/* Detaches the live instance and hands its stop and free to the worker. Its node keys stay in NVS, so
   starting again reuses the registered node. C1: with an instance, the stop hook first closes the SSH
   sessions over the tunnel (bounded, about 2 s) while it still carries their close to the Host. */
static void teardown(void)
{
    ctl_lock();
    bool live = s_ml != NULL;
    ctl_unlock();
    if (live && s_stop_hook) s_stop_hook(teardown_now, NULL);
    else teardown_now(NULL);
}

static void teardown_now(void *ctx)
{
    (void)ctx;
    ctl_lock();
    microlink_t *ml = s_ml;
    bool was_started = s_started;
    s_ml = NULL;   /* from here its callbacks are ignored and ml_live() is false */
    s_started = false; s_state = TS_OFF; s_ip[0] = 0;
    if (ml) {
        if (s_zombie) ESP_LOGE(TAG, "a second instance detached while the first isn't freed: the first stays allocated");
        s_zombie = ml;   /* published at once: nothing starts a second instance meanwhile (I1) */
        s_stopping = was_started;
    }
    ensure_worker_locked();
    ctl_unlock();
}

bool tailscale_stopping(void) { return s_zombie != NULL; }

static void on_state(microlink_t *ml, microlink_state_t st, void *user)
{
    (void)user;
    if (ml != s_ml || !g_settings.ts_enable) return;   /* a stopped instance's last task: the switch is off */
    switch (st) {
    case ML_STATE_IDLE: s_state = TS_OFF; break;
    case ML_STATE_WIFI_WAIT: s_state = TS_WAIT_WIFI; break;
    case ML_STATE_CONNECTING: s_state = TS_CONNECTING; break;
    case ML_STATE_REGISTERING: s_state = TS_REGISTERING; break;
    case ML_STATE_CONNECTED: s_state = TS_CONNECTED; microlink_ip_to_str(microlink_get_vpn_ip(ml), s_ip); ESP_LOGI(TAG, "connected, VPN IP %s", s_ip); break;
    case ML_STATE_RECONNECTING: s_state = TS_RECONNECTING; break;
    default: s_state = TS_ERROR; strlcpy(s_err, "MicroLink error", sizeof(s_err)); break;
    }
    if (st != ML_STATE_CONNECTED) s_ip[0] = 0;
    notify();
}
static void on_peer(microlink_t *ml, const microlink_peer_info_t *p, void *user) { (void)p; (void)user; if (ml == s_ml) notify(); }

/* Auth key: the saved setting only (spec U6: nothing reads the SD card at start any more; Settings ›
   Network › Auth key › Load from SD card copies the file once, on request). Never logged. */
static bool load_auth_key(char *out, size_t cap)
{
    if (!g_settings.ts_auth_key[0]) return false;
    strlcpy(out, g_settings.ts_auth_key, cap);
    return true;
}

static char s_key[160], s_name[40], s_ctrl[96];

bool tailscale_enabled(void) { return g_settings.ts_enable != 0; }

/* ctl held */
static bool start_locked(void)
{
    /* the switch is off: nothing reaches microlink_init or microlink_start */
    if (!g_settings.ts_enable) { strlcpy(s_err, "Tailscale is off", sizeof(s_err)); return false; }
    if (s_started) return true;
    if (s_hold) return false;   /* forgetting the node */
    ensure_worker_locked();   /* a worker that couldn't be created before (N2) */
    if (!reap_locked()) {
        /* the worker starts it once the old instance is freed */
        strlcpy(s_err, "Tailscale is still turning off; it starts when that is done", sizeof(s_err));
        ESP_LOGW(TAG, "start: the stopped instance isn't freed yet; the stop worker starts it after");
        return false;
    }
    if (!load_auth_key(s_key, sizeof(s_key))) {
        /* A node that already registered can start without a key (keys are kept in NVS by MicroLink). */
        s_key[0] = 0;
    }
    strlcpy(s_name, g_settings.ts_device_name[0] ? g_settings.ts_device_name : "claude-companion", sizeof(s_name));
    strlcpy(s_ctrl, g_settings.ts_ctrl_host, sizeof(s_ctrl));
    microlink_config_t cfg = {
        .auth_key = s_key[0] ? s_key : NULL,
        .device_name = s_name,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = g_settings.ts_direct ? true : false,   /* false = relay-only (reliable), true = try direct UDP */
        .max_peers = 16,
        .netcheck_override_enabled = true,      /* start on the lowest-RTT DERP region */
        .netcheck_override_threshold_ms = 30,
        .ctrl_host = s_ctrl[0] ? s_ctrl : NULL,
    };
    if (!s_ml) {
        s_ml = microlink_init(&cfg);
        if (!s_ml) { strlcpy(s_err, "microlink_init failed", sizeof(s_err)); s_state = TS_ERROR; notify(); return false; }
        microlink_set_state_callback(s_ml, on_state, NULL);
        microlink_set_peer_callback(s_ml, on_peer, NULL);
    }
    esp_err_t e = microlink_start(s_ml);
    if (e != ESP_OK) { snprintf(s_err, sizeof(s_err), "start failed: %s", esp_err_to_name(e)); s_state = TS_ERROR; notify(); return false; }
    s_started = true;
    s_state = wifi_mgr_is_connected() ? TS_CONNECTING : TS_WAIT_WIFI;
    notify();
    return true;
}

bool tailscale_start(void)
{
    ctl_lock();
    bool ok = start_locked();
    ctl_unlock();
    return ok;
}

void tailscale_stop(void)
{
    teardown();   /* the worker stops and frees it */
    notify();
}

void tailscale_set_enabled(bool on)
{
    g_settings.ts_enable = on ? 1 : 0;
    settings_save();   /* sticky: every boot and wake reads it */
    ESP_LOGI(TAG, "switch %s", on ? "on" : "off");
    if (on) {
        if (wifi_mgr_is_connected()) tailscale_start();
        else notify();   /* starts on the next Wi-Fi connect */
        return;
    }
    teardown();   /* returns at once: the worker stops and frees it (I3) */
    s_err[0] = 0;
    notify();
}

void tailscale_on_wifi(bool connected)
{
    if (!g_settings.ts_enable || !s_ctl) return;   /* the switch is off: no start, no rebind */
    ctl_lock();
    if (!g_settings.ts_enable) { ctl_unlock(); return; }   /* switched off meanwhile (M6) */
    if (connected && !s_started) start_locked();
    else if (connected && !s_fast_resume) microlink_rebind(s_ml);
    else if (connected) ESP_LOGI(TAG, "Wi-Fi back: fast resume, no rebind (the check-in handshakes first)");
    ctl_unlock();
}

void tailscale_set_fast_resume(bool on) { s_fast_resume = on; }
bool tailscale_fast_resume(void) { return s_fast_resume; }

const char *tailscale_checkin_name(ts_checkin_t c)
{
    return c == TS_CHECKIN_HANDSHAKE ? "handshake" : c == TS_CHECKIN_REBIND ? "rebind" : c == TS_CHECKIN_SKIPPED ? "lan" : "failed";
}

#define CHECKIN_CONNECT_MS 60000
#define CHECKIN_HANDSHAKE_MS 8000
#define CHECKIN_REBIND_MS 60000

static uint32_t ms_since(int64_t t0_us) { return (uint32_t)((esp_timer_get_time() - t0_us) / 1000); }

/* One handshake + CMM, then up to wait_ms for the peer. */
static bool handshake_wait(microlink_t *ml, uint32_t dest, uint32_t wait_ms)
{
    if (ml_wg_mgr_peer_is_up(ml, dest)) return true;
    ml_wg_mgr_trigger_handshake(ml, dest);
    ml_wg_mgr_send_cmm(ml, dest);
    for (uint32_t waited = 0; waited < wait_ms && ml_live(ml); waited += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (ml_wg_mgr_peer_is_up(ml, dest)) return true;
        if (waited && waited % 4000 == 0) { ml_wg_mgr_trigger_handshake(ml, dest); ml_wg_mgr_send_cmm(ml, dest); }
    }
    return false;
}

ts_checkin_t tailscale_checkin(const char *host, bool force_rebind, uint32_t *ms_out)
{
    int64_t t0 = esp_timer_get_time();
    ts_checkin_t r = TS_CHECKIN_FAILED;
    char ip[16] = "";
    microlink_t *ml = NULL;
    if (!host || !host[0]) goto done;
    /* no Tailscale, or a host that can't be on the tailnet (a LAN address, a .local name): nothing to
       bring up, so no start and no connect wait */
    if (!g_settings.ts_enable || !ts_host_maybe_tailnet(host)) {
        r = TS_CHECKIN_SKIPPED;
        goto done;
    }
    if (tailscale_stopping()) {
        /* an instance is still stopping: no tunnel until it is freed, so don't wait 60 s for one (N5) */
        ESP_LOGW(TAG, "check-in: Tailscale is still turning off or restarting; no tunnel this time");
        goto done;
    }
    if (!s_started && wifi_mgr_is_connected()) tailscale_start();
    while (s_state != TS_CONNECTED && g_settings.ts_enable && ms_since(t0) < CHECKIN_CONNECT_MS) vTaskDelay(pdMS_TO_TICKS(250));
    ml = s_state == TS_CONNECTED ? ml_get() : NULL;
    if (!ml) {
        ESP_LOGW(TAG, "check-in: MicroLink not connected after %u ms (%s)", (unsigned)ms_since(t0), tailscale_state_text());
        goto done;
    }
    if (microlink_parse_ip(host)) strlcpy(ip, host, sizeof(ip));
    else if (!tailscale_resolve(host, ip, sizeof(ip))) {
        ESP_LOGW(TAG, "check-in: %s doesn't resolve on the tailnet", host);
        goto done;
    }
    if (!tailscale_is_tailnet_ip(ip)) { r = TS_CHECKIN_SKIPPED; goto done; }
    uint32_t dest = microlink_parse_ip(ip);
    if (!force_rebind && handshake_wait(ml, dest, CHECKIN_HANDSHAKE_MS)) { r = TS_CHECKIN_HANDSHAKE; goto done; }
    ESP_LOGW(TAG, "check-in: %s, rebinding", force_rebind ? "rebind forced" : "no handshake in 8 s");
    if (!rebind_if_live(ml)) goto done;   /* switched off meanwhile */
    EventBits_t bits = 0;
    for (uint32_t w = 0; w < CHECKIN_REBIND_MS && ml_live(ml); w += 500) {
        bits = xEventGroupWaitBits(ml->events, ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
        if ((bits & (ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED)) == (ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED)) break;
    }
    ESP_LOGI(TAG, "check-in: rebind: coord %s, derp %s after %u ms", (bits & ML_EVT_COORD_REGISTERED) ? "registered" : "NOT registered",
             (bits & ML_EVT_DERP_CONNECTED) ? "connected" : "NOT connected", (unsigned)ms_since(t0));
    if (ml_live(ml) && handshake_wait(ml, dest, CHECKIN_HANDSHAKE_MS)) r = TS_CHECKIN_REBIND;
done:
    ml_put(ml);
    if (ms_out) *ms_out = ms_since(t0);
    ESP_LOGI(TAG, "check-in to %s: %s in %u ms", ip[0] ? ip : (host ? host : "?"), tailscale_checkin_name(r), (unsigned)ms_since(t0));
    return r;
}

void tailscale_kick_peer(const char *ip)
{
    if (s_state != TS_CONNECTED || !ip || !tailscale_is_tailnet_ip(ip)) return;
    microlink_t *ml = ml_get();
    if (!ml) return;
    uint32_t dest = microlink_parse_ip(ip);
    ml_wg_mgr_trigger_handshake(ml, dest);
    ml_wg_mgr_send_cmm(ml, dest);
    ml_put(ml);
    ESP_LOGI(TAG, "kicked a handshake to %s", ip);
}

void tailscale_init(void)
{
    if (!s_ctl) s_ctl = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "switch %s (saved)", g_settings.ts_enable ? "on" : "off");
    if (g_settings.ts_enable && wifi_mgr_is_connected()) tailscale_start();
}

ts_state_t tailscale_state(void) { return s_state; }
const char *tailscale_state_text(void)
{
    static const char *n[] = { "Off", "Waiting for WiFi", "Connecting", "Registering", "Connected", "Reconnecting", "Error" };
    if (s_zombie && !s_ml) return g_settings.ts_enable ? "Starting after the stop" : "Turning off";
    return n[s_state < 7 ? s_state : 6];
}
const char *tailscale_vpn_ip(void) { return s_ip; }
int tailscale_peer_count(void)
{
    microlink_t *ml = ml_get();
    int n = ml ? microlink_get_peer_count(ml) : 0;
    ml_put(ml);
    return n;
}
bool tailscale_peer(int i, ts_peer_t *out)
{
    microlink_peer_info_t p;
    microlink_t *ml = ml_get();
    bool ok = ml && microlink_get_peer_info(ml, i, &p) == ESP_OK;
    ml_put(ml);
    if (!ok) return false;
    strlcpy(out->hostname, p.hostname, sizeof(out->hostname));
    microlink_ip_to_str(p.vpn_ip, out->ip);
    out->online = p.online; out->direct = p.direct_path; out->exit_node = p.is_exit_node; out->derp_region = p.derp_region;
    return true;
}
bool tailscale_resolve(const char *host, char *ip_out, size_t cap)
{
    if (s_state != TS_CONNECTED) return false;
    microlink_t *ml = ml_get();
    uint32_t ip = ml ? microlink_resolve(ml, host) : 0;
    ml_put(ml);
    if (!ip) return false;
    char b[16]; microlink_ip_to_str(ip, b); strlcpy(ip_out, b, cap);
    return true;
}
bool tailscale_is_tailnet_ip(const char *ip)
{
    return ts_host_tailnet_ip(ip);   /* 100.64.0.0/10 */
}

static bool prepare_peer(microlink_t *ml, const char *ip, uint32_t dest, uint32_t timeout_ms);

bool tailscale_prepare_peer(const char *ip, uint32_t timeout_ms)
{
    uint32_t dest = microlink_parse_ip(ip);
    if (!dest || s_state != TS_CONNECTED) return false;
    microlink_t *ml = ml_get();   /* stops waiting once the switch turns off */
    if (!ml) return false;
    bool up = prepare_peer(ml, ip, dest, timeout_ms);
    ml_put(ml);
    return up;
}

static bool prepare_peer(microlink_t *ml, const char *ip, uint32_t dest, uint32_t timeout_ms)
{
    /* MicroLink keeps a single DERP connection (its own home region). Tailscale
     * relays through the *peer's* home region, so for a peer without a direct
     * path we move our DERP connection to that region first. */
    /* A tunnel that is already up (the companion link uses it) needs nothing: moving our
     * DERP region would re-register, take ~20 s and disturb the connections on it. */
    if (ml_wg_mgr_peer_is_up(ml, dest)) {
        ESP_LOGI(TAG, "peer %s tunnel already up", ip);
        return true;
    }
    int n = microlink_get_peer_count(ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t p;
        if (microlink_get_peer_info(ml, i, &p) != ESP_OK || p.vpn_ip != dest) continue;
        ESP_LOGI(TAG, "peer %s: online=%d direct=%d home DERP %u (ours %u, advertised %u)", p.hostname, p.online, p.direct_path, p.derp_region, ml->derp_home_region, ml->derp_region_default);
        if (!p.direct_path && p.derp_region > 0 && p.derp_region != ml->derp_home_region) {
            ESP_LOGW(TAG, "peer %s home DERP %u != ours %u: switching DERP region", p.hostname, p.derp_region, ml->derp_home_region);
            ml->derp_region_default = p.derp_region;
            ml->derp_home_region = p.derp_region;
            /* Reconnect DERP *and* re-register with the control plane so the
             * new PreferredDERP reaches the peers immediately; they relay to
             * whatever region we advertise. */
            if (!rebind_if_live(ml)) return false;   /* switched off meanwhile */
            EventBits_t bits = 0;
            for (int w = 0; w < 20000 && ml_live(ml); w += 500) {
                bits = xEventGroupWaitBits(ml->events, ML_EVT_DERP_CONNECTED | ML_EVT_COORD_REGISTERED, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
                if ((bits & (ML_EVT_DERP_CONNECTED | ML_EVT_COORD_REGISTERED)) == (ML_EVT_DERP_CONNECTED | ML_EVT_COORD_REGISTERED)) break;
            }
            ESP_LOGI(TAG, "DERP region %u: derp %s, coord %s", p.derp_region,
                     (bits & ML_EVT_DERP_CONNECTED) ? "connected" : "NOT connected", (bits & ML_EVT_COORD_REGISTERED) ? "registered" : "NOT registered");
            vTaskDelay(pdMS_TO_TICKS(2000));   /* let the peer pick up the new map */
        }
        break;
    }
    ml_wg_mgr_trigger_handshake(ml, dest);
    ml_wg_mgr_send_cmm(ml, dest);
    uint32_t waited = 0;
    bool up = ml_wg_mgr_peer_is_up(ml, dest);
    while (!up && waited < timeout_ms && ml_live(ml)) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
        up = ml_wg_mgr_peer_is_up(ml, dest);
        if (!up && (waited % 5000) == 0) { ml_wg_mgr_trigger_handshake(ml, dest); ml_wg_mgr_send_cmm(ml, dest); }
    }
    ESP_LOGI(TAG, "peer %s tunnel %s after %lu ms", ip, up ? "up" : "NOT up", (unsigned long)waited);
    return up;
}

void tailscale_tcp_probe(const char *ip, int port, int timeout_ms)
{
    ip4_addr_t dst; ip4addr_aton(ip, &dst);
    for (struct netif *n = netif_list; n; n = n->next) {
        ESP_LOGI(TAG, "netif %c%c%d up=%d link=%d ip=%s mask=%s mtu=%d", n->name[0], n->name[1], n->num,
                 netif_is_up(n), netif_is_link_up(n), ip4addr_ntoa(netif_ip4_addr(n)), ip4addr_ntoa(netif_ip4_netmask(n)), n->mtu);
    }
    struct netif *r = ip4_route(&dst);
    ESP_LOGI(TAG, "route %s -> %s", ip, r ? (char[]){ r->name[0], r->name[1], (char)('0' + r->num), 0 } : "(none)");
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { ESP_LOGE(TAG, "probe socket failed errno=%d", errno); return; }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) }; sa.sin_addr.s_addr = dst.addr;
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) { ESP_LOGE(TAG, "probe connect failed errno=%d", errno); close(fd); return; }
    fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    rc = select(fd + 1, NULL, &wf, NULL, &tv);
    int err = 0; socklen_t el = sizeof(err); getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (rc > 0 && err == 0) {
        char banner[96] = { 0 };
        struct timeval rt = { .tv_sec = 5 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
        fcntl(fd, F_SETFL, fl);
        int n = recv(fd, banner, sizeof(banner) - 1, 0);
        ESP_LOGI(TAG, "probe %s:%d TCP CONNECTED; banner(%d)=%s", ip, port, n, n > 0 ? banner : "(none)");
    } else ESP_LOGE(TAG, "probe %s:%d TCP FAILED select=%d so_error=%d", ip, port, rc, err);
    close(fd);
}

void tailscale_set_event_cb(ts_event_cb_t cb, void *user) { s_cb = cb; s_cb_user = user; }
bool tailscale_forgetting(void) { return s_hold; }

bool tailscale_forget_node(void)
{
    ctl_lock();
    if (s_hold) {   /* a second Forget while one runs: nothing to do (N4) */
        ctl_unlock();
        ESP_LOGW(TAG, "forget: already forgetting");
        return false;
    }
    s_hold = true;   /* no start (Wi-Fi event, check-in, worker) until the reset is done */
    ctl_unlock();
    teardown();
    notify();
    /* the factory reset only after the old instance is freed (I1) */
    bool freed = false;
    for (int i = 0; i < 400 && !freed; i++) {   /* 40 s: a 15 s stop plus a stuck task's last read */
        ctl_lock();
        ensure_worker_locked();
        freed = !s_zombie && !s_worker;
        ctl_unlock();
        if (!freed) vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool ok = freed && microlink_factory_reset() == ESP_OK;
    if (!freed) ESP_LOGE(TAG, "forget: the old instance isn't freed after 40 s; node keys kept");
    ctl_lock();
    s_hold = false;
    ctl_unlock();
    return ok;
}
const char *tailscale_last_error(void) { return s_err; }
bool tailscale_home_derp_code(char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = 0;
    microlink_t *ml = ml_get();
    uint16_t id = ml ? ml->derp_home_region : 0;
    for (int i = 0; ml && id && i < ml->derp_region_count; i++)
        if (ml->derp_regions[i].region_id == id && ml->derp_regions[i].code[0]) {
            strlcpy(out, ml->derp_regions[i].code, cap);
            break;
        }
    ml_put(ml);
    if (!out[0] && id) snprintf(out, cap, "%u", (unsigned)id);
    return out[0] != 0;
}
uint16_t tailscale_home_derp(void)
{
    microlink_t *ml = ml_get();
    uint16_t r = ml ? ml->derp_home_region : 0;
    ml_put(ml);
    return r;
}
