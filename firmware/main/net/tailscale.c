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
#include "storage/sd.h"
#include "net/ts_host.h"

static const char *TAG = "tailscale";
static microlink_t *s_ml = NULL;
static ts_state_t s_state = TS_OFF;
static ts_event_cb_t s_cb; static void *s_cb_user;
static char s_ip[16] = "";
static char s_err[96] = "";
static bool s_started = false;
static volatile bool s_fast_resume;

/* Lifecycle (Plan 3 Task 1). tailscale.c is MicroLink's only owner. s_ctl serializes init, start,
   stop and free (s_ml, s_started, s_zombie); it is never held across a long wait. A caller that
   uses the instance outside s_ctl (a check-in's handshake wait, the tunnel wait before a connect)
   takes a reference with ml_get() and drops it with ml_put(), and stops waiting once s_ml is no
   longer its instance. The switch-off frees the instance only when no reference is left and every
   MicroLink task has exited; until then it is a "zombie" a reaper task frees, and nothing starts
   a second instance meanwhile. */
static SemaphoreHandle_t s_ctl;
static int s_users;                 /* references taken by ml_get (atomic) */
static microlink_t *s_zombie;       /* stopped, not yet freed */
static TaskHandle_t s_reaper;

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

static void notify(void) { if (s_cb) s_cb(s_cb_user); }

/* ctl held */
static bool reap_locked(void)
{
    if (!s_zombie) return true;
    if (__atomic_load_n(&s_users, __ATOMIC_SEQ_CST) > 0) return false;
    if (!microlink_destroy_finish(s_zombie)) return false;
    s_zombie = NULL;
    ESP_LOGI(TAG, "the stopped MicroLink instance is freed");
    return true;
}

static void reaper_task(void *arg)
{
    (void)arg;
    bool done = false;
    for (int i = 0; i < 300 && !done; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ctl_lock();
        done = reap_locked();
        ctl_unlock();
    }
    if (!done) ESP_LOGE(TAG, "a stopped MicroLink instance is still in use after 300 s: it stays allocated");
    s_reaper = NULL;
    vTaskDelete(NULL);
}

static void on_state(microlink_t *ml, microlink_state_t st, void *user)
{
    (void)user;
    if (ml != s_ml) return;   /* a stopped instance's last task: the switch is off */
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

/* Auth key: settings first, then /sdcard/tailscale/authkey.txt (first line). */
static bool load_auth_key(char *out, size_t cap)
{
    if (g_settings.ts_auth_key[0]) { strlcpy(out, g_settings.ts_auth_key, cap); return true; }
    char *txt = NULL; size_t len = 0;
    if (fs_read_all(SD_MOUNT "/tailscale/authkey.txt", &txt, &len, 4096)) {
        char *nl = strpbrk(txt, "\r\n"); if (nl) *nl = 0;
        strlcpy(out, txt, cap); free(txt);
        if (strncmp(out, "tskey-", 6) == 0) {
            strlcpy(g_settings.ts_auth_key, out, sizeof(g_settings.ts_auth_key)); settings_save();
            return true;
        }
    }
    return false;
}

static char s_key[160], s_name[40], s_ctrl[96];

bool tailscale_enabled(void) { return g_settings.ts_enable != 0; }

/* ctl held */
static bool start_locked(void)
{
    /* the switch is off: nothing reaches microlink_init or microlink_start */
    if (!g_settings.ts_enable) { strlcpy(s_err, "Tailscale is off", sizeof(s_err)); return false; }
    if (s_started) return true;
    if (!reap_locked()) {
        strlcpy(s_err, "Tailscale is still stopping; try again in a few seconds", sizeof(s_err));
        ESP_LOGW(TAG, "start: the stopped instance isn't freed yet; not starting a second one");
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

static void teardown(void);

void tailscale_stop(void)
{
    teardown();   /* never holds s_ctl across microlink_stop's wait */
    notify();
}

/* Stops the instance and frees it, or leaves it to the reaper while a task or a reference
   remains. Its node keys stay in NVS, so starting again reuses the registered node. */
static void teardown(void)
{
    ctl_lock();
    microlink_t *ml = s_ml;
    s_ml = NULL;   /* from here its callbacks are ignored and ml_live() is false */
    bool was_started = s_started;
    s_started = false; s_state = TS_OFF; s_ip[0] = 0;
    ctl_unlock();
    if (!ml) return;
    if (was_started) microlink_stop(ml);   /* up to 15 s: outside s_ctl */
    ctl_lock();
    if (s_zombie) {
        ESP_LOGE(TAG, "a second stopped instance while the first isn't freed: the first stays allocated");
        s_zombie = NULL;
    }
    s_zombie = ml;
    if (!reap_locked()) {
        ESP_LOGW(TAG, "a MicroLink task or a caller is still on the stopped instance; freeing it when they finish");
        if (!s_reaper) xTaskCreate(reaper_task, "ts_reap", 3072, NULL, 2, &s_reaper);
    }
    ctl_unlock();
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
    teardown();
    s_err[0] = 0;
    notify();
}

void tailscale_on_wifi(bool connected)
{
    if (!g_settings.ts_enable || !s_ctl) return;   /* the switch is off: no start, no rebind */
    ctl_lock();
    if (connected && !s_started) start_locked();
    else if (connected && !s_fast_resume) microlink_rebind(s_ml);
    else if (connected) ESP_LOGI(TAG, "Wi-Fi back: fast resume, no rebind (the check-in handshakes first)");
    ctl_unlock();
}

void tailscale_set_fast_resume(bool on) { s_fast_resume = on; }
bool tailscale_fast_resume(void) { return s_fast_resume; }

const char *tailscale_checkin_name(ts_checkin_t c)
{
    return c == TS_CHECKIN_HANDSHAKE ? "handshake" : c == TS_CHECKIN_REBIND ? "rebind" : "failed";
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
        r = TS_CHECKIN_HANDSHAKE;
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
    if (!tailscale_is_tailnet_ip(ip)) { r = TS_CHECKIN_HANDSHAKE; goto done; }
    uint32_t dest = microlink_parse_ip(ip);
    if (!force_rebind && handshake_wait(ml, dest, CHECKIN_HANDSHAKE_MS)) { r = TS_CHECKIN_HANDSHAKE; goto done; }
    if (!ml_live(ml)) goto done;   /* switched off meanwhile */
    ESP_LOGW(TAG, "check-in: %s, rebinding", force_rebind ? "rebind forced" : "no handshake in 8 s");
    microlink_rebind(ml);
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
            if (!ml_live(ml)) return false;   /* switched off meanwhile */
            microlink_rebind(ml);
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
bool tailscale_forget_node(void)
{
    tailscale_stop();
    return microlink_factory_reset() == ESP_OK;
}
const char *tailscale_last_error(void) { return s_err; }
uint16_t tailscale_home_derp(void)
{
    microlink_t *ml = ml_get();
    uint16_t r = ml ? ml->derp_home_region : 0;
    ml_put(ml);
    return r;
}
