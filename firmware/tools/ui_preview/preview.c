#include "ui/ui.h"
#include "term/term_view.h"
#include "ui/ui_boot.h"
#include "ui/ui_comp.h"
#include "ui/ui_sessions.h"
#include "ui/ui_settings.h"
#include "ssh/device_key.h"
#include "ui/hotkeys.h"
#include "checks.h"
#include "src/misc/lv_timer_private.h"
#include "ui/ui_softkbd.h"
#include "settings/settings.h"
#include "net/tailscale.h"
#include "input/keyboard.h"
#include "sys/sysinfo.h"
#include "net/wifi_mgr.h"
#include "companion/cc_link.h"
#include "companion/cc_sessions.h"
#include "servers/server_store.h"
#include "sys/power.h"
#include "sys/checkin.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
app_settings_t g_settings;
static int env_int(const char *n, int d);
void settings_save(void) {}
void settings_reset_defaults(void) {}
static int s_reset_user_calls;
void settings_reset_user(void) { s_reset_user_calls++; }
#define PREVIEW_PUBKEY "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPreviewOnlyNotARealKey0123456789abcdefghijklmnop claude-companion@tab5"
esp_err_t device_key_pubkey_line(char *out, size_t cap) { snprintf(out, cap, "%s", PREVIEW_PUBKEY); return ESP_OK; }
int wifi_mgr_channel(void) { return 6; }
uint32_t sysinfo_uptime_s(void) { return 5025; }
bool sysinfo_charging(void) { return env_int("PREVIEW_CHG", 0) != 0; }
bool g_ssh_session_active=false;
const char *sysinfo_power_src_text(void) { return "USB-C"; }
const char *wifi_mgr_ssid(void) { return "Clientre Wi-Fi"; }
bool sd_is_mounted(void) { return true; }
static int env_int(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
int sysinfo_battery_percent(void) { return env_int("PREVIEW_BAT", 52); }
int sysinfo_rssi_cached(void) { return env_int("PREVIEW_RSSI", -60); }
const char *sysinfo_app_version(void) { return "preview"; }
void sysinfo_apply_timezone(void) {}
void sysinfo_start_sntp(void) {}
static bool s_rc_preview;   /* sessions reconnecting: woken from Radio off, Wi-Fi and the link not back yet */
static bool s_stale_preview;   /* sessions stale: woken from Off, no poll result newer than the wake */
static const char *env_str(const char *n, const char *d) { const char *e = getenv(n); return e && *e ? e : d; }
/* PREVIEW_WIFI=0: no Wi-Fi. PREVIEW_HOST=0: no Host set. PREVIEW_FAIL=notfound: the Host's name doesn't resolve. */
static bool pv_wifi(void) { return env_int("PREVIEW_WIFI", 1) != 0 && !s_rc_preview; }
static bool pv_host(void) { return env_int("PREVIEW_HOST", 1) != 0; }
static bool pv_notfound(void) { return !strcmp(env_str("PREVIEW_FAIL", ""), "notfound"); }
bool wifi_mgr_is_connected(void) { return pv_wifi(); }
bool wifi_mgr_is_connecting(void) { return s_rc_preview; }
void wifi_mgr_get_ip4(char *b, size_t n) { snprintf(b,n,"192.168.1.25"); }
void wifi_mgr_disconnect(void) {}
/* Plan 3 surfaces Settings calls (the preview stopped linking at 8b94efb without them). */
bool wifi_mgr_static_ip_get(wifi_static_ip_t *out) { if (out) memset(out, 0, sizeof(*out)); return true; }
bool wifi_mgr_static_ip_set(const wifi_static_ip_t *in) { (void)in; return true; }
void link_down_restart(const char *why) { (void)why; exit(0); }
bool keyboard_present(void) { return true; }
bool keyboard_usb_present(void) { return false; }
static void *focused_textarea;
void keyboard_lvgl_focus(void *p) { focused_textarea = p; }
void *keyboard_lvgl_focused(void) { return focused_textarea; }
void keyboard_set_layout(uint8_t layout) { (void)layout; }
void keyboard_set_led(uint8_t a,uint8_t b,uint8_t c,uint8_t d,uint8_t e) {}
void power_kbd_light_refresh(void) {}
/* power owner: the preview is always Active */
bool power_setting_set(const char *name, int n) { (void)name; (void)n; return true; }
bool hibernate_supported(void) { return false; }
int power_cfg(power_cfg_t c) {
    return c == PCFG_RADIO_OFF ? g_settings.pw_radio_off : c == PCFG_RADIO_AFTER ? g_settings.pw_radio_off_min
         : c == PCFG_CHECKIN ? g_settings.pw_checkin_min : g_settings.pw_hibernate_checkin_min;
}
power_boot_t hibernate_boot_kind(void) { return POWER_BOOT_NORMAL; }
void power_hold(power_hold_t h, bool on) { (void)h; (void)on; }
uint32_t power_wake_seq(void) { return s_rc_preview || s_stale_preview ? 1 : 0; }
uint32_t power_wake_gen(void) { return s_rc_preview || s_stale_preview ? 1 : 0; }   /* no result newer than the wake: stale */
power_state_t power_woke_from(void) { return s_rc_preview ? PS_RADIO_OFF : s_stale_preview ? PS_OFF : PS_ACTIVE; }
bool checkin_woke_parked(void) { return s_rc_preview; }
power_wake_t power_wake_reason(void) { return PW_CONSOLE; }
bool power_shield_up(void) { return false; }
ts_state_t tailscale_state(void)
{
    int m = env_int("PREVIEW_TS", 2);
    return m == 0 ? TS_OFF : m == 1 ? TS_CONNECTING : m == 3 ? TS_ERROR : TS_CONNECTED;
}
bool tailscale_start(void) { return true; }
void tailscale_stop(void) {}
bool tailscale_enabled(void) { return g_settings.ts_enable != 0; }
bool tailscale_stopping(void) { return false; }
void tailscale_set_enabled(bool on) { g_settings.ts_enable = on; }
const char *tailscale_state_text(void) { return tailscale_state() == TS_CONNECTED ? "Connected" : tailscale_state() == TS_OFF ? "Off" : "Connecting"; }
const char *tailscale_vpn_ip(void) { return tailscale_state() == TS_CONNECTED ? "100.101.102.103" : ""; }
/* ---- Settings › Network (Task 8): stubs that record what the screens did ----
   PREVIEW_TS=3: TS_ERROR. PREVIEW_SD=0: no card; PREVIEW_SD=nofile: a card without the key file. */
#define PREVIEW_TS_KEY "tskey-auth-preview-NOTAREALKEY-0123456789"
static ts_event_cb_t s_ts_cb; static void *s_ts_cb_user; static int s_ts_cb_sets;
void tailscale_set_event_cb(ts_event_cb_t cb, void *user) { s_ts_cb = cb; s_ts_cb_user = user; s_ts_cb_sets++; }
const char *tailscale_last_error(void) { return env_int("PREVIEW_TS", 2) == 3 ? "Auth key expired: enter a new one" : ""; }
bool tailscale_home_derp_code(char *out, size_t cap) { snprintf(out, cap, "%s", tailscale_state() == TS_CONNECTED ? "sea" : ""); return out[0] != 0; }
static int s_ts_forget_calls;
bool tailscale_forget_node(void) { s_ts_forget_calls++; return true; }
bool tailscale_forgetting(void) { return false; }
static const ts_peer_t k_preview_peers[] = {
    { "preview-host-a", "100.64.0.11", true, false, false, 10 },
    { "preview-host-b", "100.64.0.12", false, false, false, 10 },
    { "preview-host-c", "100.64.0.13", true, true, false, 10 },
};
static int s_peer_reads;
int tailscale_peer_count(void) { s_peer_reads++; return tailscale_state() == TS_CONNECTED ? 3 : 0; }
bool tailscale_peer(int i, ts_peer_t *out) { if (tailscale_state() != TS_CONNECTED || i < 0 || i >= 3) return false; *out = k_preview_peers[i]; return true; }
static wifi_saved_t s_preview_saved[4] = { { "Clientre Wi-Fi", "preview-pass-1" }, { "Cabin", "preview-pass-2" } };
static int s_preview_saved_n = 2;
static int s_wifi_forget_calls, s_wifi_connect_calls, s_scan_starts;
static char s_wifi_connect_ssid[33];
int wifi_mgr_saved_count(void) { return s_preview_saved_n; }
const wifi_saved_t *wifi_mgr_saved(int i) { return i >= 0 && i < s_preview_saved_n ? &s_preview_saved[i] : NULL; }
void wifi_mgr_forget(int i) {
    s_wifi_forget_calls++;
    if (i < 0 || i >= s_preview_saved_n) return;
    memmove(&s_preview_saved[i], &s_preview_saved[i + 1], sizeof(s_preview_saved[0]) * (size_t)(s_preview_saved_n - i - 1));
    s_preview_saved_n--;
}
void wifi_mgr_connect(const char *ssid, const char *pass, bool save) { (void)pass; (void)save; s_wifi_connect_calls++; snprintf(s_wifi_connect_ssid, sizeof(s_wifi_connect_ssid), "%s", ssid); }
static wifi_ev_cb_t s_wifi_cb; static void *s_wifi_cb_user;
void wifi_mgr_set_cb(wifi_ev_cb_t cb, void *user) { s_wifi_cb = cb; s_wifi_cb_user = user; }
bool wifi_mgr_scan_start(void) { s_scan_starts++; return true; }
static const wifi_scan_item_t k_preview_scan[] = {
    { "Clientre Wi-Fi", -52, 3, 6 }, { "Neighbour 5G", -71, 3, 36 }, { "Library guest", -80, 0, 11 },
    { "", -60, 3, 1 }, { "Neighbour 5G", -77, 3, 40 },
};
int wifi_mgr_scan_results(wifi_scan_item_t *out, int max) {
    int n = (int)(sizeof(k_preview_scan) / sizeof(k_preview_scan[0]));
    if (n > max) n = max;
    memcpy(out, k_preview_scan, sizeof(k_preview_scan[0]) * (size_t)n);
    return n;
}
bool sd_mount(void) { return strcmp(env_str("PREVIEW_SD", "1"), "0") != 0; }
bool fs_read_all(const char *path, char **out, size_t *len, size_t max) {
    (void)max;
    if (strcmp(path, "/sdcard/tailscale/authkey.txt") || !strcmp(env_str("PREVIEW_SD", "1"), "nofile")) return false;
    *out = strdup("tskey-auth-fromsd-NOTAREALKEY\nsecond line\n");
    *len = strlen(*out);
    return true;
}
bool keyboard_any_present(void) { return true; }
void keyboard_inject(const key_event_t *ev) { (void)ev; }
void esp_restart(void) {}
void bsp_display_brightness_set(int v) {}
bool server_store_set_on_sd(bool on) { return true; }
void server_store_reload(void) {}
int server_store_count(void) { return 3; }
static server_t s_preview_server = { .name = "Mac", .host = "mac", .port = 22, .user = "me", .use_key = true };
const server_t *server_store_get(int i) { return i >= 0 && i < 3 ? &s_preview_server : NULL; }
pr_src_t power_source(int *pct, bool *charging)
{
    bool chg = env_int("PREVIEW_CHG", 0) != 0;
    if (pct) *pct = env_int("PREVIEW_BAT", 52);
    if (charging) *charging = chg;
    return chg ? PR_SRC_USB : PR_SRC_BATTERY;
}
power_info_t sysinfo_power(void)
{
    power_info_t p = { .percent = env_int("PREVIEW_BAT", 52), .valid = true };
    p.src = env_int("PREVIEW_CHG", 0) ? PWR_USB_CHARGING : PWR_BATTERY;
    p.current_a = env_int("PREVIEW_CHG", 0) ? -0.5f : 0.2f;   /* vitals_charging reads the current (+ discharging) */
    return p;
}
static key_sink_t preview_sink; static bool preview_have_sink;   /* the sessions cases drive its key sink */
void keyboard_set_sink(const key_sink_t *sink) { if (sink) { preview_sink = *sink; preview_have_sink = true; } else preview_have_sink = false; }
void keyboard_clear_sink(void *user) { if (preview_have_sink && preview_sink.user == user) preview_have_sink = false; }
void ui_terminal_open_command(int i, const char *c, void (*cb)(void *), void *ctx) { (void)i; (void)c; if (cb) cb(ctx); }
bool ui_terminal_is_top(void) { return false; }   /* the preview builds no terminal screen yet */
/* companion link: Sessions reads PREVIEW_FIXTURE (default agents_synthetic.json; "loading": no list yet;
   "none": an empty list) from the host test fixtures */
static cc_session_t s_preview_sessions[16];
static int s_preview_n = -1;
static cc_sessions_cb_t s_poll_cb;
static void *s_poll_ctx;
static uint32_t s_poll_gen = 1;
static void preview_sessions_load(void) {
    const char *fx = env_str("PREVIEW_FIXTURE", "agents_synthetic.json");
    if (!strcmp(fx, "loading")) { s_preview_n = -1; return; }
    if (!strcmp(fx, "none")) { s_preview_n = 0; return; }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, fx);
    FILE *f = fopen(path, "rb");
    assert(f);
    static char json[16384]; size_t n = fread(json, 1, sizeof(json) - 1, f); json[n] = 0; fclose(f);
    s_preview_n = cc_sessions_parse(json, s_preview_sessions, 16);
    assert(s_preview_n > 0);
    /* ages: started a few minutes before the preview clock, in the parsed (sorted) order */
    int64_t now = (int64_t)time(NULL) * 1000;
    for (int i = 0; i < s_preview_n; i++) {
        s_preview_sessions[i].started_ms = now - (int64_t)(i + 1) * 7 * 60000;
        /* the fixture has no pids: treat the unfinished ones as running */
        if (s_preview_sessions[i].state != CC_DONE && s_preview_sessions[i].state != CC_ENDED) s_preview_sessions[i].live = true;
    }
}
/* The re-sort hook: a poll that moves session i to the end of the list. */
static void preview_resort_to_last(int i) {
    assert(i >= 0 && i < s_preview_n);
    cc_session_t t = s_preview_sessions[i];
    memmove(&s_preview_sessions[i], &s_preview_sessions[i + 1], sizeof(t) * (size_t)(s_preview_n - i - 1));
    s_preview_sessions[s_preview_n - 1] = t;
    s_poll_gen++;
    if (s_poll_cb) s_poll_cb(s_preview_sessions, s_preview_n, s_poll_ctx);
}
int cc_poll_snapshot(cc_session_t *out, int cap) {
    if (s_preview_n < 0) return -1;
    int n = s_preview_n < cap ? s_preview_n : cap;
    if (n > 0) memcpy(out, s_preview_sessions, sizeof(cc_session_t) * n);
    return n;
}
void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx) { s_poll_cb = cb; s_poll_ctx = ctx; }
void cc_poll_set_rate(int s) { (void)s; }
void cc_poll_set_floor(int s) { (void)s; }
uint32_t cc_poll_generation(void) { return s_poll_gen; }
int64_t cc_poll_last_ok_ms(void) { return 1; }
bool cc_link_busy(void) { return false; }
void cc_link_watch_enable(bool on) { (void)on; }
void cc_link_watch_test_fallback(void) {}
const char *cc_watch_status_text(char *b, size_t cap) { snprintf(b, cap, "watch: on I=3s, last block 4s ago, heartbeat 12s ago"); return b; }
bool cc_link_connected(void) { return pv_wifi() && pv_host() && !pv_notfound(); }
/* the device's own wording (cc_link.c), so the banner's cut is tested */
const char *cc_link_status_text(char *b, size_t cap) {
    if (!pv_host()) snprintf(b, cap, "no Host set up");
    else if (pv_notfound()) snprintf(b, cap, "Host not found at my-host.local (set its address in Settings \xe2\x86\x92 Network) (retry in 12s)");
    else snprintf(b, cap, cc_link_connected() ? "connected my-host" : "connecting to my-host");
    return b;
}
cc_link_fail_t cc_link_failure(void) { return pv_notfound() ? CC_LINK_FAIL_NOT_FOUND : CC_LINK_FAIL_NONE; }
bool cc_link_mac_address(char *o, size_t cap) { snprintf(o, cap, "100.101.102.103"); return true; }
bool cc_link_mac_path(char *o, size_t cap) { snprintf(o, cap, "lan 192.168.1.50"); return true; }
bool cc_link_set_lan_fallback(const char *ip) { (void)ip; return true; }
const char *cc_link_lan_fallback(char *o, size_t cap) { snprintf(o, cap, "192.168.1.50"); return o; }
int cc_poll_failures(bool *too_big) { if (too_big) *too_big = false; return 0; }
const char *cc_settings_claude_path(char *o, size_t cap) { snprintf(o, cap, "~/.local/bin/claude"); return o; }
const char *cc_settings_server_name(char *o, size_t cap) { snprintf(o, cap, "%s", pv_host() ? "my-host" : ""); return o; }
int cc_settings_server(void) { return pv_host() ? 0 : -1; }
void cc_link_set_server(const char *n) { (void)n; }
bool cc_link_set_claude_path(const char *p) { (void)p; return true; }
/* PREVIEW_LOG=long: a 300-character line with no spaces, CJK and a numbered list (Review Focus 3). */
static int s_paste_calls;
esp_err_t cc_link_exec_tail(const char *cmd, char *out, size_t cap, int *st, char *err, size_t ecap, int ms) {
    (void)cmd; (void)ms; if (ecap) err[0] = 0; if (st) *st = 0;
    if (!strcmp(env_str("PREVIEW_LOG", ""), "long")) {
        char line[301];
        for (int i = 0; i < 300; i++) line[i] = (char)('a' + i % 26);
        line[300] = 0;
        snprintf(out, cap, "\xe2\x8f\xba Read src/%s.ts\r\n%s\r\n"
                 "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x88\xe2\x96\x88 \xe2\x80\xa6 \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 \xe2\x9c\x93 \xe2\x97\x8f \xe2\x86\x90 \xe2\x86\x93 46k tokens | [\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91] 7%% reset | 51%% 3d 14h weekly reset \xe2\x80\xa6\r\n"
                 "\xe2\x8f\xba \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82"
                 "\xe9\x95\xb7\xe3\x81\x84\xe8\xa1\x8c\xe3\x81\x8c\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x95\xe3\x82\x8c\xe3\x82\x8b\xe3\x81\x8b"
                 "\xe7\xa2\xba\xe8\xaa\x8d\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xa7\xe3\x81\x99"
                 "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xa7\xe3\x81\x99\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\r\n", line, line);
        return ESP_OK;
    }
    snprintf(out, cap, "\x1b[1m\xe2\x8f\xba\x1b[0m Read\x1b[1C3\x1b[1Cfiles\r\n  \xe2\x8e\xbf  src/main.c\r\n"
             "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\r\n\xe2\x8f\xba Which colour should the theme use?\r\n"
             "  1. Amber\r\n  2. Green\r\n  3. Blue\r\n");
    return ESP_OK;
}
esp_err_t cc_link_paste(const char *id, const char *t, char *err, size_t cap) { (void)id; (void)t; s_paste_calls++; if (cap) err[0] = 0; return ESP_OK; }
static int s_check_calls;
esp_err_t cc_link_check_live(const char *id, char *err, size_t cap) { (void)id; s_check_calls++; if (cap) err[0] = 0; return ESP_OK; }
static int s_forget_calls;
#define PREVIEW_FP "SHA256:Xq3v9yPreviewFingerprintOnly0123456789abcd"
esp_err_t cc_link_forget_host_key(char *fp, size_t cap, int ms) {
    (void)ms; s_forget_calls++;
    snprintf(fp, cap, PREVIEW_FP);
    return ESP_OK;
}
uint32_t cc_link_forget_done(char *fp, size_t cap) { snprintf(fp, cap, PREVIEW_FP); return (uint32_t)s_forget_calls; }
bool cc_link_pinned_fingerprint(char *fp, size_t cap) { snprintf(fp, cap, "SHA256:OldPreviewKeyOnly9876543210zyxwvutsrqponm"); return true; }
esp_err_t cc_link_debug_corrupt_host_key(char *h, size_t cap) { if (cap) h[0] = 0; return ESP_OK; }
#define SCREEN(name) lv_obj_t *name(void *p) { return ui_screen_base(); }
SCREEN(ui_hosts_create)
SCREEN(ui_server_form_create)
static int snapshot(lv_obj_t *s, const char *path) {
    int w=lv_obj_get_width(s), h=lv_obj_get_height(s);
    lv_draw_buf_t *snap=lv_snapshot_take(s,LV_COLOR_FORMAT_RGB888);
    if(!snap) return 1;
    FILE *f=fopen(path,"wb");
    if (!f) return 1;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        uint8_t *p=snap->data+y*snap->header.stride+x*3;
        fputc(p[2],f); fputc(p[1],f); fputc(p[0],f);
    }
    fclose(f); lv_draw_buf_destroy(snap); return 0;
}
/* The screen with lv_layer_top (modals, toasts) blended over it. */
static int snapshot_top(lv_obj_t *s, const char *path) {
    int w=lv_obj_get_width(s), h=lv_obj_get_height(s);
    lv_draw_buf_t *base=lv_snapshot_take(s,LV_COLOR_FORMAT_RGB888);
    lv_draw_buf_t *top=lv_snapshot_take(lv_layer_top(),LV_COLOR_FORMAT_ARGB8888);
    if(!base||!top) return 1;
    FILE *f=fopen(path,"wb");
    if (!f) return 1;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        uint8_t *p=base->data+y*base->header.stride+x*3;
        uint8_t *q=top->data+y*top->header.stride+x*4;
        unsigned a=q[3];
        for (int c=2;c>=0;c--) fputc((q[c]*a+p[c]*(255-a))/255,f);
    }
    fclose(f); lv_draw_buf_destroy(base); lv_draw_buf_destroy(top); return 0;
}
/* The label under o whose text is t (or starts with t when prefix is set). */
static lv_obj_t *find_label(lv_obj_t *o, const char *t, bool prefix) {
    if (lv_obj_check_type(o,&lv_label_class)) {
        const char *x=lv_label_get_text(o);
        if (prefix ? !strncmp(x,t,strlen(t)) : !strcmp(x,t)) return o;
    }
    for (uint32_t i=0;i<lv_obj_get_child_count(o);i++) {
        lv_obj_t *r=find_label(lv_obj_get_child(o,i),t,prefix);
        if (r) return r;
    }
    return NULL;
}
/* The first clickable object at or above that label. */
static lv_obj_t *find_clickable(lv_obj_t *o, const char *t) {
    o=find_label(o,t,false);
    while (o && !lv_obj_has_flag(o,LV_OBJ_FLAG_CLICKABLE)) o=lv_obj_get_parent(o);
    return o;
}
static uint64_t flushed_pixels;
static unsigned flushes;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p) {
    flushed_pixels += (uint64_t)lv_area_get_width(a) * lv_area_get_height(a);
    flushes++;
    lv_display_flush_ready(d);
}
static void step(lv_display_t *d, unsigned ms) {
    lv_tick_inc(ms); lv_timer_handler(); lv_refr_now(d);
}
static int keyboard_clicks, keyboard_menus, keyboard_releases, keyboard_answer;
static void keyboard_test_cb(lv_event_t *e) {
    if (lv_event_get_code(e)==LV_EVENT_CLICKED) keyboard_clicks++;
    if (lv_event_get_code(e)==LV_EVENT_LONG_PRESSED) keyboard_menus++;
    if (lv_event_get_code(e)==LV_EVENT_RELEASED) keyboard_releases++;
}
static void keyboard_confirmed(bool yes, void *ud) { keyboard_answer=yes ? 1 : -1; }
static bool press_key(special_key_t key, uint8_t mods, bool terminal) {
    key_event_t ev={.type=KEY_EV_SPECIAL,.key=key,.mods=mods};
    return ui_keyboard_route(&ev,terminal);
}
static void keyboard_check(void) {
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    lv_obj_t *hidden=ui_button(scr,"hidden",keyboard_test_cb,NULL);
    lv_obj_add_flag(hidden,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *disabled=ui_button(scr,"disabled",keyboard_test_cb,NULL);
    lv_obj_add_state(disabled,LV_STATE_DISABLED);
    lv_obj_t *touch_only=lv_obj_create(scr);
    lv_obj_add_flag(touch_only,UI_KEYBOARD_SKIP);
    ui_button(touch_only,"soft key",keyboard_test_cb,NULL);
    lv_obj_t *button=ui_button(scr,"action",keyboard_test_cb,NULL);
    lv_obj_add_event_cb(button,keyboard_test_cb,LV_EVENT_LONG_PRESSED,NULL);
    lv_obj_t *slider=lv_slider_create(scr); lv_slider_set_range(slider,0,100);
    lv_obj_add_event_cb(slider,keyboard_test_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_t *ta=ui_textarea(scr,"text",false,true);
    press_key(SK_TAB,0,false); assert(lv_obj_has_state(button,LV_STATE_FOCUS_KEY));
    press_key(SK_ENTER,0,false); assert(keyboard_clicks==1);
    press_key(SK_ENTER,MOD_CTRL,false); assert(keyboard_menus==1);
    press_key(SK_TAB,0,false); assert(lv_obj_has_state(slider,LV_STATE_FOCUS_KEY));
    press_key(SK_RIGHT,0,false); assert(lv_slider_get_value(slider)==1 && keyboard_releases==1);
    press_key(SK_TAB,0,false); assert(keyboard_lvgl_focused()==ta);
    assert(!press_key(SK_LEFT,0,false));
    press_key(SK_TAB,MOD_SHIFT,false); assert(keyboard_lvgl_focused()==NULL);
    ui_confirm("test",NULL,keyboard_confirmed,NULL);
    press_key(SK_ESC,0,true); assert(keyboard_answer==-1 && keyboard_clicks==1);
    ui_confirm("test",NULL,keyboard_confirmed,NULL);
    press_key(SK_TAB,0,true); press_key(SK_TAB,0,true); press_key(SK_ENTER,0,true);
    assert(keyboard_answer==1);
    assert(!press_key(SK_TAB,0,true)); /* untouched SSH Tab */
    press_key(SK_TAB,MOD_CTRL,true); assert(ui_keyboard_owns_input());
    press_key(SK_ESC,0,true); assert(!ui_keyboard_owns_input());
    assert(!press_key(SK_ESC,0,true)); /* untouched SSH Esc */
    press_key(SK_TAB,0,false);
    lv_obj_delete(button); /* focused deletion must not leave a dangling target */
    press_key(SK_TAB,0,false); press_key(SK_RIGHT,0,false);
    ui_prompt("test","", "",false,NULL,NULL);
    press_key(SK_TAB,0,true); assert(keyboard_lvgl_focused()!=NULL);
    press_key(SK_ESC,0,true); assert(keyboard_lvgl_focused()==NULL);
    keyboard_lvgl_focus(ta); /* mixed touch/physical input follows the touched field */
    assert(!press_key(SK_RIGHT,0,false)); assert(lv_obj_has_state(ta,LV_STATE_FOCUS_KEY));
    ui_confirm("outer",NULL,keyboard_confirmed,NULL);
    ui_alert("inner","test");
    press_key(SK_ESC,0,true); assert(ui_keyboard_owns_input());
    press_key(SK_ESC,0,true); assert(!ui_keyboard_owns_input());
    lv_obj_t *sw=ui_switch_row(scr,"switch",false,NULL,NULL);
    keyboard_lvgl_focus(ta);
    press_key(SK_RIGHT,0,false); press_key(SK_TAB,0,false);
    lv_obj_t *toggle=lv_obj_get_user_data(sw);
    assert(lv_obj_has_state(toggle,LV_STATE_FOCUS_KEY));
    press_key(SK_ENTER,0,false); assert(lv_obj_has_state(toggle,LV_STATE_CHECKED));
    press_key(SK_ENTER,0,false); assert(!lv_obj_has_state(toggle,LV_STATE_CHECKED));
    puts("Keyboard navigation, slider persistence, modal isolation, mixed input, switches, deletion and SSH routing checks passed.");
}

static void terminal_spacing_check(void) {
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    fonts_set_term_line_height(32);
    vt_t *vt=vt_create(80,16,10);
    term_view_t *tv=term_view_create(scr,vt,&g_term_font);
    lv_obj_set_size(term_view_obj(tv),960,512);
    int cols,rows;
    term_view_fit(tv,&cols,&rows);
    assert(cols==80 && rows==16 && term_view_cell_h(tv)==32);
    const char *sample="ABCDEFGHIJKLMNOPQRSTUVWXYZ gjpqy 0123456789 日本語テスト\r\n";
    for(int i=0;i<12;i++) vt_feed(vt,(const uint8_t *)sample,strlen(sample));
    const char *hide="\033[?25l";
    vt_feed(vt,(const uint8_t *)hide,strlen(hide));
    lv_draw_buf_t *snap=lv_snapshot_take(term_view_obj(tv),LV_COLOR_FORMAT_RGB888);
    assert(snap);
    /* At least four wholly blank scanlines between every pair of rendered rows. */
    for(int row=0;row<11;row++) {
        int last=-1,first=32;
        for(int y=0;y<64;y++) {
            bool ink=false;
            for(int x=0;x<960;x++) {
                uint8_t *p=snap->data+(row*32+y)*snap->header.stride+x*3;
                if(p[0]||p[1]||p[2]) { ink=true; break; }
            }
            if(ink && y<32) last=y;
            if(ink && y>=32 && first==32) first=y-32;
        }
        assert(last>=0 && first<32 && 32+first-last-1>=4);
    }
    lv_draw_buf_destroy(snap);
    assert(snapshot(scr,"terminal.ppm")==0);
    fonts_set_term_line_height(38);
    term_view_fit(tv,&cols,&rows);
    assert(term_view_cell_h(tv)==38 && rows==13);
    term_view_destroy(tv); vt_destroy(vt);
    puts("Terminal: 32px rows, visible inter-row whitespace and live 38px resize passed.");
}

static lv_point_t test_pointer;
static bool test_pointer_down;
static unsigned test_swipes;
static void pointer_read(lv_indev_t *in, lv_indev_data_t *data) {
    (void)in; data->point=test_pointer;
    data->state=test_pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void count_swipe(lv_dir_t dir) { if(dir==LV_DIR_LEFT || dir==LV_DIR_RIGHT) test_swipes++; }
static void pointer_move(lv_indev_t *in,int x,int y,bool down) {
    test_pointer=(lv_point_t){x,y}; test_pointer_down=down;
    lv_tick_inc(20); lv_indev_read(in);
}
static void gesture_check(void) {
    lv_indev_t *in=lv_indev_create(); lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in,pointer_read); ui_init();
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    ui_screen_set_gesture(scr,count_swipe);
    lv_obj_t *slider=lv_slider_create(scr); lv_obj_set_pos(slider,50,80); lv_obj_set_size(slider,400,40);
    lv_obj_update_layout(scr);
    pointer_move(in,100,100,true);
    for(int x=130;x<=400;x+=30) pointer_move(in,x,100,true);
    pointer_move(in,400,100,false);
    assert(lv_slider_get_value(slider)>50 && test_swipes==0);
    pointer_move(in,100,300,true);
    for(int x=130;x<=400;x+=30) pointer_move(in,x,300,true);
    pointer_move(in,400,300,false);
    assert(test_swipes==1);
    lv_indev_delete(in);
    puts("Touch slider drag changes volume control without track swipe; background swipe retained.");
}
static void terminal_art_check(void) {
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&g_term_font,&glyph,0x23f5,0));
    assert(!glyph.is_placeholder && glyph.adv_w==12);
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    fonts_set_term_line_height(32);
    vt_t *vt=vt_create(60,12,0);
    term_view_t *tv=term_view_create(scr,vt,&g_term_font);
    lv_obj_set_size(term_view_obj(tv),720,408);
    const char *art="[⏵⏵] ASCII 日本語\r\n  ▐▛███▜▌\r\n ▝▜█████▛▘\r\n   ▘▘ ▝▝\r\n█████\r\n█████\r\n";
    vt_feed(vt,(const uint8_t *)art,strlen(art));
    const char *hide="\033[?25l"; vt_feed(vt,(const uint8_t *)hide,strlen(hide));
    const vt_cell_t *row=vt_view_row(vt,0);
    assert(row[1].cp==0x23f5 && row[2].cp==0x23f5 && row[3].cp==']' && row[5].cp=='A');
    lv_draw_buf_t *snap=lv_snapshot_take(term_view_obj(tv),LV_COLOR_FORMAT_RGB888); assert(snap);
    for(int y=4*32;y<6*32;y++) for(int x=0;x<5*12;x++) {
        uint8_t *p=snap->data+y*snap->header.stride+x*3;
        assert(p[0] || p[1] || p[2]); /* no seams in solid block artwork */
    }
    lv_draw_buf_destroy(snap);
    assert(snapshot(scr,"art.ppm")==0);
    term_view_destroy(tv); vt_destroy(vt);
    puts("CLI triangles are one cell; full-height block artwork has no gaps across columns or rows.");
}

/* ---------------------------------------------------------------- nav */
/* P4-2: a probe screen's create function runs after the previous screen is already deleted. */
static lv_obj_t *s_probe_prev;
static unsigned s_probe_runs;
static lv_obj_t *probe_settings_create(void *arg) {
    assert(s_probe_prev && !lv_obj_is_valid(s_probe_prev));
    s_probe_runs++;
    return ui_settings_create(arg);
}
static void noop_cb(lv_event_t *e) { (void)e; }
static bool s_boot_deleted;
static void boot_deleted_cb(lv_event_t *e) { (void)e; s_boot_deleted = true; }
/* Every Task 4 component on one screen, with stub sessions: the checks run on it with no skips. */
static lv_obj_t *probe_gallery_create(void *arg) {
    (void)arg;
    assert(s_probe_prev && !lv_obj_is_valid(s_probe_prev));
    s_probe_runs++;
    lv_obj_t *scr = ui_screen_base();
    lv_obj_t *h = ui_header(scr, "Components", "Sessions");
    ui_chip(ui_header_slot(h), "Save", noop_cb, NULL);
    ui_header_needs_you_set(ui_header_needs_you(h), 2);
    lv_obj_t *banner = ui_banner(scr, UI_TK_HEADER_H);
    ui_banner_set(banner, UI_BANNER_MUTED, "Updating\xe2\x80\xa6", NULL, NULL, NULL);
    ui_banner_set(banner, UI_BANNER_RED, "Can't reach the host: connection refused", "Host settings", noop_cb, NULL);
    lv_obj_t *f = ui_footer(scr);
    ui_chip(ui_footer_group(f, false), "\xe2\x86\x91\xe2\x86\x93 Select", noop_cb, NULL);
    ui_chip(ui_footer_group(f, false), "Send", noop_cb, NULL);
    ui_chip(ui_footer_group(f, true), "Settings", noop_cb, NULL);
    int top = UI_TK_HEADER_H + UI_TK_BANNER_H + UI_TK_GAP_M;
    int body_h = 720 - top - UI_TK_FOOTER_H - UI_TK_GAP_M;
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_style_pad_all(list, UI_TK_GAP_S, 0);   /* room for the 80 px hit areas */
    lv_obj_set_pos(list, UI_TK_MARGIN, top);
    lv_obj_set_size(list, UI_TK_LIST_W, body_h);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, UI_TK_GAP_S, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    static cc_session_t ss[4] = {
        { .id = "a1", .name = "auth-refactor-with-a-name-far-too-long-to-fit-in-the-row", .repo = "api",
          .state = CC_NEEDS_YOU, .background = true, .live = true },
        { .id = "b2", .name = "deps-bump", .repo = "web", .state = CC_NEEDS_YOU, .background = true, .live = true },
        { .id = "c3", .name = "flaky-tests", .repo = "api", .state = CC_WORKING, .background = true, .live = true },
        { .id = "d4", .name = "docs-pass", .repo = "docs", .state = CC_IDLE, .background = true, .live = true },
    };
    const char *meta[4] = { "~/code/api \xc2\xb7 7m", "~/code/web \xc2\xb7 12m", "~/code/api \xc2\xb7 1h", "~/code/docs \xc2\xb7 2d" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = ui_session_row(list, noop_cb, NULL);
        ui_session_row_set(r, &ss[i], i == 0, meta[i]);
        ui_session_row_set(r, &ss[i], i == 0, meta[i]);   /* unchanged: a no-op */
    }
    ui_section_header(list, "Section header");
    ui_srow(list, UI_ROW_ACTION, "Reboot", NULL, noop_cb, NULL);
    ui_skeleton_row(list);
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_style_pad_all(col, UI_TK_GAP_S, 0);   /* room for the 80 px hit areas */
    int x = UI_TK_MARGIN + UI_TK_LIST_W + UI_TK_GAP_L;
    lv_obj_set_pos(col, x, top);
    lv_obj_set_size(col, 1280 - x - UI_TK_MARGIN, body_h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, UI_TK_GAP_S, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *buttons = lv_obj_create(col);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_style_pad_all(buttons, UI_TK_GAP_S, 0);   /* room for the 80 px hit areas */
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, UI_TK_GAP_M, 0);
    lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    ui_btn(buttons, UI_BTN_PRIMARY, "Send", noop_cb, NULL);
    ui_btn(buttons, UI_BTN_SECONDARY, "Open", noop_cb, NULL);
    ui_btn(buttons, UI_BTN_DESTRUCTIVE, "Forget", noop_cb, NULL);
    lv_obj_t *pill = ui_state_pill(buttons);
    ui_state_pill_set(pill, &ss[2]);
    ui_srow(col, UI_ROW_VALUE, "Fallback address", "192.168.1.50", noop_cb, NULL);
    ui_srow(col, UI_ROW_SWITCH, "Tailscale", "on", noop_cb, NULL);
    ui_srow(col, UI_ROW_NAV, "Host", "my-host.local", noop_cb, NULL);
    ui_srow_slider(col, "Brightness", 5, 100, 58, noop_cb, NULL);
    ui_srow(col, UI_ROW_INFO, "Link", "connected over home Wi-Fi", NULL, NULL);
    ui_footnote(col, "A footnote wraps onto as many lines as it needs, in the muted label role.");
    return scr;
}

static int nav_case(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d,400);
    s_probe_prev = lv_screen_active();
    ui_nav_push(probe_settings_create, NULL); step(d,100);
    assert(s_probe_runs == 1 && ui_nav_is_top(probe_settings_create));
    ui_nav_back(); step(d,100);
    assert(ui_nav_is_top(ui_sessions_create));
    s_probe_prev = lv_screen_active();
    ui_nav_push(probe_gallery_create, NULL); step(d,100);
    assert(s_probe_runs == 2);
    /* the toast: one line at the bottom centre */
    ui_toast("A toast is one line at the bottom centre, cut with dots when it is longer than the screen is wide, "
             "which this one is");
    step(d,100);
    int r = pv_run_checks(d, lv_screen_active(), "nav", true, 0);
    step(d,3100);
    assert(lv_obj_get_child_count(lv_layer_top()) == 0);   /* gone after 3 s */
    /* the picker and the confirm over it */
    const char *items[] = { "Lowest", "Low", "Medium" };
    ui_picker("Needs-you light when the screen is dark", items, 3, 1, NULL, NULL);
    step(d,100);
    r |= pv_run_checks(d, lv_screen_active(), "picker", false, 0);
    lv_obj_clean(lv_layer_top());
    ui_confirm_ex("Forget this host's key? The Tab5 trusts the next key the host offers, so only do this after the key changed.",
                  "Only do this if the host's SSH key changed.", "Forget", true, NULL, NULL);
    step(d,100);
    r |= pv_run_checks(d, lv_screen_active(), "confirm", false, 0);
    lv_obj_clean(lv_layer_top());
    return r;
}

/* P4C-5: a hit area LVGL clips at a parent does not count. A 64 px button in a container of exactly its
 * size gets 64 px of hit area whatever its ext_click_area, so the check must fail it; the bars' chips must
 * reach the full bar height and 80 px across. */
static int targets_clip_case(lv_display_t *d) {
    char why[256];
    lv_obj_t *scr = ui_screen_base(); lv_screen_load(scr);
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 200, 300);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui_btn(box, UI_BTN_PRIMARY, "Clipped", noop_cb, NULL);
    lv_obj_update_layout(scr); lv_refr_now(d);
    int clipped = pv_touch_targets(scr, why, sizeof(why));
    printf("targets-clip: clipped button -> %s\n", clipped ? why : "passed (wrong)");
    lv_obj_delete(box);
    /* the real bars: every chip reaches the bar's full height and 80 px across */
    lv_obj_t *h = ui_header(scr, "Bars", "Sessions");
    ui_chip(ui_header_slot(h), "Save", noop_cb, NULL);
    lv_obj_t *f = ui_footer(scr);
    ui_chip(ui_footer_group(f, false), "Send", noop_cb, NULL);
    ui_chip(ui_footer_group(f, true), "Keys", noop_cb, NULL);
    lv_obj_t *b = ui_banner(scr, UI_TK_HEADER_H);
    ui_banner_set(b, UI_BANNER_RED, "Can't reach the host", "Host settings", noop_cb, NULL);
    lv_obj_update_layout(scr); lv_refr_now(d);
    int bars = pv_touch_targets(scr, why, sizeof(why));
    printf("targets-clip: bars -> %s\n", bars ? why : "ok");
    /* T4 carry: a dialog's short end button (OK, Forget) keeps its 80 px hit area inside the button row */
    ui_alert("Short", "An alert's only button is OK.");
    lv_obj_update_layout(lv_layer_top()); lv_refr_now(d);
    int ok_btn = pv_touch_targets(lv_layer_top(), why, sizeof(why));
    printf("targets-clip: dialog OK -> %s\n", ok_btn ? why : "ok");
    lv_obj_clean(lv_layer_top());
    /* the shortest label: the end button at its 64 px minimum width, so only the row's own padding gives it 80 */
    ui_confirm_ex("A one-letter action", NULL, "Y", true, NULL, NULL);
    lv_obj_update_layout(lv_layer_top()); lv_refr_now(d);
    int forget_btn = pv_touch_targets(lv_layer_top(), why, sizeof(why));
    printf("targets-clip: dialog 64 px end button -> %s\n", forget_btn ? why : "ok");
    lv_obj_clean(lv_layer_top());
    /* T7 review I1: a scroller with no padding clips its rows' 80 px hit areas, and no scroll undoes it at the top
       (scroll 0) or at the sides, so the check must report it, not skip it as scrolled out */
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 200, 100);
    lv_obj_set_size(col, 600, 400);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, UI_TK_GAP_S, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    for (int i = 0; i < 10; i++) {
        char t[16]; snprintf(t, sizeof(t), "Row %d", i);
        ui_srow(col, UI_ROW_NAV, t, NULL, noop_cb, NULL);
    }
    lv_obj_update_layout(scr); lv_refr_now(d);
    int scroll_clip = pv_touch_targets(scr, why, sizeof(why));
    printf("targets-clip: unpadded scroller -> %s\n", scroll_clip ? why : "passed (wrong)");
    lv_obj_delete(col);
    if (!clipped || bars || ok_btn || forget_btn || !scroll_clip) { printf("check targets-clip: FAIL\n"); return 1; }
    printf("check targets-clip: ok\n");
    return 0;
}

/* ------------------------------------------------------------ sessions */
static bool sink(key_event_t ev) { return preview_have_sink && preview_sink.key(&ev, preview_sink.user); }
static bool sink_ch(uint32_t cp, uint8_t mods) { return sink((key_event_t){ .type = KEY_EV_CHAR, .cp = cp, .mods = mods }); }
static bool sink_sk(special_key_t k) { return sink((key_event_t){ .type = KEY_EV_SPECIAL, .key = k }); }
static void sink_text(const char *t) { for (; *t; t++) assert(sink_ch((uint8_t)*t, 0)); }
static bool obj_shown(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
/* The first shown object of class c under o. */
static lv_obj_t *find_shown(lv_obj_t *o, const lv_obj_class_t *c) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, c)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_shown(lv_obj_get_child(o, i), c);
        if (r) return r;
    }
    return NULL;
}
/* A shown label with text t whose left edge is at or right of x (the detail pane starts at 440). */
static lv_obj_t *find_label_x(lv_obj_t *o, const char *t, int32_t x) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), t)) {
        lv_area_t a; lv_obj_get_coords(o, &a);
        if (a.x1 >= x) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_label_x(lv_obj_get_child(o, i), t, x);
        if (r) return r;
    }
    return NULL;
}
/* The session row whose (uncut) name label is t: a label left of the pane, in row > column > name. */
static lv_obj_t *find_label_left(lv_obj_t *o, const char *t) {
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), t)) {
        lv_area_t a; lv_obj_get_coords(o, &a);
        if (a.x2 < 440) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_label_left(lv_obj_get_child(o, i), t);
        if (r) return r;
    }
    return NULL;
}
static lv_obj_t *row_named(lv_obj_t *scr, const char *t) {
    lv_obj_t *l = find_label_left(scr, t);
    return l ? lv_obj_get_parent(lv_obj_get_parent(l)) : NULL;
}
/* Shown skeleton rows: 72 px panel-2 placeholders with no children. */
static int skeleton_rows(lv_obj_t *o) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
    int n = 0;
    if (!lv_obj_get_child_count(o) && lv_obj_get_height(o) == UI_TK_SESSION_ROW_H && !lv_obj_check_type(o, &lv_label_class) &&
        lv_color_eq(lv_obj_get_style_bg_color(o, 0), lv_color_hex(UI_TK_PANEL2))) n++;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n += skeleton_rows(lv_obj_get_child(o, i));
    return n;
}
/* A shown label right of x whose text contains t. */
static lv_obj_t *find_label_has(lv_obj_t *o, const char *t, int32_t x) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), t)) {
        lv_area_t a; lv_obj_get_coords(o, &a);
        if (a.x1 >= x) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_label_has(lv_obj_get_child(o, i), t, x);
        if (r) return r;
    }
    return NULL;
}
static bool row_selected(lv_obj_t *row) { return row && lv_obj_get_style_outline_width(row, 0) == 2; }
static const char *field_text(lv_obj_t *scr) {
    lv_obj_t *ta = find_shown(scr, &lv_textarea_class);
    return ta ? lv_textarea_get_text(ta) : NULL;
}
static int selected_index(lv_obj_t *list) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++)
        if (obj_shown(lv_obj_get_child(list, i)) && row_selected(lv_obj_get_child(list, i))) return (int)i;
    return -1;
}

static int sessions_case(lv_display_t *d, const char *v) {
    char name[64];
    snprintf(name, sizeof(name), !strcmp(v, "ctrlk") ? "ctrlk" : "sessions-%s", v);
    bool needs = false;
    if (!strcmp(v, "working")) setenv("PREVIEW_FIXTURE", "agents_working.json", 1);
    if (!strcmp(v, "loading")) setenv("PREVIEW_FIXTURE", "loading", 1);
    if (!strcmp(v, "empty")) setenv("PREVIEW_FIXTURE", "none", 1);
    if (!strcmp(v, "16") || !strcmp(v, "resort-scroll")) setenv("PREVIEW_FIXTURE", "agents_16.json", 1);
    if (!strcmp(v, "long") || !strcmp(v, "interactive")) { setenv("PREVIEW_FIXTURE", "agents_long.json", 1); setenv("PREVIEW_LOG", "long", 1); }
    if (!strcmp(v, "no-wifi")) setenv("PREVIEW_WIFI", "0", 1);
    if (!strcmp(v, "no-host")) setenv("PREVIEW_HOST", "0", 1);
    if (!strcmp(v, "not-found")) setenv("PREVIEW_FAIL", "notfound", 1);
    if (!strcmp(v, "low-battery")) setenv("PREVIEW_BAT", "14", 1);
    if (!strcmp(v, "reconnecting")) s_rc_preview = true;
    if (!strcmp(v, "stale")) s_stale_preview = true;
    preview_sessions_load();
    ui_nav_home(); step(d, 400); step(d, 400);
    assert(ui_nav_is_top(ui_sessions_create) && preview_have_sink);
    lv_obj_t *s = lv_screen_active();
    lv_obj_update_layout(s); lv_refr_now(d);
    char why[256];
    lv_obj_t *row0 = s_preview_n > 0 ? row_named(s, s_preview_sessions[0].name) : NULL;
    lv_obj_t *list = row0 ? lv_obj_get_parent(row0) : NULL;
    if (!strcmp(v, "live") || !strcmp(v, "selected-needs-you") || !strcmp(v, "resort") || !strcmp(v, "16") ||
        !strcmp(v, "tap-resort") || !strcmp(v, "resort-scroll")) needs = true;
    if (!strcmp(v, "live")) {
        assert(find_label(s, "\xe2\x97\x8f 2 need you", false));
        assert(list && selected_index(list) == 0);
        assert(find_label(s, "1\xe2\x80\x93" "3 picks an option", false));   /* the tail ends in a 3-item list */
        /* review carry (T4 re-review): no bar control's hit area reaches the first row's */
        if (pv_bar_hits_clear(s, row0, why, sizeof(why))) { printf("check %s: FAIL bar hits: %s\n", name, why); return 1; }
    } else if (!strcmp(v, "selected-needs-you")) {
        assert(sink_sk(SK_DOWN)); step(d, 400);
        assert(selected_index(list) == 1 && cc_session_needs_you(&s_preview_sessions[1]));
        assert(find_label_x(s, s_preview_sessions[1].name, 440));
    } else if (!strcmp(v, "working")) {
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber drawn with no session needing you\n", name); return 1; }
        assert(!find_label(s, "\xe2\x97\x8f", true));
    } else if (!strcmp(v, "loading")) {
        assert(find_label(s, "Loading sessions\xe2\x80\xa6", false));
        assert(skeleton_rows(s) == 3);
    } else if (!strcmp(v, "stale")) {
        assert(find_label(s, "Updating\xe2\x80\xa6", false));
    } else if (!strcmp(v, "reconnecting")) {
        assert(find_label(s, "Reconnecting to host\xe2\x80\xa6", false));
        assert(!find_label(s, "No Wi-Fi", true));
        assert(lv_obj_get_style_opa(list, 0) == LV_OPA_50);
    } else if (!strcmp(v, "no-wifi")) {
        assert(find_label(s, "No Wi-Fi", false) && find_clickable(s, "Wi-Fi settings"));
        assert(lv_obj_get_style_opa(list, 0) == LV_OPA_50);
        /* inert: a tap on another row selects nothing */
        lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(selected_index(list) == 0);
        if (pv_bar_hits_clear(s, row0, why, sizeof(why))) { printf("check %s: FAIL bar hits: %s\n", name, why); return 1; }
    } else if (!strcmp(v, "not-found")) {
        assert(find_label(s, "Host not found at my-host.local", false) && find_clickable(s, "Host settings"));
    } else if (!strcmp(v, "no-host")) {
        lv_obj_t *b = find_clickable(s, "Set up host");
        assert(b && obj_shown(b));
        assert(!find_shown(s, &lv_textarea_class));
        assert(!row0 || !obj_shown(row0));
    } else if (!strcmp(v, "empty")) {
        assert(find_label(s, "No sessions on this host. Start one with `claude --bg`.", false));
    } else if (!strcmp(v, "interactive")) {
        /* select the interactive blocked session */
        int k = -1;
        for (int i = 0; i < s_preview_n; i++) if (!s_preview_sessions[i].background && s_preview_sessions[i].state == CC_NEEDS_YOU) k = i;
        assert(k >= 0);
        for (int i = 0; i < k; i++) { assert(sink_sk(SK_DOWN)); step(d, 400); }
        assert(selected_index(list) == k);
        assert(find_label(s, "Interactive \xe2\x80\x94 answer it on the Host", false));
        assert(!find_shown(s, &lv_textarea_class));
        assert(find_label(s, "waiting on Host", false));
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber on a session that can't need you\n", name); return 1; }
    } else if (!strcmp(v, "sent")) {
        assert(sink_ch('2', 0)); step(d, 400);
        assert(!strcmp(field_text(s), "2"));
        assert(sink_sk(SK_ENTER)); step(d, 400);
        assert(s_paste_calls == 1);
        assert(find_label(s, "Sent \xe2\x9c\x93", false));
        assert(!strcmp(field_text(s), ""));   /* the draft is cleared after a send */
    } else if (!strcmp(v, "low-battery")) {
        assert(find_label(s, "Battery 14 %. The Tab5 sleeps at 10 %.", false));
    } else if (!strcmp(v, "16")) {
        int shown = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) shown += obj_shown(lv_obj_get_child(list, i));
        assert(shown == 16);
        lv_obj_t *last = lv_obj_get_child(list, 15);
        lv_obj_scroll_to_view(last, LV_ANIM_OFF);
        lv_obj_update_layout(s);
        lv_area_t a, l; lv_obj_get_coords(last, &a); lv_obj_get_coords(list, &l);
        assert(a.y1 >= l.y1 && a.y2 <= l.y2);
        step(d, 400);
    } else if (!strcmp(v, "long")) {
        /* the 95-character name: cut in its row, whole (wrapped) in the pane */
        int k = -1;
        for (int i = 0; i < s_preview_n; i++) if (strlen(s_preview_sessions[i].name) == 95) k = i;
        assert(k >= 0);
        for (int i = 0; i < k; i++) { assert(sink_sk(SK_DOWN)); step(d, 400); }
        lv_obj_t *t = find_label_x(s, s_preview_sessions[k].name, 440);
        assert(t && lv_obj_get_height(t) > lv_font_get_line_height(FONT_BODY));   /* wrapped */
        assert(!find_label_x(s, s_preview_sessions[k].name, 0) || find_label_x(s, s_preview_sessions[k].name, 0) == t);
        /* CJK in the mono log: confirm the cell width the wrap assumes (Review Focus 3, T5 carry) */
        lv_font_glyph_dsc_t g;
        assert(lv_font_get_glyph_dsc(FONT_MONO, &g, 0x65E5, 0) && !g.is_placeholder);
        printf("sessions long: U+65E5 advances %u px in FONT_MONO (cell 12), 'M' %u px\n", (unsigned)g.adv_w,
               (unsigned)lv_font_get_glyph_width(FONT_MONO, 'M', 0));
        assert(g.adv_w == 24);   /* two cells: sm_wrap_tail counts it as 2 (vt_wcwidth) */
        /* the glyphs a Claude Code log tail carries: each must advance as many 12 px cells as vt_wcwidth says */
        static const uint32_t tui[] = { 0x2026, 0x2190, 0x2193, 0x23F5, 0x23FA, 0x2588, 0x2591, 0x2722, 0x2733, 0x2736,
                                        0x273B, 0x273D, 0x276F, 0x00A0, 0x00B7, 0x2500, 0x256D, 0x23BF, 0x2713, 0x25CF };
        for (unsigned i = 0; i < sizeof(tui) / sizeof(tui[0]); i++) {
            lv_font_glyph_dsc_t t;
            bool ok = lv_font_get_glyph_dsc(FONT_MONO, &t, tui[i], 0);
            if (!ok || t.adv_w != 12 * (unsigned)vt_wcwidth(tui[i]))
                printf("sessions long: U+%04X advances %u px (%s), vt_wcwidth %d\n", (unsigned)tui[i], (unsigned)t.adv_w,
                       ok && !t.is_placeholder ? "drawn" : "missing", vt_wcwidth(tui[i]));
        }
        /* the CJK line and the 300-character one are in the block, and every row fits its width unwrapped */
        lv_obj_t *log = find_label_has(s, "\xe6\x97\xa5\xe6\x9c\xac", 440);
        assert(log && lv_obj_get_style_text_font(log, 0) == FONT_MONO);
        assert(find_label_has(s, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", 440) == log);
        assert(find_label_has(s, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91", 440) == log);   /* the TUI-symbol line */
        int32_t w = lv_obj_get_content_width(lv_obj_get_parent(log)), rows = 0;   /* the block, not the label */
        char line[512];
        for (const char *p = lv_label_get_text(log); *p; ) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            snprintf(line, sizeof(line), "%.*s", (int)n, p);
            lv_point_t sz;
            lv_text_get_size(&sz, line, FONT_MONO, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x > w) { printf("check %s: FAIL log row %d is %d px, the block %d\n", name, (int)rows, (int)sz.x, (int)w); return 1; }
            rows++;
            p += n + (e ? 1 : 0);
        }
        assert(lv_obj_get_height(log) == rows * lv_font_get_line_height(FONT_MONO));   /* LVGL wrapped nothing more */
        printf("sessions long: %d log rows, block %d px wide (%d cells)\n", (int)rows, (int)w, (int)(w / 12));
    } else if (!strcmp(v, "resort")) {
        /* Review Focus 1: select B, type, re-sort B to the end, tick */
        const int b = 1;
        char bname[96]; snprintf(bname, sizeof(bname), "%s", s_preview_sessions[b].name);
        assert(sink_sk(SK_DOWN)); step(d, 400);
        sink_text("abc"); step(d, 400);
        preview_resort_to_last(b); step(d, 400);
        lv_obj_update_layout(s);
        assert(find_label_x(s, bname, 440));
        assert(!strcmp(field_text(s), "abc"));
        assert(row_selected(row_named(s, bname)) && selected_index(list) == s_preview_n - 1);
    } else if (!strcmp(v, "tap-resort")) {
        /* review I1: a tap belongs to the session under the finger at the press. Press the last row, then a
           poll moves the selected session (row 0) into that slot before the release: nothing opens, and the
           selection stays on the session it was on. */
        int last = s_preview_n - 1;
        char sel[96]; snprintf(sel, sizeof(sel), "%s", s_preview_sessions[0].name);
        lv_obj_t *r = lv_obj_get_child(list, last);
        lv_obj_send_event(r, LV_EVENT_PRESSED, NULL);
        preview_resort_to_last(0); step(d, 400);
        assert(row_selected(row_named(s, sel)) && selected_index(list) == last);
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(s_check_calls == 0 && ui_nav_is_top(ui_sessions_create));
        assert(row_selected(row_named(s, sel)) && find_label_x(s, sel, 440));
        /* a whole tap on the selected row still opens it (the check runs; the stub terminal closes at once) */
        lv_obj_send_event(r, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(s_check_calls == 1);
        s = lv_screen_active();
    } else if (!strcmp(v, "resort-scroll")) {
        /* review M1: a re-sort that moves the selected session out of view scrolls the list to it */
        preview_resort_to_last(0); step(d, 400);
        lv_obj_update_layout(s);
        int32_t x, y;
        assert(ui_sessions_selected_center(&x, &y));
        lv_area_t l; lv_obj_get_coords(list, &l);
        if (y < l.y1 || y > l.y2) { printf("check %s: FAIL selected centre %d,%d outside the list %d-%d\n", name, (int)x, (int)y, (int)l.y1, (int)l.y2); return 1; }
        assert(selected_index(list) == s_preview_n - 1);
    } else if (!strcmp(v, "keys")) {
        assert(sink_ch('k', MOD_CTRL) && ui_sessions_keys_overlay_shown());
        /* Ctrl+Tab is not taken by buttons mode while the overlay is up; the sink closes it */
        key_event_t ct = { .type = KEY_EV_SPECIAL, .key = SK_TAB, .mods = MOD_CTRL };
        assert(!ui_keyboard_route(&ct, true) && !ui_keyboard_owns_input());
        assert(sink_ch('z', 0) && !ui_sessions_keys_overlay_shown());
        assert(!strcmp(field_text(s), ""));   /* the closing key is eaten */
        assert(sink_ch('t', MOD_CTRL)); step(d, 400);
        assert(ui_nav_is_top(ui_hosts_create));
        ui_nav_back(); step(d, 400);
        assert(sink_ch('m', MOD_CTRL)); step(d, 400);
        assert(ui_nav_is_top(ui_hosts_create));
        ui_nav_back(); step(d, 400);
        s = lv_screen_active();
        sink_text("ab"); step(d, 400);
        assert(!strcmp(field_text(s), "ab"));
        assert(sink_sk(SK_ESC)); step(d, 400);
        assert(ui_nav_is_top(ui_sessions_create) && !strcmp(field_text(s), ""));   /* Esc clears, never leaves */
        sink_text("cd");
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        assert(!strcmp(field_text(s), "cd"));   /* no space reaches the field */
        assert(find_label(lv_layer_top(), HK_VOICE_TOAST, false));
        step(d, 3100);
        assert(sink_ch('p', MOD_CTRL)); step(d, 400);
        assert(ui_nav_is_top(ui_settings_create));
        ui_nav_back(); step(d, 400);
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_sessions_create) && !strcmp(field_text(s), "cd"));   /* the draft outlives the visit */
    } else if (!strcmp(v, "ctrlk")) {
        assert(sink_ch('k', MOD_CTRL) && ui_sessions_keys_overlay_shown());
        step(d, 100);
        /* two columns, nothing wrapped */
        lv_obj_t *t = find_label(s, "Keyboard shortcuts", false);
        assert(t);
        lv_obj_t *sheet = lv_obj_get_parent(t);
        for (uint32_t i = 1; i < lv_obj_get_child_count(sheet); i++) {
            lv_obj_t *r = lv_obj_get_child(sheet, i);
            for (uint32_t j = 0; j < lv_obj_get_child_count(r); j++) {
                lv_obj_t *l = lv_obj_get_child(r, j);
                assert(lv_obj_get_height(l) <= lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
            }
        }
        int r = pv_run_checks(d, s, name, needs, 0);
        /* a touch closes it on the press, and its release lands on nothing: a tap over the second row
           neither selects nor opens it */
        lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, pointer_read);
        lv_area_t a; lv_obj_get_coords(lv_obj_get_child(list, 1), &a);
        int px = (a.x1 + a.x2) / 2, py = (a.y1 + a.y2) / 2;
        pointer_move(in, px, py, true); lv_timer_handler();
        pointer_move(in, px, py, false); lv_timer_handler();
        pointer_move(in, px, py, false); step(d, 400);
        assert(!ui_sessions_keys_overlay_shown() && selected_index(list) == 0);
        /* and a tap after it does select the row */
        pointer_move(in, px, py, true); lv_timer_handler();
        pointer_move(in, px, py, false); lv_timer_handler(); step(d, 400);
        assert(selected_index(list) == 1);
        lv_indev_delete(in);
        return r;
    } else if (!strcmp(v, "idle")) {
        unsigned px = pv_idle_pixels(d, 2000);
        if (px) { printf("check %s: FAIL idle: %u px redrawn\n", name, px); return 1; }
        /* and past a log re-fetch (5 s), an age refresh (9 s) and the shared vitals timer (30 s) */
        px = pv_idle_pixels(d, 31000);
        if (px) { printf("check %s: FAIL idle over 31 s: %u px redrawn\n", name, px); return 1; }
        if (pv_timers(true, why, sizeof(why))) { printf("check %s: FAIL timers: %s\n", name, why); return 1; }
    } else if (!strcmp(v, "forget")) {
        /* P4-3: a forget done in time toasts once, from its own result; the tick does not repeat it */
        assert(ui_sessions_forget_host_key());
        step(d, 100);
        assert(find_label(lv_layer_top(), "New host key pinned: " PREVIEW_FP, false));
        step(d, FORGET_TOAST_MS + 200);
        assert(!find_label(lv_layer_top(), "New host key pinned: " PREVIEW_FP, false));
        step(d, 1000);
        assert(!find_label(lv_layer_top(), "New host key pinned", true));
    } else {
        fprintf(stderr, "preview: unknown sessions case %s\n", v);
        return 2;
    }
    s = lv_screen_active();
    return pv_run_checks(d, s, name, needs, 0);
}

/* ------------------------------------------------------------ settings */
static const char *const k_rail_names[UI_SET_COUNT] = { "Host", "Network", "Power", "Keyboard light", "Display & time", "About" };
/* A shown label with text t whose right edge is left of x. */
static lv_obj_t *find_label_before(lv_obj_t *o, const char *t, int32_t x) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), t)) {
        lv_area_t a; lv_obj_get_coords(o, &a);
        if (a.x2 < x) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_label_before(lv_obj_get_child(o, i), t, x);
        if (r) return r;
    }
    return NULL;
}
static lv_obj_t *rail_row(lv_obj_t *scr, int i) {
    lv_obj_update_layout(scr);
    lv_obj_t *l = find_label_before(scr, k_rail_names[i], UI_TK_RAIL_W);
    return l ? lv_obj_get_parent(l) : NULL;
}
static lv_obj_t *settings_panel(lv_obj_t *scr) { return lv_obj_get_child(lv_obj_get_child(scr, 1), 1); }
static int timers_with_period(uint32_t ms) {
    int n = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) n += t->period == ms;
    return n;
}
/* The first clickable object at or above the shown label t under o. */
static lv_obj_t *find_clickable_shown(lv_obj_t *o, const char *t) {
    lv_obj_t *l = find_label(o, t, false);
    while (l && !lv_obj_has_flag(l, LV_OBJ_FLAG_CLICKABLE)) l = lv_obj_get_parent(l);
    return l;
}
static void slug(char *out, size_t cap, const char *t) {
    size_t n = 0;
    for (; *t && n + 1 < cap; t++) out[n++] = (*t >= 'A' && *t <= 'Z') ? (char)(*t + 32) : (*t >= 'a' && *t <= 'z') || (*t >= '0' && *t <= '9') ? *t : '-';
    out[n] = 0;
}

/* ------------------------------------------------------------ Settings › Network (Task 8) */
static const char *const k_ts_rows[] = { "Tailscale", "Status", "Auth key", "Device name", "Control server", "Connection",
                                        "Peers", "Forget this device on the tailnet" };
/* The panel row (a direct child) whose label reads t. */
static lv_obj_t *row_labeled(lv_obj_t *panel, const char *t) {
    for (uint32_t k = 0; k < lv_obj_get_child_count(panel); k++) {
        lv_obj_t *c = lv_obj_get_child(panel, k);
        lv_obj_t *l = lv_obj_get_child_count(c) ? lv_obj_get_child(c, 0) : NULL;
        if (l && lv_obj_check_type(l, &lv_label_class) && !strcmp(lv_label_get_text(l), t) && lv_obj_get_height(c) >= 48) return c;
    }
    return NULL;
}
/* The y of the first shown label t under o (-1: none). */
static int32_t label_y(lv_obj_t *o, const char *t) {
    lv_obj_t *l = find_label(o, t, false);
    if (!l) return -1;
    lv_area_t a; lv_obj_get_coords(l, &a);
    return a.y1;
}
/* A label anywhere on the screen or its dialogs that shows the stub auth key (it must never be shown). */
static lv_obj_t *key_shown(lv_obj_t *s) {
    lv_obj_t *l = find_label_has(s, "tskey-auth-", 0);
    return l ? l : find_label_has(lv_layer_top(), "tskey-auth-", 0);
}
/* The number of action rows in the open sheet titled t. */
static int sheet_rows(const char *t) {
    lv_obj_t *l = find_label(lv_layer_top(), t, false);
    return l ? (int)lv_obj_get_child_count(lv_obj_get_parent(l)) - 1 : -1;
}
static void click_label(lv_display_t *d, lv_obj_t *o, const char *t) {
    lv_obj_t *c = find_clickable_shown(o, t);
    assert(c);
    lv_obj_send_event(c, LV_EVENT_CLICKED, NULL); step(d, 200);
}
/* Types text into the open prompt and presses OK. */
static void prompt_answer(lv_display_t *d, const char *text) {
    lv_obj_t *ta = find_shown(lv_layer_top(), &lv_textarea_class);
    assert(ta);
    lv_textarea_set_text(ta, text);
    click_label(d, lv_layer_top(), "OK");
}

/* settings 1 (Network): returns true when it ran its own checks (r set). */
static bool network_case(lv_display_t *d, lv_obj_t *s, lv_obj_t *panel, const char *click, int scroll, const char *name, int *r) {
    lv_obj_t *top = lv_layer_top();
    if (key_shown(s)) { printf("check %s: FAIL the auth key is shown: \"%s\"\n", name, lv_label_get_text(key_shown(s))); *r = 1; return true; }
    if (!click[0] && !scroll) {
        /* U10's order, one column; every Tailscale row shows with the switch off too */
        int32_t last = -1;
        for (unsigned i = 0; i < sizeof(k_ts_rows) / sizeof(k_ts_rows[0]); i++) {
            lv_obj_t *row = row_labeled(panel, k_ts_rows[i]);
            if (!row) { printf("check %s: FAIL no row \"%s\"\n", name, k_ts_rows[i]); *r = 1; return true; }
            lv_area_t a; lv_obj_get_coords(row, &a);
            if (a.y1 <= last) { printf("check %s: FAIL \"%s\" is out of U10's order\n", name, k_ts_rows[i]); *r = 1; return true; }
            last = a.y1;
            if (lv_obj_get_height(row) < UI_TK_ROW_H) { printf("check %s: FAIL \"%s\" row is %d px\n", name, k_ts_rows[i], (int)lv_obj_get_height(row)); *r = 1; return true; }
        }
        assert(label_y(panel, "Wi-Fi") < label_y(panel, "Networks") && label_y(panel, "Networks") < label_y(panel, "Advanced"));
        assert(label_y(panel, "Advanced") < label_y(panel, "Status"));
        assert(find_label(panel, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 set", false));
        assert(find_label(panel, "Relay only", false) && find_label(panel, "Default (Tailscale)", false));
        assert(find_label(panel, "Applies the next time Tailscale starts", false));
        assert(find_label(panel, "\xe2\x97\x8f connected \xc2\xb7 good", false));   /* -60 dBm */
        assert(!find_label_has(panel, "Mac", 0) && !find_label_has(panel, "Wi-Fi networks", 0));   /* P4-9's bridge is gone */
        int ts = env_int("PREVIEW_TS", 2);
        if (ts == 0) {
            if (!find_label_has(panel, "Off: doesn't run", 0)) { printf("check %s: FAIL Status doesn't read off\n", name); *r = 1; return true; }
            assert(find_label(panel, "off \xc2\xb7 saved", false));
        } else if (ts == 3) {
            lv_obj_t *e = find_label(panel, "Auth key expired: enter a new one", false);
            assert(e && lv_color_eq(lv_obj_get_style_text_color(e, 0), lv_color_hex(UI_TK_RED)));
        } else {
            assert(find_label(panel, "on", false));
            if (!find_label(panel, "connected \xc2\xb7 100.101.102.103 \xc2\xb7 DERP sea \xc2\xb7 2 peers online", false)) {
                printf("check %s: FAIL the Status row\n", name); *r = 1; return true;
            }
            assert(find_label(panel, "2 online", false));
        }
        return false;
    }
    if (scroll && !click[0]) {
        lv_area_t pa; lv_obj_get_coords(panel, &pa);
        const char *want[] = { "Peers", "Forget this device on the tailnet" };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *l = find_label(panel, want[i], false);
            assert(l);
            lv_area_t a; lv_obj_get_coords(lv_obj_get_parent(l), &a);
            if (a.y1 < pa.y1 || a.y2 > pa.y2) { printf("check %s: FAIL \"%s\" at %d-%d, the panel %d-%d\n", name, want[i], (int)a.y1, (int)a.y2, (int)pa.y1, (int)pa.y2); *r = 1; return true; }
        }
        return false;
    }
    if (!strcmp(click, "Auth key")) {
        int n = sheet_rows("Auth key");
        if (n != 3) { printf("check %s: FAIL %d auth key actions\n", name, n); *r = 1; return true; }
        assert(find_label(top, "Enter key", false) && find_label(top, "Load from SD card", false) && find_label(top, "Clear key", false));
        lv_obj_t *clr = find_label(top, "Clear key", false);
        assert(lv_color_eq(lv_obj_get_style_text_color(clr, 0), lv_color_hex(UI_TK_RED)));
        *r |= pv_run_checks(d, s, name, false, 0);
        /* Enter key: a masked field that starts empty, never the saved key */
        click_label(d, top, "Enter key");
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta && lv_textarea_get_password_mode(ta) && !strcmp(lv_textarea_get_text(ta), ""));
        lv_textarea_set_text(ta, "tskey-auth-typed-NOTAREALKEY");
        step(d, 100);
        if (key_shown(s)) { printf("check %s: FAIL the typed key shows: \"%s\"\n", name, lv_label_get_text(key_shown(s))); *r = 1; }
        click_label(d, top, "OK");
        assert(!strcmp(g_settings.ts_auth_key, "tskey-auth-typed-NOTAREALKEY"));
        /* Load from SD card: no card, no file, then the file's first line */
        setenv("PREVIEW_SD", "0", 1);
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Load from SD card");
        assert(find_label(top, "No SD card", false));
        setenv("PREVIEW_SD", "nofile", 1);
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Load from SD card");
        assert(find_label(top, "No key file", false));
        setenv("PREVIEW_SD", "1", 1);
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Load from SD card");
        assert(!strcmp(g_settings.ts_auth_key, "tskey-auth-fromsd-NOTAREALKEY"));
        if (key_shown(s)) { printf("check %s: FAIL the loaded key shows\n", name); *r = 1; }
        /* Clear key: confirmed, destructive; then the row reads Not set and the sheet has two actions */
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Clear key");
        assert(find_label(top, "Clear the auth key?", false));
        click_label(d, top, "Clear");
        assert(!g_settings.ts_auth_key[0] && find_label(settings_panel(s), "Not set", false));
        click_label(d, settings_panel(s), "Auth key");
        assert(sheet_rows("Auth key") == 2);
        lv_obj_clean(top);
        printf("%s: 3 actions, masked entry, SD: no card / no file / loaded, clear confirmed; the key never shown\n", name);
        return true;
    }
    if (!strcmp(click, "Device name")) {
        char before[sizeof(g_settings.ts_device_name)];
        strcpy(before, g_settings.ts_device_name);
        prompt_answer(d, "   ");
        if (strcmp(before, g_settings.ts_device_name)) { printf("check %s: FAIL an empty name was saved\n", name); *r = 1; return true; }
        if (!find_label(top, "A device name can't be empty", false)) { printf("check %s: FAIL no refusal toast\n", name); *r = 1; return true; }
        *r |= pv_run_checks(d, s, name, false, 0);
        lv_obj_clean(top);
        click_label(d, settings_panel(s), "Device name");
        prompt_answer(d, " tab5-desk ");
        assert(!strcmp(g_settings.ts_device_name, "tab5-desk") && find_label(settings_panel(s), "tab5-desk", false));
        lv_obj_clean(top);
        return true;
    }
    if (!strcmp(click, "Forget this device on the tailnet")) {
        bool had_key = g_settings.ts_auth_key[0] != 0;
        assert(find_label(top, "Forget this device on the tailnet?", false));
        bool extra = find_label_has(top, "You'll need a new auth key.", 0) != NULL;
        if (extra == had_key) { printf("check %s: FAIL the no-key sentence %s\n", name, extra ? "shown with a key" : "missing"); *r = 1; return true; }
        *r |= pv_run_checks(d, s, name, false, 0);
        char key[sizeof(g_settings.ts_auth_key)];
        strcpy(key, g_settings.ts_auth_key);
        click_label(d, top, "Forget");
        step(d, 100);
        if (s_ts_forget_calls != 1) { printf("check %s: FAIL forget called %d times\n", name, s_ts_forget_calls); *r = 1; }
        if (strcmp(key, g_settings.ts_auth_key)) { printf("check %s: FAIL the forget cleared the auth key\n", name); *r = 1; }   /* U5 */
        assert(find_label_has(top, "Forgotten", 0));
        lv_obj_clean(top);
        printf("%s: forget ran once and kept the auth key (%s)\n", name, had_key ? "set" : "not set");
        return true;
    }
    if (!strcmp(click, "Connection")) {
        assert(sheet_rows("Connection") == 2 && find_label(top, "Try direct", false));
        *r |= pv_run_checks(d, s, name, false, 0);
        click_label(d, top, "Try direct");
        assert(g_settings.ts_direct == 1 && find_label(settings_panel(s), "Try direct", false));
        lv_obj_clean(top);
        return true;
    }
    if (!strcmp(click, "Clientre Wi-Fi")) {
        /* the current network: a sheet with Disconnect and Forget, both confirmed */
        assert(sheet_rows("Clientre Wi-Fi") == 2 && find_label(top, "Disconnect", false));
        *r |= pv_run_checks(d, s, name, false, 0);
        click_label(d, top, "Forget");
        assert(find_label(top, "Forget Clientre Wi-Fi?", false) && s_wifi_forget_calls == 0);
        click_label(d, top, "Forget");
        assert(s_wifi_forget_calls == 1 && s_preview_saved_n == 1);
        lv_obj_clean(top);
        return true;
    }
    printf("preview: no assertions for settings 1 with PREVIEW_CLICK=\"%s\"\n", click);
    return false;
}

static int timer_count(void) { int n = 0; for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) n++; return n; }

/* settings-sub networks|advanced|peers [deleted-event]: a pushed Network sub-page. */
static int settings_sub_case(lv_display_t *d, const char *sub, const char *variant) {
    ui_net_sub_t which = !strcmp(sub, "advanced") ? UI_NET_SUB_ADVANCED : !strcmp(sub, "peers") ? UI_NET_SUB_PEERS
                       : UI_NET_SUB_NETWORKS;
    char name[64];
    snprintf(name, sizeof(name), "settings-sub-%s%s%s%s", sub, variant[0] ? "-" : "", variant, pv_host() ? "" : "-no-host");
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_NETWORK); step(d, 200);
    int timers = timer_count();
    ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)which); step(d, 200);
    lv_obj_t *s = lv_screen_active(), *top = lv_layer_top();
    assert(ui_nav_is_top(ui_settings_sub_create));
    assert(find_clickable_shown(s, "\xe2\x86\x90 Settings"));
    if (timer_count() > timers) { printf("check %s: FAIL %d timers added\n", name, timer_count() - timers); return 1; }
    lv_obj_t *body = lv_obj_get_child(s, 1);
    int r = 0;
    if (which == UI_NET_SUB_NETWORKS) {
        assert(s_scan_starts == 1 && s_wifi_cb);
        assert(find_label(body, "Scanning\xe2\x80\xa6", false) && find_label(body, "Cabin", false));
        assert(find_label(body, "\xe2\x97\x8f connected \xc2\xb7 good", false));
        s_wifi_cb(WIFI_EV_SCAN_DONE, s_wifi_cb_user); step(d, 200);   /* the results, by event */
        assert(!find_label(body, "Scanning\xe2\x80\xa6", false));
        lv_obj_t *lib = find_label(body, "Library guest", false), *nb = find_label(body, "Neighbour 5G", false);
        assert(lib && nb);
        lv_obj_t *nbv = ui_srow_value(lv_obj_get_parent(nb)), *libv = ui_srow_value(lv_obj_get_parent(lib));
        if (!strstr(lv_label_get_text(nbv), UI_GLYPH_LOCK) || strstr(lv_label_get_text(libv), UI_GLYPH_LOCK)) {
            printf("check %s: FAIL the lock glyph (\"%s\", \"%s\")\n", name, lv_label_get_text(nbv), lv_label_get_text(libv)); return 1;
        }
        lv_font_glyph_dsc_t g;
        assert(lv_font_get_glyph_dsc(FONT_SMALL, &g, 0xF023, 0) && !g.is_placeholder);   /* the font draws it */
        int nbs = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) nbs += find_label(lv_obj_get_child(body, i), "Neighbour 5G", false) != NULL;
        assert(nbs == 1);   /* two access points, one row; the hidden network is left out */
        r |= pv_run_checks(d, s, name, false, 0);
        /* a secured network asks for its password, masked; an open one joins at once; a saved one uses its password */
        click_label(d, body, "Neighbour 5G");
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta && lv_textarea_get_password_mode(ta) && find_label(top, "Password for Neighbour 5G", false));
        prompt_answer(d, "preview-typed-pass");
        assert(s_wifi_connect_calls == 1 && !strcmp(s_wifi_connect_ssid, "Neighbour 5G"));
        click_label(d, body, "Library guest");
        assert(s_wifi_connect_calls == 2 && !find_shown(top, &lv_textarea_class));
        click_label(d, body, "Cabin");
        assert(s_wifi_connect_calls == 3 && !strcmp(s_wifi_connect_ssid, "Cabin"));
        click_label(d, body, "Clientre Wi-Fi");   /* the network it is on: no rejoin, so the Host link stays */
        assert(s_wifi_connect_calls == 3 && find_label(top, "Connected to Clientre Wi-Fi", false));
        lv_obj_clean(top);
        /* the trash button forgets, after a confirm */
        lv_obj_t *cab = find_label(body, "Cabin", false);
        lv_obj_t *trash = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(cab)), 1);
        lv_obj_send_event(trash, LV_EVENT_CLICKED, NULL); step(d, 200);
        assert(find_label(top, "Forget Cabin?", false) && s_wifi_forget_calls == 0);
        r |= pv_run_checks(d, s, "settings-sub-networks-forget", false, 0);
        click_label(d, top, "Forget");
        assert(s_wifi_forget_calls == 1 && !find_label(body, "Cabin", false));
        lv_obj_clean(top);
        /* Join other…: the name, then a masked password */
        click_label(d, body, "Join other\xe2\x80\xa6");
        prompt_answer(d, "Hidden lab");
        ta = find_shown(top, &lv_textarea_class);
        assert(ta && lv_textarea_get_password_mode(ta));
        prompt_answer(d, "");
        assert(s_wifi_connect_calls == 4 && !strcmp(s_wifi_connect_ssid, "Hidden lab"));
        lv_obj_clean(top);
        /* Back: the scan slot is cleared */
        ui_nav_back(); step(d, 200);
        assert(ui_nav_is_top(ui_settings_create));
        assert(s_wifi_cb != NULL);   /* Settings › Network's current-network row follows Wi-Fi again */
        lv_obj_send_event(rail_row(lv_screen_active(), UI_SET_HOST), LV_EVENT_CLICKED, NULL); step(d, 200);
        if (s_wifi_cb) { printf("check %s: FAIL the Wi-Fi slot is set after the Network panel went\n", name); r = 1; }
        printf("%s: scan by event, lock on secured, masked password, forget confirmed, slot cleared\n", name);
        return r;
    }
    if (which == UI_NET_SUB_ADVANCED) {
        assert(find_label(body, "Static address", false) && find_label(body, "Off (DHCP)", false));
        assert(find_label_has(body, "Applies from the next join.", 0));
        return pv_run_checks(d, s, name, false, 0);
    }
    /* Peers */
    assert(s_ts_cb != NULL && find_clickable_shown(s, "Refresh"));
    lv_obj_t *a = find_label(body, "preview-host-a", false), *b = find_label(body, "preview-host-b", false);
    assert(a && b && label_y(body, "preview-host-c") < label_y(body, "preview-host-b"));   /* online first */
    assert(find_label(body, "100.64.0.12 \xc2\xb7 offline \xc2\xb7 relay", false));
    assert(find_label(body, "100.64.0.13 \xc2\xb7 online \xc2\xb7 direct", false));
    int reads = s_peer_reads;
    click_label(d, s, "Refresh");
    assert(s_peer_reads > reads);
    reads = s_peer_reads;
    s_ts_cb(s_ts_cb_user); s_ts_cb(s_ts_cb_user); step(d, 200);   /* two events: one re-read, on the LVGL task */
    assert(s_peer_reads == reads + 1);
    if (!strcmp(variant, "deleted-event")) {
        ts_event_cb_t old = s_ts_cb; void *old_user = s_ts_cb_user;
        ui_nav_back(); step(d, 200);
        lv_obj_t *now = lv_screen_active();
        if (s_ts_cb != NULL) { printf("check %s: FAIL the Tailscale slot is still set after Peers went\n", name); return 1; }
        reads = s_peer_reads;
        old(old_user); step(d, 200);   /* a late event from MicroLink's task: nothing happens */
        if (lv_screen_active() != now || s_peer_reads != reads) { printf("check %s: FAIL a stale event acted\n", name); return 1; }
        if (s_ts_cb) s_ts_cb(s_ts_cb_user);   /* the stub's saved callback: NULL, nothing to call */
        assert(ui_nav_is_top(ui_settings_create));
        printf("%s: slot NULL after back; a stale event did nothing\n", name);
        return pv_run_checks(d, now, name, false, 0);
    }
    r |= pv_run_checks(d, s, name, false, 0);
    click_label(d, body, "preview-host-a");
    assert(sheet_rows("preview-host-a") == 1 && find_label(top, "Add as SSH host", false));
    r |= pv_run_checks(d, s, "settings-sub-peers-sheet", false, 0);
    click_label(d, top, "Add as SSH host");
    assert(find_label(top, "Add it from Terminal", false));
    lv_obj_clean(top);
    return r;
}

static int settings_case(lv_display_t *d, int section) {
    assert(section >= 0 && section < UI_SET_COUNT);
    const char *click = env_str("PREVIEW_CLICK", "");
    int scroll = env_int("PREVIEW_SCROLL", 0);
    char name[64], sl[32];
    slug(sl, sizeof(sl), click);
    if (click[0]) snprintf(name, sizeof(name), "settings-%d-%s", section, sl);
    else if (!pv_host()) snprintf(name, sizeof(name), "settings-%d-no-host", section);
    else if (pv_notfound()) snprintf(name, sizeof(name), "settings-%d-not-found", section);
    else if (scroll) snprintf(name, sizeof(name), "settings-%d-scroll-%d", section, scroll);
    else snprintf(name, sizeof(name), "settings-%d", section);
    if (env_int("PREVIEW_TS", 2) != 2) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-ts%d", env_int("PREVIEW_TS", 2));
    if (!env_int("PREVIEW_KEY", 1)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-nokey");
    preview_sessions_load();
    ui_nav_home(); step(d,200);
    ui_nav_push(ui_settings_create, NULL); step(d,200);
    lv_obj_t *s = lv_screen_active();
    assert(ui_nav_is_top(ui_settings_create));
    /* Real click events through every rail row: one panel, refilled; the same screen object (B2) */
    for (int i = 0; i < UI_SET_COUNT; i++) {
        lv_obj_t *r = rail_row(s, i);
        assert(r);
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); step(d,200);
        assert(lv_screen_active() == s);
        lv_obj_t *body = lv_obj_get_child(s, 1);
        assert(lv_obj_get_child_count(body) == 2);   /* the rail and the one panel: nothing hidden behind it */
        lv_obj_t *title = lv_obj_get_child(settings_panel(s), 0);
        assert(title && !strcmp(lv_label_get_text(title), k_rail_names[i]));
        assert(row_selected(r));
        for (int j = 0; j < UI_SET_COUNT; j++) if (j != i) assert(!row_selected(rail_row(s, j)));
    }
    lv_obj_send_event(rail_row(s, section), LV_EVENT_CLICKED, NULL); step(d,200);
    assert(lv_screen_active() == s);
    lv_obj_t *panel = settings_panel(s);
    if (scroll) {
        lv_obj_update_layout(s);
        lv_obj_scroll_by(panel, 0, -scroll, LV_ANIM_OFF);
        lv_obj_update_layout(s); step(d,100);
    }
    if (click[0]) {
        lv_obj_t *c = find_clickable_shown(panel, click);
        if (!c) { printf("check %s: FAIL no row \"%s\"\n", name, click); return 1; }
        lv_obj_scroll_to_view(c, LV_ANIM_OFF);
        lv_obj_send_event(c, LV_EVENT_CLICKED, NULL); step(d,200);
    }
    lv_obj_t *top = lv_layer_top();
    int r = 0;
    if (section == UI_SET_HOST && !strcmp(click, "Public key")) {
        lv_obj_t *k = find_label(top, "ssh-ed25519 ", true);
        assert(k && !strcmp(lv_label_get_text(k), PREVIEW_PUBKEY));   /* the whole line, wrapped */
        assert(lv_obj_get_style_text_font(k, 0) == FONT_MONO);
        assert(find_label(top, "Add this line to ~/.ssh/authorized_keys on each host this Tab5 signs in to.", false));
    } else if (section == UI_SET_HOST && !strcmp(click, "Forget")) {
        assert(find_label(top, "Forget this host's key?", false));
        assert(find_label(top, "Only do this if the host's SSH key changed. The Tab5 will trust the next key it sees.", false));
        r |= pv_run_checks(d, s, name, false, 0);   /* the confirm: wrapped title, fit, every button's 80 px hit */
        lv_obj_t *ok = find_clickable_shown(top, "Forget");
        assert(ok && s_forget_calls == 0);
        lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL); step(d,200);
        if (s_forget_calls != 1) { printf("check %s: FAIL forget called %d times\n", name, s_forget_calls); r = 1; }
        lv_obj_clean(top);
        return r;
    } else if (section == UI_SET_ABOUT && !strcmp(click, "Reset settings")) {
        assert(find_label(top, "Reset settings?", false));
        assert(find_label_has(top, "keeps the time zone", 0));
        r |= pv_run_checks(d, s, name, false, 0);
        lv_obj_t *ok = find_clickable_shown(top, "Reset");
        assert(ok);
        lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL); step(d,200);
        if (s_reset_user_calls != 1) { printf("check %s: FAIL reset called %d times\n", name, s_reset_user_calls); r = 1; }
        assert(lv_screen_active() == s);
        lv_obj_clean(top);
        return r;
    } else if (section == UI_SET_ABOUT && !strcmp(click, "Diagnostics")) {
        if (timers_with_period(2000) != 1) { printf("check %s: FAIL %d 2 s timers expanded\n", name, timers_with_period(2000)); return 1; }
        assert(find_label_has(panel, "Last reset: power-on", 0));
        r |= pv_run_checks(d, s, name, false, 0);   /* idle: a refresh with nothing changed redraws nothing */
        step(d, 2100);
        lv_obj_send_event(find_clickable_shown(panel, "Diagnostics"), LV_EVENT_CLICKED, NULL); step(d,200);
        if (timers_with_period(2000) != 0) { printf("check %s: FAIL a 2 s timer after collapsing\n", name); r = 1; }
        assert(!find_label_has(panel, "Last reset", 0));
        /* expanded again, then another section: the timer goes with the panel */
        lv_obj_send_event(find_clickable_shown(panel, "Diagnostics"), LV_EVENT_CLICKED, NULL); step(d,200);
        assert(timers_with_period(2000) == 1);
        lv_obj_send_event(rail_row(s, UI_SET_HOST), LV_EVENT_CLICKED, NULL); step(d,200);
        if (timers_with_period(2000) != 0) { printf("check %s: FAIL a 2 s timer after leaving About\n", name); r = 1; }
        /* B1 (review M4): Esc out of Settings while expanded: the timer goes with the screen */
        lv_obj_send_event(rail_row(s, UI_SET_ABOUT), LV_EVENT_CLICKED, NULL); step(d,200);
        lv_obj_send_event(find_clickable_shown(panel, "Diagnostics"), LV_EVENT_CLICKED, NULL); step(d,200);
        assert(timers_with_period(2000) == 1);
        press_key(SK_ESC, 0, false); step(d,200);
        assert(ui_nav_is_top(ui_sessions_create));
        if (timers_with_period(2000) != 0) { printf("check %s: FAIL a 2 s timer after Esc from Settings\n", name); r = 1; }
        return r;
    } else if (section == UI_SET_ABOUT && !strcmp(click, "Reboot")) {
        /* the short-label confirm: its end button keeps an 80 px hit area (pv_run_checks below) */
        assert(find_label(top, "Reboot now?", false) && find_clickable_shown(top, "Reboot"));
    } else if (section == UI_SET_KBD && !strcmp(click, "Colour")) {
        lv_obj_t *t = find_label(top, "Colour", false);
        assert(t);
        lv_obj_t *sheet = lv_obj_get_parent(t);
        int rows = (int)lv_obj_get_child_count(sheet) - 1;
        if (rows != 7) { printf("check %s: FAIL %d colour rows\n", name, rows); return 1; }
        assert(find_label(sheet, "Keyboard's own", false) && find_label(sheet, "Cyan", false));
        assert(!find_label(sheet, "#", true));   /* drawn, never hex */
        int amber = pv_amber_pixels(s);
        if (amber) { printf("check %s: FAIL %d amber pixels in the colour picker\n", name, amber); return 1; }
    } else if (section == UI_SET_POWER && scroll) {
        lv_obj_t *h = find_label(panel, "Battery saver", false);
        assert(h);
        lv_area_t a, p; lv_obj_get_coords(h, &a); lv_obj_get_coords(panel, &p);
        if (a.y1 < p.y1 || a.y2 > p.y2) { printf("check %s: FAIL Battery saver at %d-%d, the panel %d-%d\n", name, (int)a.y1, (int)a.y2, (int)p.y1, (int)p.y2); return 1; }
    } else if (section == UI_SET_HOST && !click[0] && (!pv_host() || pv_notfound())) {
        /* U11 and the frozen countdown (T7 review I2): the Link row says Host, never Mac, and no "(retry in Ns)" */
        const char *want = !pv_host() ? "No Host set up" : "Host not found at my-host.local";
        if (!find_label(panel, want, false)) { printf("check %s: FAIL no Link text \"%s\"\n", name, want); return 1; }
        if (find_label_has(panel, "Mac", 0) || find_label_has(panel, "retry in", 0)) {
            printf("check %s: FAIL the Link row says \"%s\"\n", name,
                   lv_label_get_text(find_label_has(panel, "Mac", 0) ? find_label_has(panel, "Mac", 0) : find_label_has(panel, "retry in", 0)));
            return 1;
        }
    } else if (section == UI_SET_HOST && !click[0]) {
        assert(!find_label(s, "(macOS)", true));
        assert(find_label(panel, "\xe2\x97\x8f connected over home Wi-Fi", false));
        assert(find_label(panel, "ssh-ed25519 \xe2\x80\xa6 claude-companion \xc2\xb7 Show", false));
        for (lv_obj_t *o = rail_row(s, 0); o; o = NULL) assert(lv_obj_get_height(o) == UI_TK_RAIL_ROW_H);
        lv_obj_t *rail = lv_obj_get_child(lv_obj_get_child(s, 1), 0);
        assert(lv_obj_get_width(rail) == UI_TK_RAIL_W);
    } else if (section == UI_SET_NETWORK) {
        if (network_case(d, s, panel, click, scroll, name, &r)) { lv_obj_clean(top); return r; }
    } else if (click[0] || scroll) {
        printf("preview: no assertions for settings %d with PREVIEW_CLICK=\"%s\" PREVIEW_SCROLL=%d\n", section, click, scroll);
    }
    /* Task 8: Network is rebuilt, so every section runs every check (C14 ends) */
    r |= pv_run_checks(d, s, name, false, 0);
    lv_obj_clean(top);
    return r;
}

/* No sink on Settings: the arrows move the focus through ui_keyboard_route's no-sink path (no Ctrl+Tab); the
   footer hint shows while a key was used last; Esc goes back to Sessions. */
static int keyboard_settings_case(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d,200);
    ui_settings_open(UI_SET_HOST); step(d,200);
    lv_obj_t *s = lv_screen_active();
    assert(ui_nav_is_top(ui_settings_create) && !preview_have_sink);
    lv_obj_t *r0 = rail_row(s, 0), *r1 = rail_row(s, 1);
    int n = 0;
    while (!lv_obj_has_state(r0, LV_STATE_FOCUS_KEY) && n < 4) { press_key(SK_DOWN, 0, false); step(d,50); n++; }
    if (!lv_obj_has_state(r0, LV_STATE_FOCUS_KEY)) { printf("check keyboard-settings: FAIL rail row 0 never focused\n"); return 1; }
    lv_obj_t *hint = find_label(s, "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 Enter choose \xc2\xb7 Esc back", false);
    assert(hint && !lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN));
    press_key(SK_DOWN, 0, false); step(d,50);
    if (!lv_obj_has_state(r1, LV_STATE_FOCUS_KEY)) { printf("check keyboard-settings: FAIL Down did not reach rail row 1\n"); return 1; }
    press_key(SK_ENTER, 0, false); step(d,100);
    assert(lv_screen_active() == s && !strcmp(lv_label_get_text(lv_obj_get_child(settings_panel(s), 0)), "Network"));
    assert(lv_obj_has_state(r1, LV_STATE_FOCUS_KEY));   /* the focus stays on the rail */
    int r = pv_run_checks(d, s, "keyboard-settings", false, 0);   /* Network, rebuilt in Task 8: no skips */
    press_key(SK_ESC, 0, false); step(d,200);
    if (!ui_nav_is_top(ui_sessions_create)) { printf("check keyboard-settings: FAIL Esc did not go back\n"); return 1; }
    /* Ctrl+P again lands on the last used section, Network */
    ui_nav_push(ui_settings_create, NULL); step(d,200);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(settings_panel(lv_screen_active()), 0)), "Network"));
    printf("keyboard-settings: rail row 0 after %d Down, then Down to row 1\n", n);
    return r;
}

int main(int argc,char **argv) {
    lv_init();
    int w=1280, h=720;
    lv_display_t *d=lv_display_create(w,h);
    void *buf=malloc(w*50*4);
    lv_display_set_buffers(d,buf,NULL,w*50*4,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    g_settings.brightness=58;
    g_settings.term_line_height=32;
    g_settings.kbd_dark_light=5;   /* PT24 default: "Low" */
    /* the Plan 2 power defaults (settings_power_defaults), so Settings › Power shows real values */
    g_settings.pw_dim_s=30; g_settings.pw_off_s=120; g_settings.pw_sleep_on_usb=1; g_settings.pw_wake_needs_you=1;
    g_settings.kbd_needs_light=1; g_settings.pw_radio_off_min=20; g_settings.pw_checkin_min=5;
    g_settings.pw_deep_idle_min=60; g_settings.pw_hibernate_h=2; g_settings.pw_hibernate_checkin_min=15;
    g_settings.ss_disable_in_ssh=1; g_settings.kbd_led_bright=20;
    g_settings.kbd_led_rgb[0]=0x00; g_settings.kbd_led_rgb[1]=0x80; g_settings.kbd_led_rgb[2]=0xFF;
    strcpy(g_settings.tz_rule, "PST8PDT,M3.2.0,M11.1.0");
    const char *name = argc>1 ? argv[1] : "";
    /* Exercise splash before font runtime copies or the application UI exist. */
    if (!strcmp(name,"boot")) {
        lv_font_glyph_dsc_t g;
        assert(lv_font_get_glyph_dsc(&font_px20,&g,0x2026,0) && !g.is_placeholder);   /* "Starting…" */
        ui_boot_show();
        lv_obj_t *boot=lv_screen_active();
        assert(lv_obj_get_child_count(boot)==1);
        flushed_pixels=0; step(d,1000);
        assert(flushed_pixels==0); /* static logo must not schedule redraws */
        int result=pv_run_checks(d,boot,"boot",false,0);
        lv_obj_add_event_cb(boot,boot_deleted_cb,LV_EVENT_DELETE,NULL);
        fonts_init(); ui_init(); ui_nav_home(); step(d,400);
        assert(s_boot_deleted && ui_nav_is_top(ui_sessions_create));   /* P4-2: the splash is deleted */
        puts("Early boot logo and handoff checks passed.");
        return result;
    }
    fonts_init(); ui_init();
    g_settings.ts_enable = env_int("PREVIEW_TS", 2) != 0;
    if (env_int("PREVIEW_KEY", 1)) strcpy(g_settings.ts_auth_key, PREVIEW_TS_KEY);
    if (!strcmp(name,"sleep")) {
        /* the pre-Hibernate notice: full screen, over whatever is shown */
        lv_obj_t *s=ui_screen_base(); lv_screen_load(s);
        ui_sleep_notice(true); step(d,400);
        return pv_run_checks(d,s,"sleep",false,0);
    }
    if (!strcmp(name,"nav")) return nav_case(d);
    if (!strcmp(name,"targets-clip")) return targets_clip_case(d);
    if (!strcmp(name,"softkbd")) {
        /* the soft keyboard (a fallback with no physical keyboard): touch-only, so the touch rules apply */
        lv_obj_t *s=ui_screen_base(); lv_screen_load(s);
        softkbd_create(s); step(d,100);
        return pv_run_checks(d,s,"softkbd",false,0);
    }
    if (!strcmp(name,"gesture")) { gesture_check(); return 0; }
    if (!strcmp(name,"art")) { terminal_art_check(); return 0; }
    if (!strcmp(name,"terminal")) { terminal_spacing_check(); return 0; }
    if (!strcmp(name,"keyboard")) { keyboard_check(); return 0; }
    if (!strcmp(name,"glyphs")) {
        /* Which of Claude Code's interface symbols the terminal font draws. */
        static const struct { uint32_t cp; const char *name; } g[] = {
            {0x2500,"box \xe2\x94\x80"},{0x256D,"box \xe2\x95\xad"},{0x273B,"\xe2\x9c\xbb"},{0x23FA,"\xe2\x8f\xba"},
            {0x23BF,"\xe2\x8e\xbf"},{0x2713,"\xe2\x9c\x93"},{0x25CF,"\xe2\x97\x8f"},{0x276F,"\xe2\x9d\xaf"},
            {0x2026,"\xe2\x80\xa6"},{0x00B7,"\xc2\xb7"},{0x2014,"\xe2\x80\x94"},{0x2192,"\xe2\x86\x92"},
            {0x2190,"\xe2\x86\x90"},{0x2191,"\xe2\x86\x91"},{0x2193,"\xe2\x86\x93"},{0x21B5,"\xe2\x86\xb5"},
            {0x2303,"\xe2\x8c\x83"},{0x203A,"\xe2\x80\xba"},
            {0x2722,"\xe2\x9c\xa2"},{0x2733,"\xe2\x9c\xb3"},{0x2736,"\xe2\x9c\xb6"},{0x273D,"\xe2\x9c\xbd"},{0x00B7,"spinner dot"} };
        const lv_font_t *fonts[] = { &g_term_font, FONT_BODY, FONT_SMALL, FONT_TITLE };
        const char *fn[] = { "terminal", "body", "small", "title" };
        for (unsigned f=0; f<4; f++) for (unsigned i=0;i<sizeof(g)/sizeof(g[0]);i++) {
            lv_font_glyph_dsc_t dsc;
            bool ok = lv_font_get_glyph_dsc(fonts[f],&dsc,g[i].cp,0) && !dsc.is_placeholder;
            printf("%-8s U+%04X %-12s %s\n", fn[f], (unsigned)g[i].cp, g[i].name, ok ? "yes" : "MISSING");
        }
        return 0;
    }
    if (!strcmp(name,"sessions")) return sessions_case(d, argc>2 ? argv[2] : "live");
    if (!strcmp(name,"ctrlk")) return sessions_case(d, "ctrlk");
    if (!strcmp(name,"settings")) return settings_case(d, argc>2 ? atoi(argv[2]) : 0);
    if (!strcmp(name,"keyboard-settings")) return keyboard_settings_case(d);
    if (!strcmp(name,"settings-sub")) return settings_sub_case(d, argc>2 ? argv[2] : "networks", argc>3 ? argv[3] : "");
    fprintf(stderr, "preview: unknown case %s\n", argc>1 ? argv[1] : "(none)");
    return 2;
}
