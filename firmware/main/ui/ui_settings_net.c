/* Settings › Network (spec §5.8) and its three sub-pages: Networks, Advanced and Peers.
 *
 * The Network panel is one column of 64 px rows: Wi-Fi (the current network, Networks, Advanced), then
 * Tailscale in U10's order, every row shown with the switch off. The panel reads its values when it is
 * built; a row that changes one rebuilds it in place (ui_settings_refill). Only the current-network row
 * follows Wi-Fi events while the panel shows.
 *
 * No timers. Scan results and peer changes arrive by event (wifi_mgr_set_cb, tailscale_set_event_cb), are
 * moved to the LVGL task with ui_async, and both slots are cleared when the object that set them is
 * deleted. The Tailscale slot is single: the Network panel holds it while its Status row exists, Peers while it
 * shows, Terminal while it shows (one screen alive at a time, B2).
 *
 * Secrets: the auth key and Wi-Fi passwords are typed masked, never shown, never logged. */
#include "ui_settings.h"
#include "ui_host_form.h"
#include "companion/cc_link.h"
#include "ui.h"
#include "ui_comp.h"
#include "settings/settings.h"
#include "net/wifi_mgr.h"
#include "net/fast_rules.h"
#include "net/tailscale.h"
#include "storage/sd.h"
#include "sys/vitals.h"
#include "sys/sysinfo.h"
#ifndef UI_PREVIEW
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FILE SD_MOUNT "/tailscale/authkey.txt"
#define SCAN_MAX 20
#define PEERS_MAX 64

/* ---------------------------------------------------------------- helpers */
static const char *signal_word(int rssi)
{
    switch (vitals_wifi_bars(true, rssi)) {
    case 4: return tr(STR_NET_SIG_STRONG);
    case 3: return tr(STR_NET_SIG_GOOD);
    case 2: return tr(STR_NET_SIG_FAIR);
    default: return tr(STR_NET_SIG_WEAK);
    }
}

/* Trims leading and trailing spaces in place. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static int saved_index(const char *ssid)
{
    for (int i = 0; ssid && ssid[0] && i < wifi_mgr_saved_count(); i++) {
        const wifi_saved_t *w = wifi_mgr_saved(i);
        if (w && !strcmp(w->ssid, ssid)) return i;
    }
    return -1;
}

static void ip4_text(uint32_t a, char *out, size_t cap)
{
    uint8_t b[4];
    memcpy(b, &a, 4);
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int prefix_len(uint32_t mask)
{
    uint8_t m[4];
    memcpy(m, &mask, 4);
    int n = 0;
    for (int i = 0; i < 4; i++) for (int b = 7; b >= 0; b--) n += (m[i] >> b) & 1;
    return n;
}

/* The row value: "Off (DHCP)" or "192.168.1.50/24 · gw 192.168.1.1". */
static void static_value(char *out, size_t cap)
{
    wifi_static_ip_t s;
    if (!wifi_mgr_static_ip_get(&s) || !s.on) { strlcpy(out, tr(STR_NET_STATIC_OFF), cap); return; }
    char ip[16], gw[16];
    ip4_text(s.ip, ip, sizeof(ip));
    ip4_text(s.gw, gw, sizeof(gw));
    snprintf(out, cap, tr(STR_NET_STATIC_VALUE), ip, prefix_len(s.mask), gw);
}

/* The prompt's starting text, in the format it takes: "address/prefix gateway dns", or "" when off. */
static void static_initial(char *out, size_t cap)
{
    wifi_static_ip_t s;
    out[0] = 0;
    if (!wifi_mgr_static_ip_get(&s) || !s.on) return;
    char ip[16], gw[16], dns[16];
    ip4_text(s.ip, ip, sizeof(ip));
    ip4_text(s.gw, gw, sizeof(gw));
    ip4_text(s.dns, dns, sizeof(dns));
    snprintf(out, cap, "%s/%d %s %s", ip, prefix_len(s.mask), gw, dns);
}

static int peers_online(void)
{
    int n = tailscale_peer_count(), on = 0;
    for (int i = 0; i < n; i++) {
        ts_peer_t p;
        if (tailscale_peer(i, &p) && p.online) on++;
    }
    return on;
}

/* ------------------------------------------------- the current network row */
static lv_obj_t *s_wifi_row;   /* while the Network panel shows */

static void wifi_row_set(void)
{
    if (!s_wifi_row) return;
    bool up = wifi_mgr_is_connected();
    lv_label_set_text(lv_obj_get_child(s_wifi_row, 0), up ? wifi_mgr_ssid() : tr(STR_NET_WIFI));
    lv_obj_t *v = ui_srow_value(s_wifi_row);
    char b[64];
    if (up) snprintf(b, sizeof(b), tr(STR_NET_CONNECTED), signal_word(sysinfo_rssi_cached()));
    else strlcpy(b, tr(STR_NET_NOT_CONNECTED), sizeof(b));
    if (strcmp(lv_label_get_text(v), b)) lv_label_set_text(v, b);
    ui_text_tone(v, up ? UI_TONE_GREEN : UI_TONE_MUTED);
}

static void panel_wifi_async(void *arg) { (void)arg; wifi_row_set(); }
static void panel_wifi_ev(wifi_ev_t ev, void *user)
{
    (void)user;
    if (ev == WIFI_EV_CONNECTED || ev == WIFI_EV_DISCONNECTED || ev == WIFI_EV_CONNECT_FAILED)
        ui_async(panel_wifi_async, NULL);
}
static void wifi_row_deleted(lv_event_t *e)
{
    (void)e;
    wifi_mgr_set_cb(NULL, NULL);
    s_wifi_row = NULL;
}

static char s_cur_ssid[33];   /* the network the sheet and its confirms act on */

static void disconnect_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    wifi_mgr_disconnect();   /* RAM only: until the next boot or the next join */
    wifi_row_set();
}
static void forget_cur_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    int i = saved_index(s_cur_ssid);
    if (i >= 0) wifi_mgr_forget(i);
    ui_toastf(tr(STR_NET_FORGOTTEN), s_cur_ssid);
    ui_settings_refill();
}
static void wifi_sheet_cb(int i, void *ud)
{
    (void)ud;
    char q[96];
    if (i == 0) {
        ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_NETWORKS);
    } else if (i == 1) {
        snprintf(q, sizeof(q), tr(STR_NET_DISCONNECT_Q), s_cur_ssid);
        ui_confirm_ex(q, tr(STR_NET_DISCONNECT_BODY), tr(STR_NET_DISCONNECT), false, disconnect_yes, NULL);
    } else if (i == 2) {
        snprintf(q, sizeof(q), tr(STR_NET_FORGET_Q), s_cur_ssid);
        ui_confirm_ex(q, tr(STR_NET_FORGET_BODY), tr(STR_NET_FORGET), true, forget_cur_yes, NULL);
    }
}
static void wifi_row_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_mgr_is_connected()) {   /* nothing to disconnect: pick a network instead */
        ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_NETWORKS);
        return;
    }
    strlcpy(s_cur_ssid, wifi_mgr_ssid(), sizeof(s_cur_ssid));
    /* the harmless choice first: a bare Enter on the fresh sheet only opens the list (review M6); Disconnect and
       Forget each ask first */
    const char *items[3] = { tr(STR_NET_OTHER_NETWORKS), tr(STR_NET_DISCONNECT), tr(STR_NET_FORGET) };
    ui_action_sheet(s_cur_ssid, items, saved_index(s_cur_ssid) >= 0 ? 3 : 2, 1u << 2, wifi_sheet_cb, NULL);
}

static void networks_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_NETWORKS); }
static void advanced_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_ADVANCED); }
static void peers_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_PEERS); }

/* ------------------------------------------------------------ Tailscale */
static void ts_switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    tailscale_set_enabled(on);   /* saved at once: sticky across boots and wakes */
    ui_toast(tr(on ? (tailscale_stopping() ? STR_NET_TS_TOAST_AFTER_STOP
                      : wifi_mgr_is_connected() ? STR_NET_TS_TOAST_CONNECTING : STR_NET_TS_TOAST_WITH_WIFI)
                   : (tailscale_stopping() ? STR_NET_TS_TOAST_TURNING_OFF : STR_NET_TS_TOAST_OFF)));
    ui_settings_refill();
}

static str_id_t ts_state_word(void)
{
    if (tailscale_stopping()) return STR_NET_TS_ST_STOPPING;
    switch (tailscale_state()) {
    case TS_WAIT_WIFI: return STR_NET_TS_ST_WAIT_WIFI;
    case TS_CONNECTING: return STR_NET_TS_ST_CONNECTING;
    case TS_REGISTERING: return STR_NET_TS_ST_REGISTERING;
    case TS_CONNECTED: return STR_NET_TS_ST_CONNECTED;
    case TS_RECONNECTING: return STR_NET_TS_ST_RECONNECTING;
    case TS_ERROR: return STR_NET_TS_ST_ERROR;
    default: return STR_NET_TS_ST_OFF;
    }
}

/* "connected · 100.x.y.z · DERP sea · 3 peers online"; the last error while TS_ERROR; the off sentence. */
static void ts_status_text(char *b, size_t cap, bool *red)
{
    *red = false;
    if (!tailscale_enabled()) { strlcpy(b, tr(STR_NET_TS_OFF_TEXT), cap); return; }
    if (tailscale_state() == TS_ERROR && !tailscale_stopping()) {
        const char *err = tailscale_last_error();
        strlcpy(b, err && *err ? err : tr(STR_NET_TS_ST_ERROR), cap);
        *red = true;
        return;
    }
    strlcpy(b, tr(ts_state_word()), cap);
    if (tailscale_state() != TS_CONNECTED || tailscale_stopping()) return;
    char part[48], derp[16];
    const char *ip = tailscale_vpn_ip();
    if (ip && *ip) { strlcat(b, " \xc2\xb7 ", cap); strlcat(b, ip, cap); }
    if (tailscale_home_derp_code(derp, sizeof(derp))) {
        snprintf(part, sizeof(part), tr(STR_NET_TS_DERP), derp);
        strlcat(b, " \xc2\xb7 ", cap);
        strlcat(b, part, cap);
    }
    int on = peers_online();
    if (on == 1) strlcpy(part, tr(STR_NET_TS_PEER_ONLINE), sizeof(part));
    else snprintf(part, sizeof(part), tr(STR_NET_TS_PEERS_ONLINE), on);
    strlcat(b, " \xc2\xb7 ", cap);
    strlcat(b, part, cap);
}

/* Auth key: never shown, never logged, never put in a field as a starting text. */
static void key_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    char *k = strdup(v);
    if (!k) return;
    char *t = trim(k);
    if (!*t) ui_toast(tr(STR_NET_TS_KEY_EMPTY));
    else {
        strlcpy(g_settings.ts_auth_key, t, sizeof(g_settings.ts_auth_key));
        settings_save();
        ui_toast(tr(STR_NET_TS_KEY_SAVED));
    }
    memset(k, 0, strlen(k));
    free(k);
    ui_settings_refill();
}

/* Load from SD card: the first line of /sdcard/tailscale/authkey.txt, mounting the card on demand (spec §7.1).
   The mount and the read run on a one-shot worker task, as the forget does, so the LVGL task never waits on the
   card; the result comes back through ui_async. A line that isn't a key, or doesn't fit the setting, is refused
   whole: never truncated. The key is never logged. */
typedef enum { SDK_OK, SDK_NO_CARD, SDK_NO_FILE, SDK_NOT_A_KEY, SDK_TOO_LONG } sdk_result_t;
typedef struct { sdk_result_t res; char key[sizeof(g_settings.ts_auth_key)]; } sdk_job_t;
static volatile bool s_sdk_running;   /* set on the LVGL task; cleared there, or by the worker when its post fails */

static void key_sd_done_async(void *arg)
{
    sdk_job_t *j = arg;
    s_sdk_running = false;
    static const str_id_t msg[] = { STR_NET_TS_KEY_LOADED, STR_NET_TS_NO_SD, STR_NET_TS_NO_KEY_FILE,
                                    STR_NET_TS_KEY_NOT_A_KEY, STR_NET_TS_KEY_TOO_LONG };
    if (j->res == SDK_OK) {
        strlcpy(g_settings.ts_auth_key, j->key, sizeof(g_settings.ts_auth_key));
        settings_save();
    }
    ui_toast(tr(msg[j->res]));
    memset(j, 0, sizeof(*j));
    free(j);
    ui_settings_refill();
}
static void key_sd_work(sdk_job_t *j)
{
    char *txt = NULL;
    size_t len = 0;
    bool held = sd_mount();
    bool read = held && fs_read_all(KEY_FILE, &txt, &len, 4096);
    if (held) sd_unmount();   /* M1: the key is in RAM; the card goes down again */
    if (!held) j->res = SDK_NO_CARD;
    else if (!read) j->res = SDK_NO_FILE;
    else {
        char *nl = strpbrk(txt, "\r\n");
        if (nl) *nl = 0;
        char *t = trim(txt);
        if (!*t) j->res = SDK_NO_FILE;
        else if (strncmp(t, "tskey-", 6) != 0) j->res = SDK_NOT_A_KEY;
        else if (strlen(t) >= sizeof(j->key)) j->res = SDK_TOO_LONG;
        else { strlcpy(j->key, t, sizeof(j->key)); j->res = SDK_OK; }
        memset(txt, 0, len);
        free(txt);
    }
    if (!ui_async(key_sd_done_async, j)) {   /* never posted: nobody else frees the job or clears the flag */
        memset(j, 0, sizeof(*j));
        free(j);
        s_sdk_running = false;
    }
}
#ifndef UI_PREVIEW
static void key_sd_task(void *arg)
{
    key_sd_work(arg);
    vTaskDelete(NULL);
}
#endif
static void key_from_sd(void)
{
    if (s_sdk_running) return;
    sdk_job_t *j = calloc(1, sizeof(*j));
    if (!j) { ui_toast(tr(STR_NET_TS_NO_SD)); return; }
    s_sdk_running = true;
#ifdef UI_PREVIEW
    key_sd_work(j);   /* the preview has no tasks */
#else
    if (xTaskCreate(key_sd_task, "ts_keysd", 4096, j, 3, NULL) != pdPASS) {
        s_sdk_running = false;
        free(j);
        ui_toast(tr(STR_NET_TS_NO_SD));
    }
#endif
}

static void key_clear_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    memset(g_settings.ts_auth_key, 0, sizeof(g_settings.ts_auth_key));
    settings_save();
    ui_toast(tr(STR_NET_TS_KEY_CLEARED));
    ui_settings_refill();
}

static void key_sheet_cb(int i, void *ud)
{
    (void)ud;
    if (i == 0) ui_prompt(tr(STR_NET_TS_AUTH_KEY), tr(STR_NET_TS_KEY_PLACEHOLDER), "", true, key_prompt_cb, NULL);
    else if (i == 1) key_from_sd();
    else if (i == 2)
        ui_confirm_ex(tr(STR_NET_TS_KEY_CLEAR_Q), tr(STR_NET_TS_KEY_CLEAR_BODY), tr(STR_NET_TS_KEY_CLEAR_ACTION), true,
                      key_clear_yes, NULL);
}
static void key_cb(lv_event_t *e)
{
    (void)e;
    const char *items[3] = { tr(STR_NET_TS_KEY_ENTER), tr(STR_NET_TS_KEY_SD), tr(STR_NET_TS_KEY_CLEAR) };
    ui_action_sheet(tr(STR_NET_TS_AUTH_KEY), items, g_settings.ts_auth_key[0] ? 3 : 2, 1u << 2, key_sheet_cb, NULL);
}

static const char *device_name(void) { return g_settings.ts_device_name[0] ? g_settings.ts_device_name : tr(STR_NET_TS_NAME_DEFAULT); }
static void name_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    char b[sizeof(g_settings.ts_device_name)];
    strlcpy(b, v, sizeof(b));
    char *t = trim(b);
    if (!*t) { ui_toast(tr(STR_NET_TS_NAME_EMPTY)); return; }   /* refused: nothing saved */
    strlcpy(g_settings.ts_device_name, t, sizeof(g_settings.ts_device_name));
    settings_save();
    ui_toast(tr(STR_NET_TS_APPLIES));
    ui_settings_refill();
}
static void name_cb(lv_event_t *e)
{
    (void)e;
    ui_prompt(tr(STR_NET_TS_NAME), tr(STR_NET_TS_NAME_DEFAULT), device_name(), false, name_prompt_cb, NULL);
}

static void ctrl_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    char b[sizeof(g_settings.ts_ctrl_host)];
    strlcpy(b, v, sizeof(b));
    strlcpy(g_settings.ts_ctrl_host, trim(b), sizeof(g_settings.ts_ctrl_host));   /* empty: the default */
    settings_save();
    ui_toast(tr(STR_NET_TS_APPLIES));
    ui_settings_refill();
}
static void ctrl_cb(lv_event_t *e)
{
    (void)e;
    ui_prompt(tr(STR_NET_TS_CTRL), tr(STR_NET_TS_CTRL_PLACEHOLDER), g_settings.ts_ctrl_host, false, ctrl_prompt_cb, NULL);
}

static const char *s_conn_items[2];
static void conn_pick_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i > 1 || i == (g_settings.ts_direct ? 1 : 0)) return;
    g_settings.ts_direct = (uint8_t)i;
    settings_save();
    ui_toast(tr(STR_NET_TS_APPLIES));
    ui_settings_refill();
}
static void conn_cb(lv_event_t *e)
{
    (void)e;
    s_conn_items[0] = tr(STR_NET_TS_RELAY);
    s_conn_items[1] = tr(STR_NET_TS_DIRECT);
    ui_picker(tr(STR_NET_TS_CONN), s_conn_items, 2, g_settings.ts_direct ? 1 : 0, conn_pick_cb, NULL);
}

/* Forget this device: tailscale_forget_node waits for the old instance to be freed (up to 40 s), so it runs
   on a worker task. The auth key is kept (U5): the node registers again with it on the next start. */
static volatile bool s_forget_running;   /* a second tap doesn't start a second forget; cleared by the done async,
                                           or by the worker when its post fails */
static void forget_done_async(void *arg)
{
    s_forget_running = false;
    ui_toast(tr(arg ? STR_NET_TS_FORGOTTEN : STR_NET_TS_FORGET_FAILED));
    ui_settings_refill();
}
static void forget_work(void)
{
    bool ok = tailscale_forget_node();
    if (!ui_async(forget_done_async, ok ? (void *)1 : NULL)) s_forget_running = false;   /* no job to free */
}
#ifndef UI_PREVIEW
static void forget_task(void *arg)
{
    (void)arg;
    forget_work();
    vTaskDelete(NULL);
}
#endif
static void forget_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    if (s_forget_running || tailscale_forgetting()) { ui_toast(tr(STR_NET_TS_FORGET_BUSY)); return; }
    s_forget_running = true;
    ui_toast(tr(STR_NET_TS_FORGETTING));
#ifdef UI_PREVIEW
    forget_work();   /* the preview has no tasks: its stub returns at once */
#else
    if (xTaskCreate(forget_task, "ts_forget", 4096, NULL, 3, NULL) != pdPASS) {
        s_forget_running = false;
        ui_toast(tr(STR_NET_TS_FORGET_FAILED));
    }
#endif
}
static void forget_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm_ex(tr(STR_NET_TS_FORGET_Q),
                  tr(g_settings.ts_auth_key[0] ? STR_NET_TS_FORGET_BODY : STR_NET_TS_FORGET_BODY_NO_KEY),
                  tr(STR_NET_TS_FORGET_ACTION), true, forget_yes, NULL);
}

/* The Tailscale rows that follow events while the panel shows: the switch and its state, Status, Peers. */
static lv_obj_t *s_ts_switch, *s_ts_state, *s_ts_status, *s_ts_peers;
static volatile bool s_ts_queued;   /* one ui_async in flight at most, however many events arrive */

static void label_set(lv_obj_t *l, const char *t) { if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }

/* In place: no rebuild, so the focus and the scroll stay. */
static void ts_rows_set(void)
{
    if (!s_ts_status) return;
    bool on = tailscale_enabled(), red;
    if (on != lv_obj_has_state(s_ts_switch, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(s_ts_switch, LV_STATE_CHECKED);   /* no VALUE_CHANGED: nothing is toggled */
        else lv_obj_remove_state(s_ts_switch, LV_STATE_CHECKED);
    }
    label_set(s_ts_state, tr(on ? STR_NET_TS_ON : STR_NET_TS_OFF_SAVED));
    char v[200];
    ts_status_text(v, sizeof(v), &red);
    label_set(s_ts_status, v);
    ui_text_tone(s_ts_status, red ? UI_TONE_RED : UI_TONE_MUTED);
    snprintf(v, sizeof(v), tr(STR_NET_TS_N_ONLINE), peers_online());
    label_set(s_ts_peers, v);
}
static void panel_ts_async(void *arg)
{
    (void)arg;
    s_ts_queued = false;
    ts_rows_set();   /* nothing when the panel went meanwhile */
}
static void panel_ts_ev(void *user)
{
    (void)user;
    if (s_ts_queued) return;
    s_ts_queued = true;
    if (!ui_async(panel_ts_async, NULL)) s_ts_queued = false;   /* never posted: the next event tries again (M2) */
}
static void ts_rows_deleted(lv_event_t *e)
{
    (void)e;
    tailscale_set_event_cb(NULL, NULL);
    s_ts_switch = s_ts_state = s_ts_status = s_ts_peers = NULL;
}

/* --------------------------------------------------------------- the panel */
void ui_settings_fill_network(lv_obj_t *p)
{
    char v[200];
    ui_section_header(p, tr(STR_NET_WIFI));
    s_wifi_row = ui_srow(p, UI_ROW_NAV, tr(STR_NET_WIFI), "", wifi_row_cb, NULL);
    lv_obj_add_event_cb(s_wifi_row, wifi_row_deleted, LV_EVENT_DELETE, NULL);
    wifi_row_set();
    wifi_mgr_set_cb(panel_wifi_ev, NULL);
    snprintf(v, sizeof(v), tr(STR_NET_SAVED_N), wifi_mgr_saved_count());
    ui_srow(p, UI_ROW_NAV, tr(STR_NET_NETWORKS), v, networks_cb, NULL);
    static_value(v, sizeof(v));
    ui_srow(p, UI_ROW_NAV, tr(STR_NET_ADVANCED), v, advanced_cb, NULL);

    /* Tailscale, U10's order; every row stays with the switch off, so a key can be set first */
    ui_section_header(p, tr(STR_NET_TS));
    bool on = tailscale_enabled();
    lv_obj_t *r = ui_srow(p, UI_ROW_SWITCH, tr(STR_NET_TS), on ? "1" : NULL, ts_switch_cb, NULL);
    s_ts_switch = ui_srow_value(r);
    s_ts_state = ui_text(r, "", UI_TONE_MUTED, true);
    lv_obj_move_to_index(s_ts_state, 1);   /* "off · saved" before the switch (the row's own 16 px gap) */
    lv_obj_t *status = ui_srow(p, UI_ROW_INFO, tr(STR_NET_TS_STATUS), "", NULL, NULL);
    s_ts_status = ui_srow_value(status);
    ui_srow(p, UI_ROW_NAV, tr(STR_NET_TS_AUTH_KEY), tr(g_settings.ts_auth_key[0] ? STR_NET_TS_KEY_SET : STR_NET_TS_KEY_NOT_SET),
            key_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_NET_TS_NAME), device_name(), name_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_NET_TS_CTRL), g_settings.ts_ctrl_host[0] ? g_settings.ts_ctrl_host : tr(STR_NET_TS_CTRL_DEFAULT),
            ctrl_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_NET_TS_CONN), tr(g_settings.ts_direct ? STR_NET_TS_DIRECT : STR_NET_TS_RELAY), conn_cb, NULL);
    ui_footnote(p, tr(STR_NET_TS_APPLIES));
    s_ts_peers = ui_srow_value(ui_srow(p, UI_ROW_NAV, tr(STR_NET_TS_PEERS), "", peers_cb, NULL));
    ts_rows_set();
    /* the panel holds the Tailscale slot while its Status row exists; Peers takes it over (B2: the old screen
       is deleted before the next is built), and the row's delete gives it back */
    lv_obj_add_event_cb(status, ts_rows_deleted, LV_EVENT_DELETE, NULL);
    s_ts_queued = false;
    tailscale_set_event_cb(panel_ts_ev, NULL);
    r = ui_srow(p, UI_ROW_ACTION, tr(STR_NET_TS_FORGET), NULL, forget_cb, NULL);
    ui_text_tone(lv_obj_get_child(r, 0), UI_TONE_RED);
}

/* ============================================================ sub-pages */
static ui_net_sub_t s_sub;
static lv_obj_t *s_body;   /* the shown sub-page's scrolling column; NULL when none */

static void sub_refill(void);

/* ---------------------------------------------------------------- Networks */
static wifi_scan_item_t *s_scan;   /* PSRAM-sized (malloc'd per page), freed with it */
static int s_scan_n;               /* results; -1 while scanning, -2 when the scan didn't start */
static char s_pick[33];            /* the SSID a password prompt is for */

static void scan_start(void)
{
    s_scan_n = wifi_mgr_scan_start() ? -1 : -2;
}

static void join(const char *ssid, const char *pass)
{
    if (wifi_mgr_is_connected() && !strcmp(wifi_mgr_ssid(), ssid)) {   /* a rejoin would drop the Host link for nothing */
        ui_toastf(tr(STR_NET_JOINED), ssid);
        return;
    }
    /* copies first: ssid and pass may point into the saved table, which saving the join rewrites */
    char s[33], k[65];
    strlcpy(s, ssid, sizeof(s));
    strlcpy(k, pass ? pass : "", sizeof(k));
    wifi_mgr_connect(s, k, true);
    memset(k, 0, sizeof(k));
    ui_toastf(tr(STR_NET_JOINING), s);
}

/* A saved row's SSID, read from its label at tap time: the saved table may have changed since the page was built
   (a join saves at once; `wifi forget`), so a build-time index could name another network (T8 M1). */
static const wifi_saved_t *saved_of_row(lv_obj_t *row)
{
    int i = saved_index(lv_label_get_text(lv_obj_get_child(row, 0)));
    return i >= 0 ? wifi_mgr_saved(i) : NULL;
}
static void saved_tap_cb(lv_event_t *e)
{
    const wifi_saved_t *w = saved_of_row(lv_event_get_current_target_obj(e));
    if (w) join(w->ssid, w->pass);
    else sub_refill();   /* forgotten meanwhile */
}
static void saved_forget_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    int i = saved_index(s_pick);
    if (i >= 0) wifi_mgr_forget(i);
    ui_toastf(tr(STR_NET_FORGOTTEN), s_pick);
    sub_refill();
}
static void saved_forget_cb(lv_event_t *e)
{
    /* the button's line: its first child is the row */
    const wifi_saved_t *w = saved_of_row(lv_obj_get_child(lv_obj_get_parent(lv_event_get_current_target_obj(e)), 0));
    if (!w) { sub_refill(); return; }
    strlcpy(s_pick, w->ssid, sizeof(s_pick));
    char q[96];
    snprintf(q, sizeof(q), tr(STR_NET_FORGET_Q), s_pick);
    ui_confirm_ex(q, tr(STR_NET_FORGET_BODY), tr(STR_NET_FORGET), true, saved_forget_yes, NULL);
}

static void pass_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (v) join(s_pick, v);
}
static void ask_password(const char *ssid)
{
    strlcpy(s_pick, ssid, sizeof(s_pick));
    char t[64];
    snprintf(t, sizeof(t), tr(STR_NET_PASSWORD_FOR), s_pick);
    ui_prompt(t, tr(STR_NET_PASSWORD_HINT), "", true, pass_prompt_cb, NULL);
}
static void avail_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_scan || i < 0 || i >= s_scan_n) return;
    const wifi_scan_item_t *it = &s_scan[i];
    int si = saved_index(it->ssid);
    if (si >= 0) join(wifi_mgr_saved(si)->ssid, wifi_mgr_saved(si)->pass);   /* the saved password */
    else if (!it->auth) join(it->ssid, "");
    else ask_password(it->ssid);
}
static void other_ssid_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    char b[33];
    strlcpy(b, v, sizeof(b));
    char *t = trim(b);
    if (*t) ask_password(t);
}
static void other_cb(lv_event_t *e)
{
    (void)e;
    ui_prompt(tr(STR_NET_JOIN_OTHER), tr(STR_NET_SSID), "", false, other_ssid_cb, NULL);
}
static void scan_cb(lv_event_t *e)
{
    (void)e;
    scan_start();
    sub_refill();
}

/* A saved network: the row connects; the trash button beside it forgets (both 64 px, 8 apart). */
static void saved_line(lv_obj_t *p, const wifi_saved_t *w)
{
    lv_obj_t *line = ui_srow_line(p);
    char v[64] = "";
    if (wifi_mgr_is_connected() && !strcmp(wifi_mgr_ssid(), w->ssid))
        snprintf(v, sizeof(v), tr(STR_NET_CONNECTED), signal_word(sysinfo_rssi_cached()));
    lv_obj_t *r = ui_srow(line, UI_ROW_ACTION, w->ssid, v, saved_tap_cb, NULL);
    lv_obj_set_flex_grow(r, 1);
    if (v[0]) ui_text_tone(ui_srow_value(r), UI_TONE_GREEN);
    lv_obj_t *b = ui_btn(line, UI_BTN_DESTRUCTIVE, LV_SYMBOL_TRASH, saved_forget_cb, NULL);
    lv_obj_set_size(b, UI_TK_TARGET, UI_TK_TARGET);
    lv_obj_center(lv_obj_get_child(b, 0));
}

static void fill_networks(lv_obj_t *p)
{
    ui_section_header(p, tr(STR_NET_SAVED));
    int n = wifi_mgr_saved_count();
    if (!n) ui_footnote(p, tr(STR_NET_NO_SAVED));
    for (int i = 0; i < n; i++) {
        const wifi_saved_t *w = wifi_mgr_saved(i);
        if (w) saved_line(p, w);
    }
    ui_section_header(p, tr(STR_NET_AVAILABLE));
    if (s_scan_n == -1) ui_footnote(p, tr(STR_NET_SCANNING));
    else if (s_scan_n == -2) ui_footnote(p, tr(STR_NET_SCAN_FAILED));
    else {
        int shown = 0;
        for (int i = 0; i < s_scan_n; i++) {
            const wifi_scan_item_t *it = &s_scan[i];
            if (!it->ssid[0]) continue;   /* a hidden network: Join other… */
            bool dup = false;
            for (int j = 0; j < i && !dup; j++) dup = !strcmp(s_scan[j].ssid, it->ssid);
            if (dup) continue;            /* another access point of a network already listed */
            char v[48];
            snprintf(v, sizeof(v), "%s%s%s", signal_word(it->rssi), it->auth ? " " : "", it->auth ? UI_GLYPH_LOCK : "");
            ui_srow(p, UI_ROW_ACTION, it->ssid, v, avail_tap_cb, (void *)(intptr_t)i);
            shown++;
        }
        if (!shown) ui_footnote(p, tr(STR_NET_SCAN_NONE));
    }
    ui_srow(p, UI_ROW_ACTION, tr(STR_NET_JOIN_OTHER), NULL, other_cb, NULL);
}

static void sub_wifi_async(void *arg)
{
    wifi_ev_t ev = (wifi_ev_t)(intptr_t)arg;
    if (!s_body || s_sub != UI_NET_SUB_NETWORKS) return;   /* the page went meanwhile */
    if (ev == WIFI_EV_SCAN_DONE) s_scan_n = s_scan ? wifi_mgr_scan_results(s_scan, SCAN_MAX) : 0;
    else if (ev == WIFI_EV_CONNECTED) { ui_toastf(tr(STR_NET_JOINED), wifi_mgr_ssid()); sysinfo_start_sntp(); }
    else if (ev == WIFI_EV_CONNECT_FAILED) ui_toast(tr(STR_NET_JOIN_FAILED));
    else if (ev != WIFI_EV_DISCONNECTED) return;
    sub_refill();
}
static void sub_wifi_ev(wifi_ev_t ev, void *user) { (void)user; ui_async(sub_wifi_async, (void *)(intptr_t)ev); }

/* ---------------------------------------------------------------- Advanced */
static void sip_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    while (*v == ' ') v++;
    if (!*v) {
        /* Plan 3 review N2: a failed save leaves the static address on: say so */
        ui_toast(tr(wifi_mgr_static_ip_set(NULL) ? STR_NET_STATIC_OFF_DONE : STR_NET_STATIC_NOT_SAVED));
        sub_refill();
        return;
    }
    sip_t p;
    if (!sip_parse(v, &p)) { ui_toast(tr(STR_NET_STATIC_BAD)); return; }
    wifi_static_ip_t s = { .on = 1, .ip = p.ip, .mask = p.mask, .gw = p.gw, .dns = p.dns };
    ui_toast(tr(wifi_mgr_static_ip_set(&s) ? STR_NET_STATIC_ON : STR_NET_STATIC_REFUSED));
    sub_refill();
}
static void sip_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    static_initial(cur, sizeof(cur));
    ui_prompt(tr(STR_NET_STATIC_PROMPT), "192.168.1.50/24 192.168.1.1", cur, false, sip_prompt_cb, NULL);
}
static void fill_advanced(lv_obj_t *p)
{
    char v[64];
    static_value(v, sizeof(v));
    ui_srow(p, UI_ROW_VALUE, tr(STR_NET_STATIC), v, sip_cb, NULL);
    ui_footnote(p, tr(STR_NET_STATIC_NOTE));
}

/* ------------------------------------------------------------------ Peers */
static ts_peer_t *s_peers;   /* the list as shown (malloc'd, so a tap names the peer it showed) */
static int s_peer_n;
static ts_peer_t s_sel_peer;
static volatile bool s_peers_queued;   /* one ui_async in flight at most, however many events arrive */

static int peer_cmp(const void *a, const void *b)
{
    const ts_peer_t *x = a, *y = b;
    if (x->online != y->online) return x->online ? -1 : 1;   /* online first */
    return strcmp(x->hostname, y->hostname);
}

static void peer_sheet_cb(int i, void *ud)
{
    (void)ud;
    if (i == 0) ui_settings_add_peer_host(&s_sel_peer);
}
static void peer_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_peers || i < 0 || i >= s_peer_n) return;
    s_sel_peer = s_peers[i];
    const char *items[1] = { tr(STR_NET_ADD_SSH_HOST) };
    ui_action_sheet(s_sel_peer.hostname, items, 1, 0, peer_sheet_cb, NULL);
}

static void fill_peers(lv_obj_t *p)
{
    free(s_peers);
    s_peers = NULL;
    s_peer_n = 0;
    int n = tailscale_peer_count();
    if (n > PEERS_MAX) n = PEERS_MAX;
    if (n > 0) s_peers = malloc(sizeof(ts_peer_t) * (size_t)n);
    for (int i = 0; s_peers && i < n; i++)
        if (tailscale_peer(i, &s_peers[s_peer_n])) s_peer_n++;
    if (s_peer_n > 1) qsort(s_peers, (size_t)s_peer_n, sizeof(ts_peer_t), peer_cmp);
    if (!s_peer_n) {
        ui_empty_state(p, tr(tailscale_enabled() ? STR_NET_PEERS_NONE : STR_NET_PEERS_TS_OFF), NULL, NULL, NULL);
        return;
    }
    for (int i = 0; i < s_peer_n; i++) {
        const ts_peer_t *q = &s_peers[i];
        char v[64];
        snprintf(v, sizeof(v), tr(STR_NET_PEER_LINE), q->ip, tr(q->online ? STR_NET_PEER_ONLINE : STR_NET_PEER_OFFLINE),
                 tr(q->direct ? STR_NET_PEER_DIRECT : STR_NET_PEER_RELAY));
        ui_srow(p, UI_ROW_ACTION, q->hostname, v, peer_tap_cb, (void *)(intptr_t)i);
    }
}

static void peers_async(void *arg)
{
    (void)arg;
    s_peers_queued = false;
    if (s_body && s_sub == UI_NET_SUB_PEERS) sub_refill();
}
static void peers_ev(void *user)
{
    (void)user;
    if (s_peers_queued) return;
    s_peers_queued = true;
    if (!ui_async(peers_async, NULL)) s_peers_queued = false;   /* never posted: the next event tries again (M2) */
}
static void refresh_cb(lv_event_t *e) { (void)e; sub_refill(); }

/* Terminal, then its host form prefilled with the peer (its name, and its tailnet IP as the address): Back from the
   form lands on Terminal, which then lists the new host. */
void ui_settings_add_peer_host(const ts_peer_t *p)
{
    static hf_arg_t a;
    if (!p) return;
    /* C1: the Host's own peer (by its tailnet IP, or its MagicDNS name's first label) is never added */
    hf_host_addrs_t *h = malloc(sizeof(*h));
    if (!h) return;
    cc_link_host_addresses(h);
    bool host = hf_is_host(p->ip, h) || hf_is_host(p->hostname, h);
    free(h);
    if (host) { ui_toast(tr(STR_HOSTS_NOT_LISTED)); return; }
    memset(&a, 0, sizeof(a));
    a.index = -1;
    a.origin = HF_FROM_TERMINAL;
    strlcpy(a.prefill_name, p->hostname, sizeof(a.prefill_name));
    strlcpy(a.prefill_host, p->ip, sizeof(a.prefill_host));
    ui_nav_push(ui_hosts_create, NULL);
    ui_nav_push(ui_host_form_create, &a);
}

/* -------------------------------------------------------------- the page */
static void sub_refill(void)
{
    if (!s_body) return;
    int32_t y = lv_obj_get_scroll_y(s_body);
    ui_focus_key_t k;
    ui_focus_save(s_body, &k);   /* T13-3: a scan or a Tailscale event keeps the keyboard focus on its row */
    lv_obj_clean(s_body);
    if (s_sub == UI_NET_SUB_NETWORKS) fill_networks(s_body);
    else if (s_sub == UI_NET_SUB_ADVANCED) fill_advanced(s_body);
    else fill_peers(s_body);
    lv_obj_update_layout(s_body);
    lv_obj_scroll_to_y(s_body, y, LV_ANIM_OFF);
    ui_focus_restore(s_body, &k);
}

static void sub_deleted(lv_event_t *e)
{
    (void)e;
    if (s_sub == UI_NET_SUB_NETWORKS) wifi_mgr_set_cb(NULL, NULL);
    if (s_sub == UI_NET_SUB_PEERS) tailscale_set_event_cb(NULL, NULL);   /* the slot Terminal shares (Review Focus 5) */
    s_body = NULL;
    free(s_scan);
    s_scan = NULL;
    free(s_peers);
    s_peers = NULL;
    s_peer_n = 0;
}

lv_obj_t *ui_settings_sub_create(void *arg)
{
    s_sub = (ui_net_sub_t)(intptr_t)arg;
    if (s_sub > UI_NET_SUB_PEERS) s_sub = UI_NET_SUB_NETWORKS;
    static const str_id_t titles[] = { STR_NET_NETWORKS, STR_NET_ADVANCED, STR_NET_TS_PEERS };
    lv_obj_t *scr = ui_screen_base();
    lv_obj_add_event_cb(scr, sub_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *h = ui_header(scr, tr(titles[s_sub]), tr(STR_NET_BACK_SETTINGS));
    lv_obj_t *slot = ui_header_slot(h);
    lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (s_sub == UI_NET_SUB_NETWORKS) ui_chip(slot, tr(STR_NET_SCAN), scan_cb, NULL);
    else if (s_sub == UI_NET_SUB_PEERS) ui_chip(slot, tr(STR_NET_REFRESH), refresh_cb, NULL);

    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    ui_settings_column(s_body);   /* the same column as Settings' panel (shared styles) */
    lv_obj_set_pos(s_body, 0, UI_TK_HEADER_H);
    lv_obj_set_size(s_body, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - UI_TK_HEADER_H);

    if (s_sub == UI_NET_SUB_NETWORKS) {
        s_scan = malloc(sizeof(wifi_scan_item_t) * SCAN_MAX);
        wifi_mgr_set_cb(sub_wifi_ev, NULL);
        scan_start();   /* results by WIFI_EV_SCAN_DONE; no timer */
    } else if (s_sub == UI_NET_SUB_PEERS) {
        s_peers_queued = false;
        tailscale_set_event_cb(peers_ev, NULL);
    }
    sub_refill();
    return scr;
}
