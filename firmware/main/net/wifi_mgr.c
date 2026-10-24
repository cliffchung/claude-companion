#include "wifi_mgr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "cJSON.h"
#include "settings/settings.h"
#include "bsp/m5stack_tab5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "sys/power_rules.h"
#include "sys/boot_phase.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";
#define NS "wifi"

static wifi_saved_t s_saved[WIFI_MAX_SAVED];
static int s_saved_n = 0;
static char s_last_ssid[33];
static bool s_started = false, s_connected = false, s_connecting = false, s_scanning = false;
static char s_cur_ssid[33];
static esp_netif_t *s_netif = NULL;
static wifi_ev_cb_t s_cb = NULL; static void *s_cb_user = NULL;
static wifi_ev_cb_t s_gcb = NULL; static void *s_gcb_user = NULL;
static int s_retry = 0;
static char s_err[64];
static wifi_scan_item_t s_scan[24]; static int s_scan_n = 0;
static bool s_user_disconnect = false;
static bool s_auto_connect;          /* the running connect is the saved network's (boot, unpark), not the user's */

/* Task 8: background reconnect on its own task, the parked radio and modem power save */
static TaskHandle_t s_rc_task;
static volatile bool s_rc_active;    /* a drop the user didn't ask for: retry until an IP */
static volatile bool s_rc_failed;    /* the attempt in flight ended with a disconnect */
static volatile int s_rc_attempt;
static volatile int64_t s_rc_next_ms;
static volatile bool s_parked;
static bool s_park_disconnect;       /* sticky: the C6 refused stop or start */
static bool s_parked_by_disconnect;  /* how the current park was done */
static volatile int s_ps_target = -1;  /* wifi_ps_type_t wanted, -1 = leave the driver's default */
static volatile int s_ps_applied = -1; /* what the driver has (reset when the radio starts again) */
static volatile bool s_ps_hold_none;   /* Task 3: no modem sleep during a check-in boot's few seconds */
static volatile int s_ps_req = -1;     /* the power task's last wish (set_ps_max), applied after a hold */

/* Plan 3 Task 3: when the join started, for the "associated in" log (L30 measured the C6 join) */
static int64_t s_join_at_ms;
/* The optional static address (Network > Wi-Fi > Advanced), NVS "wifi"/"sip"; off by default */
static wifi_static_ip_t s_sip;

static void emit(wifi_ev_t ev) { if (s_gcb) s_gcb(ev, s_gcb_user); if (s_cb) s_cb(ev, s_cb_user); }

static void saved_load(void)
{
    void *blob = NULL; size_t len = 0;
    s_saved_n = 0;
    if (nvs_get_blob_alloc(NS, "list", &blob, &len)) {
        int n = len / sizeof(wifi_saved_t);
        if (n > WIFI_MAX_SAVED) n = WIFI_MAX_SAVED;
        memcpy(s_saved, blob, n * sizeof(wifi_saved_t));
        s_saved_n = n;
        free(blob);
    }
    char *last = NULL;
    if (nvs_get_str_alloc(NS, "last", &last)) { strlcpy(s_last_ssid, last, sizeof(s_last_ssid)); free(last); }
    memset(&s_sip, 0, sizeof(s_sip));
    blob = NULL; len = 0;
    if (nvs_get_blob_alloc(NS, "sip", &blob, &len)) {
        if (len == sizeof(s_sip)) memcpy(&s_sip, blob, len);
        free(blob);
    }
}
static void saved_store(void)
{
    nvs_set_blob_ns(NS, "list", s_saved, s_saved_n * sizeof(wifi_saved_t));
    nvs_set_str_ns(NS, "last", s_last_ssid);
}
static int saved_find(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++) if (!strcmp(s_saved[i].ssid, ssid)) return i;
    return -1;
}
static void saved_put(const char *ssid, const char *pass)
{
    int i = saved_find(ssid);
    if (i < 0) {
        if (s_saved_n >= WIFI_MAX_SAVED) { memmove(&s_saved[0], &s_saved[1], (WIFI_MAX_SAVED - 1) * sizeof(wifi_saved_t)); s_saved_n = WIFI_MAX_SAVED - 1; }
        i = s_saved_n++;
    }
    strlcpy(s_saved[i].ssid, ssid, sizeof(s_saved[i].ssid));
    strlcpy(s_saved[i].pass, pass ? pass : "", sizeof(s_saved[i].pass));
    strlcpy(s_last_ssid, ssid, sizeof(s_last_ssid));
    saved_store();
}

static void rc_kick(void) { if (s_rc_task) xTaskNotifyGive(s_rc_task); }
static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

static void join_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = (pass && *pass) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_join_at_ms = esp_timer_get_time() / 1000;
}

static void apply_static_ip(void);
static void do_connect(const char *ssid, const char *pass)
{
    apply_static_ip();
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    s_connecting = true; s_user_disconnect = false; s_retry = 0;
    join_config(ssid, pass);
    boot_phase_mark(BP_JOIN);
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(e));
}

static void apply_static_ip(void)
{
    if (!s_netif) return;
    if (!s_sip.on) {
        esp_netif_dhcp_status_t st;
        if (esp_netif_dhcpc_get_status(s_netif, &st) == ESP_OK && st == ESP_NETIF_DHCP_STOPPED) esp_netif_dhcpc_start(s_netif);
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = s_sip.ip;
    ip.netmask.addr = s_sip.mask;
    ip.gw.addr = s_sip.gw;
    esp_err_t e = esp_netif_set_ip_info(s_netif, &ip);
    if (s_sip.dns) {
        esp_netif_dns_info_t d = { 0 };
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4.addr = s_sip.dns;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d);
    }
    ESP_LOGI(TAG, "static address " IPSTR " (%s)", IP2STR(&ip.ip), esp_err_to_name(e));
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            boot_phase_mark(BP_WIFI_START);
            s_started = true;
            s_ps_applied = -1;   /* a restarted radio gets the wanted power save again */
            rc_kick();
            if (s_parked) break;   /* a park in progress: never auto-connect */
            if (s_last_ssid[0]) {
                int i = saved_find(s_last_ssid);
                if (i >= 0) { do_connect(s_saved[i].ssid, s_saved[i].pass); s_auto_connect = true; }
            }
            break;
        case WIFI_EVENT_STA_STOP:
            s_started = false;
            s_connecting = false;
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            boot_phase_mark(BP_ASSOC);
            wifi_event_sta_connected_t *c = data;
            if (c) ESP_LOGI(TAG, "associated in %lld ms (ch %u, auth %d)", (long long)(esp_timer_get_time() / 1000 - s_join_at_ms),
                            c->channel, (int)c->authmode);
            esp_netif_create_ip6_linklocal(s_netif);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            bool was = s_connected;
            s_connected = false;
            if (s_parked) {
                /* our own stop or disconnect: never reconnect while parked */
                s_connecting = false;
                s_rc_active = false;
                if (was) emit(WIFI_EV_DISCONNECTED);
                break;
            }
            if (s_user_disconnect) { s_connecting = false; s_rc_active = false; emit(WIFI_EV_DISCONNECTED); break; }
            if (s_rc_active) {
                /* a background attempt failed: the wifi_rc task waits its backoff and tries again */
                s_connecting = false;
                s_rc_failed = true;
                if (was) emit(WIFI_EV_DISCONNECTED);
                rc_kick();
                break;
            }
            if (s_connecting && s_retry < 3) { s_retry++; esp_wifi_connect(); break; }
            if (s_connecting) {
                s_connecting = false;
                snprintf(s_err, sizeof(s_err), "reason %d", d ? d->reason : -1);
                emit(WIFI_EV_CONNECT_FAILED);
                /* the saved network at boot or after an unpark: keep trying in the background (a
                   user's join with a wrong password is left to the Wi-Fi screen) */
                if (s_auto_connect) {
                    s_rc_active = true; s_rc_attempt = 0; s_rc_failed = false;
                    s_rc_next_ms = ms_now() + (int64_t)pr_backoff_s(0) * 1000;
                    rc_kick();
                }
            } else if (was) {
                emit(WIFI_EV_DISCONNECTED);
                /* background reconnect on the wifi_rc task: never a delay in the event handler */
                s_rc_active = true; s_rc_attempt = 0; s_rc_failed = false;
                s_rc_next_ms = ms_now() + (int64_t)pr_backoff_s(0) * 1000;
                ESP_LOGW(TAG, "dropped (reason %d): reconnecting in %d s", d ? d->reason : -1, pr_backoff_s(0));
                rc_kick();
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = sizeof(s_scan) / sizeof(s_scan[0]);
            wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
            s_scan_n = 0;
            if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                for (int i = 0; i < n; i++) {
                    if (!recs[i].ssid[0]) continue;
                    bool dup = false;
                    for (int j = 0; j < s_scan_n; j++) if (!strcmp(s_scan[j].ssid, (char *)recs[i].ssid)) { dup = true; break; }
                    if (dup) continue;
                    strlcpy(s_scan[s_scan_n].ssid, (char *)recs[i].ssid, 33);
                    s_scan[s_scan_n].rssi = recs[i].rssi;
                    s_scan[s_scan_n].auth = recs[i].authmode;
                    s_scan[s_scan_n].channel = recs[i].primary;
                    s_scan_n++;
                }
            }
            free(recs);
            esp_wifi_clear_ap_list();
            s_scanning = false;
            emit(WIFI_EV_SCAN_DONE);
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            boot_phase_mark(BP_IP);
            s_connected = true; s_connecting = false; s_retry = 0;
            if (s_rc_active) ESP_LOGI(TAG, "reconnected after %d background attempt(s)", s_rc_attempt);
            s_rc_active = false; s_rc_attempt = 0; s_auto_connect = false;
            emit(WIFI_EV_CONNECTED);
        } else if (id == IP_EVENT_GOT_IP6) {
            emit(WIFI_EV_GOT_IP6);
        }
    }
}

/* ---- the wifi_rc task -------------------------------------------------------- */

#define RC_ATTEMPT_MS 15000   /* an attempt with no IP and no failure in this long counts as failed */

static void rc_apply_ps(void)
{
    int want = s_ps_target;
    if (want < 0 || want == s_ps_applied || !s_started) return;
    esp_err_t e = esp_wifi_set_ps((wifi_ps_type_t)want);
    ESP_LOGI(TAG, "power save: %s (%s)", want == WIFI_PS_MAX_MODEM ? "max modem" : want == WIFI_PS_NONE ? "none" : "min modem",
             esp_err_to_name(e));
    s_ps_applied = want;   /* a refusal isn't retried every wake-up; the next change or start tries again */
}

static void rc_task(void *arg)
{
    (void)arg;
    int64_t attempt_at = 0;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_rc_active) {
            int64_t left = (attempt_at ? attempt_at + RC_ATTEMPT_MS : s_rc_next_ms) - ms_now();
            wait = pdMS_TO_TICKS(left > 0 ? left : 1);
        }
        ulTaskNotifyTake(pdTRUE, wait);
        rc_apply_ps();
        if (!s_rc_active || s_connected || s_parked || s_user_disconnect) {
            s_rc_active = false;
            attempt_at = 0;
            continue;
        }
        int64_t now = ms_now();
        if (attempt_at) {
            /* an attempt in flight: wait for its IP (rc inactive) or its failure */
            if (!s_rc_failed && now < attempt_at + RC_ATTEMPT_MS) continue;
            attempt_at = 0;
            s_connecting = false;   /* a join stuck with no event counts as failed too */
            int wait_s = pr_backoff_s(s_rc_attempt);
            s_rc_next_ms = now + (int64_t)wait_s * 1000;
            ESP_LOGW(TAG, "reconnect attempt %d failed: next in %d s", s_rc_attempt, wait_s);
            continue;
        }
        if (now < s_rc_next_ms) continue;
        s_rc_failed = false;
        s_rc_attempt++;
        attempt_at = now;
        if (s_connecting) {   /* an unpark's own join is still running: count it as this attempt */
            ESP_LOGI(TAG, "reconnect attempt %d: the join in progress", s_rc_attempt);
            continue;
        }
        esp_err_t e;
        if (!s_started) {
            e = esp_wifi_start();   /* an unpark's start failed: STA_START joins the saved network */
            if (e != ESP_OK) s_park_disconnect = true;
        } else {
            if (s_last_ssid[0]) {
                int i = saved_find(s_last_ssid);
                if (i >= 0) strlcpy(s_cur_ssid, s_saved[i].ssid, sizeof(s_cur_ssid));
            }
            s_connecting = true;
            e = esp_wifi_connect();
        }
        ESP_LOGI(TAG, "reconnect attempt %d: %s", s_rc_attempt, esp_err_to_name(e));
        if (e != ESP_OK) { s_rc_failed = true; s_connecting = false; }
        xTaskNotifyGive(xTaskGetCurrentTaskHandle());   /* handle an immediate failure at once */
    }
}

bool wifi_mgr_park(void)
{
    s_parked = true;
    s_rc_active = false;
    s_auto_connect = false;
    rc_kick();
    esp_err_t e = ESP_FAIL;
    if (!s_park_disconnect) {
        e = esp_wifi_stop();
        if (e != ESP_OK) {
            s_park_disconnect = true;
            ESP_LOGW(TAG, "park mode: disconnect (esp_wifi_stop: %s)", esp_err_to_name(e));
        }
    }
    if (e == ESP_OK) {
        s_parked_by_disconnect = false;
        /* M4: the stop's own STA_DISCONNECTED would find s_connected already false, so emit here */
        bool was = s_connected;
        s_connected = false;
        if (was) emit(WIFI_EV_DISCONNECTED);
        ESP_LOGI(TAG, "parked (stop)");
        return true;
    }
    s_parked_by_disconnect = true;
    esp_err_t d = esp_wifi_disconnect();
    esp_err_t p = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (p == ESP_OK) s_ps_applied = WIFI_PS_MAX_MODEM;
    ESP_LOGI(TAG, "parked (disconnect): disconnect %s, power save %s", esp_err_to_name(d), esp_err_to_name(p));
    return d == ESP_OK || d == ESP_ERR_WIFI_NOT_CONNECT;
}

void wifi_mgr_unpark(void)
{
    if (!s_parked) return;
    s_parked = false;
    s_user_disconnect = false;
    /* the background reconnect is the safety net: a join that fails (or never starts) is retried
       with the backoff, and a join that works ends it */
    s_rc_attempt = 0; s_rc_failed = false;
    s_rc_next_ms = ms_now() + RC_ATTEMPT_MS;
    s_rc_active = true;
    if (!s_parked_by_disconnect) {
        esp_err_t e = esp_wifi_start();   /* STA_START joins the saved network */
        if (e != ESP_OK) {
            s_park_disconnect = true;
            s_rc_next_ms = ms_now() + (int64_t)pr_backoff_s(0) * 1000;
            ESP_LOGW(TAG, "park mode: disconnect (esp_wifi_start: %s); retrying in the background", esp_err_to_name(e));
        } else {
            ESP_LOGI(TAG, "unparked (start)");
        }
        rc_kick();
        return;
    }
    int i = s_last_ssid[0] ? saved_find(s_last_ssid) : -1;
    ESP_LOGI(TAG, "unparked (disconnect)");
    if (i >= 0 && !s_connected) { do_connect(s_saved[i].ssid, s_saved[i].pass); s_auto_connect = true; }
    rc_kick();
}

bool wifi_mgr_parked(void) { return s_parked; }
const char *wifi_mgr_park_mode(void)
{
    if (s_parked) return s_parked_by_disconnect ? "disconnect" : "stop";
    return s_park_disconnect ? "disconnect" : "stop";
}

void wifi_mgr_set_ps_max(bool max)
{
    s_ps_req = max ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    if (s_ps_hold_none) return;   /* a check-in holds the radio awake until it goes down or is released */
    s_ps_target = s_ps_req;
    rc_kick();
}

void wifi_mgr_ps_hold_none(bool on)
{
    if (s_ps_hold_none == on) return;
    s_ps_hold_none = on;
    s_ps_target = on ? WIFI_PS_NONE : s_ps_req;   /* released: whatever the power task asked for meanwhile */
    rc_kick();
}

void wifi_mgr_test_drop(void)
{
    ESP_LOGW(TAG, "test drop: disconnecting as a real drop would");
    esp_wifi_disconnect();
}

void wifi_mgr_reconnect_text(char *buf, size_t cap)
{
    char hw[24] = "";
    if (s_rc_task) snprintf(hw, sizeof(hw), " rc-stack-free=%u", (unsigned)uxTaskGetStackHighWaterMark(s_rc_task));   /* M5 */
    if (s_parked) { snprintf(buf, cap, "wifi parked (%s)%s", wifi_mgr_park_mode(), hw); return; }
    if (!s_rc_active) { snprintf(buf, cap, "reconnect: idle (park mode %s)%s", wifi_mgr_park_mode(), hw); return; }
    int64_t left = s_rc_next_ms - ms_now();
    snprintf(buf, cap, "reconnect: attempt %d, next in %lld s%s", s_rc_attempt, (long long)(left > 0 ? (left + 999) / 1000 : 0), hw);
}

void wifi_mgr_init(void)
{
    saved_load();
    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "tab5");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e)); return; }
    boot_phase_mark(BP_HOSTED);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_wifi, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (!s_rc_task && xTaskCreate(rc_task, "wifi_rc", 3072, NULL, 3, &s_rc_task) != pdPASS)
        ESP_LOGE(TAG, "no memory for the reconnect task");
    e = esp_wifi_start();
    if (e != ESP_OK) ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
}

bool wifi_mgr_is_connected(void) { return s_connected; }
bool wifi_mgr_is_connecting(void) { return s_connecting; }
bool wifi_mgr_is_started(void) { return s_started; }
const char *wifi_mgr_ssid(void) { return s_connected ? s_cur_ssid : ""; }

void wifi_mgr_get_ip4(char *buf, size_t len)
{
    buf[0] = 0;
    if (!s_connected || !s_netif) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

void wifi_mgr_get_ip6(char *buf, size_t len)
{
    buf[0] = 0;
    if (!s_connected || !s_netif) return;
    esp_ip6_addr_t addrs[5];
    int n = esp_netif_get_all_ip6(s_netif, addrs);
    /* prefer global */
    for (int pass = 0; pass < 2 && !buf[0]; pass++) {
        for (int i = 0; i < n; i++) {
            esp_ip6_addr_type_t t = esp_netif_ip6_get_addr_type(&addrs[i]);
            bool global = (t == ESP_IP6_ADDR_IS_GLOBAL || t == ESP_IP6_ADDR_IS_UNIQUE_LOCAL);
            if ((pass == 0 && global) || pass == 1) { snprintf(buf, len, IPV6STR, IPV62STR(addrs[i])); break; }
        }
    }
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
int wifi_mgr_channel(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.primary;
    return 0;
}

void wifi_mgr_connect(const char *ssid, const char *pass, bool save)
{
    if (!s_started) return;
    if (save) saved_put(ssid, pass);
    s_rc_active = false;
    if (s_connected || s_connecting) { s_user_disconnect = true; esp_wifi_disconnect(); vTaskDelay(pdMS_TO_TICKS(200)); }
    do_connect(ssid, pass);
    s_auto_connect = false;
}

void wifi_mgr_disconnect(void)
{
    s_user_disconnect = true;
    s_rc_active = false;
    esp_wifi_disconnect();
}

bool wifi_mgr_scan_start(void)
{
    if (!s_started || s_scanning) return false;
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    sc.scan_time.active.min = 60; sc.scan_time.active.max = 150;
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    s_scanning = (e == ESP_OK);
    if (e != ESP_OK) ESP_LOGW(TAG, "scan: %s", esp_err_to_name(e));
    return s_scanning;
}

int wifi_mgr_scan_results(wifi_scan_item_t *out, int max)
{
    int n = s_scan_n < max ? s_scan_n : max;
    memcpy(out, s_scan, n * sizeof(*out));
    return n;
}

int wifi_mgr_saved_count(void) { return s_saved_n; }
const wifi_saved_t *wifi_mgr_saved(int i) { return (i >= 0 && i < s_saved_n) ? &s_saved[i] : NULL; }
void wifi_mgr_forget(int i)
{
    if (i < 0 || i >= s_saved_n) return;
    if (!strcmp(s_saved[i].ssid, s_last_ssid)) s_last_ssid[0] = 0;
    memmove(&s_saved[i], &s_saved[i + 1], (s_saved_n - i - 1) * sizeof(wifi_saved_t));
    s_saved_n--;
    saved_store();
}
void wifi_mgr_set_cb(wifi_ev_cb_t cb, void *user) { s_cb = cb; s_cb_user = user; }
void wifi_mgr_set_global_cb(wifi_ev_cb_t cb, void *user) { s_gcb = cb; s_gcb_user = user; }
const char *wifi_mgr_last_error(void) { return s_err; }

char *wifi_mgr_export_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < s_saved_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", s_saved[i].ssid);
        cJSON_AddStringToObject(o, "pass", s_saved[i].pass);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddStringToObject(root, "last", s_last_ssid);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

bool wifi_mgr_import_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *arr = cJSON_GetObjectItem(root, "networks");
    if (cJSON_IsArray(arr)) {
        s_saved_n = 0;
        cJSON *o;
        cJSON_ArrayForEach(o, arr) {
            const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(o, "ssid"));
            const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(o, "pass"));
            if (ssid && s_saved_n < WIFI_MAX_SAVED) {
                strlcpy(s_saved[s_saved_n].ssid, ssid, 33);
                strlcpy(s_saved[s_saved_n].pass, pass ? pass : "", 65);
                s_saved_n++;
            }
        }
    }
    const char *last = cJSON_GetStringValue(cJSON_GetObjectItem(root, "last"));
    if (last) strlcpy(s_last_ssid, last, sizeof(s_last_ssid));
    saved_store();
    cJSON_Delete(root);
    return true;
}

/* ---- Plan 3 Task 3: the static address ---- */


bool wifi_mgr_static_ip_get(wifi_static_ip_t *out) { if (out) *out = s_sip; return s_sip.on; }
bool wifi_mgr_static_ip_set(const wifi_static_ip_t *in)
{
    wifi_static_ip_t n = { 0 };
    if (in && in->on) {
        if (!in->ip || !in->mask || !in->gw) return false;
        n = *in;
        n.on = 1;
    }
    s_sip = n;
    nvs_set_blob_ns(NS, "sip", &s_sip, sizeof(s_sip));
    ESP_LOGI(TAG, "static address: %s (from the next join)", s_sip.on ? "on" : "off");
    return true;
}
