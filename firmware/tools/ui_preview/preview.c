#include "ui/ui.h"
#include "term/term_view.h"
#include "ui/ui_boot.h"
#include "ui/ui_comp.h"
#include "ui/ui_sessions.h"
#include "ui/ui_settings.h"
#include "ssh/device_key.h"
#include "ssh/ssh_client.h"
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
#include "servers/host_rules.h"
#include "companion/cc_cmd.h"
#include "ui/ui_host_form.h"
#include "i18n/i18n.h"
#include "net/ts_host.h"
#include "sys/power.h"
#include "sys/checkin.h"
#include "voice/voice.h"
#include "ota/fw_update.h"
#include "ui/ui_fwupdate.h"
#include "ui/sessions_model.h"
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
/* PREVIEW_WIFI=0: no Wi-Fi. PREVIEW_HOST=0: no Host set. PREVIEW_FAIL=notfound: the Host's name doesn't resolve.
   PREVIEW_FAIL=newkey: first run, the Host presented a key nothing pins (until Trust this key pins it). */
static bool pv_wifi(void) { return env_int("PREVIEW_WIFI", 1) != 0 && !s_rc_preview; }
static bool pv_host(void) { return env_int("PREVIEW_HOST", 1) != 0; }
static bool pv_notfound(void) { return !strcmp(env_str("PREVIEW_FAIL", ""), "notfound"); }
static bool s_pv_trusted;   /* newkey: Trust this key ran */
static bool pv_newkey(void) { return !strcmp(env_str("PREVIEW_FAIL", ""), "newkey") && !s_pv_trusted; }
bool wifi_mgr_is_connected(void) { return pv_wifi(); }
bool wifi_mgr_is_connecting(void) { return s_rc_preview; }
void wifi_mgr_get_ip4(char *b, size_t n) { snprintf(b,n,"192.168.1.25"); }
static int s_wifi_disconnect_calls;
void wifi_mgr_disconnect(void) { s_wifi_disconnect_calls++; }
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
static int s_note_activity_calls;
void power_note_activity(void) { s_note_activity_calls++; }
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
int pv_lvgl_lock_fails;   /* stubs/esp_lvgl_port.h: the next N LVGL lock takes time out */
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
static bool s_peer_a_offline;   /* terminal on: preview-host-a drops off */
bool tailscale_peer(int i, ts_peer_t *out) {
    if (tailscale_state() != TS_CONNECTED || i < 0 || i >= 3) return false;
    *out = k_preview_peers[i];
    if (i == 0 && s_peer_a_offline) out->online = false;
    if (i == 2 && env_int("PREVIEW_HOST_TSIP", 1) == 0) snprintf(out->hostname, sizeof(out->hostname), "studio-host.tail1234.ts.net");
    return true;
}
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
void sd_unmount(void) {}
bool fs_read_all(const char *path, char **out, size_t *len, size_t max) {
    (void)max;
    if (strcmp(path, "/sdcard/tailscale/authkey.txt") || !strcmp(env_str("PREVIEW_SD", "1"), "nofile")) return false;
    const char *v = env_str("PREVIEW_SD", "1");
    char longkey[400];
    memset(longkey, 'k', sizeof(longkey) - 1); memcpy(longkey, "tskey-auth-", 11); longkey[sizeof(longkey) - 1] = 0;
    *out = strdup(!strcmp(v, "bad") ? "not a key at all\n" : !strcmp(v, "long") ? longkey : "tskey-auth-fromsd-NOTAREALKEY\nsecond line\n");
    *len = strlen(*out);
    return true;
}
bool keyboard_any_present(void) { return env_int("PREVIEW_KBD", 1) != 0; }   /* PREVIEW_KBD=0: touch only */
void keyboard_inject(const key_event_t *ev) { (void)ev; }
void esp_restart(void) {}
void bsp_display_brightness_set(int v) {}
/* The server store: the Host's entry (index 0, Settings › Host), a LAN host and a tailnet host (preview-host-a's
   address, online). PREVIEW_HOSTS=empty: the Host's entry only. PREVIEW_HOSTS=sd: the LAN host signs in with a key
   file on the SD card. The add / update / remove calls are recorded. */
static server_t s_preview_servers[SERVER_MAX] = {
    { .name = "Desk", .host = "studio-host.local", .port = 22, .user = "me", .use_key = true, .keypath = "device" },
    { .name = "pi-lan", .host = "192.168.1.50", .port = 22, .user = "pi", .use_key = true, .keypath = "device" },
    { .name = "build-box", .host = "100.64.0.11", .port = 22, .user = "dev", .use_key = true, .keypath = "device" },
};
static int s_preview_server_n = -1;
static int s_store_adds, s_store_updates, s_store_removes;
static void preview_store_init(void) {
    if (s_preview_server_n >= 0) return;
    s_preview_server_n = !strcmp(env_str("PREVIEW_HOSTS", ""), "empty") ? 1 : 3;
    if (!strcmp(env_str("PREVIEW_HOSTS", ""), "sd")) snprintf(s_preview_servers[1].keypath, sizeof(s_preview_servers[1].keypath), "/sdcard/ssh/id_ed25519");
}
int server_store_count(void) { preview_store_init(); return s_preview_server_n; }
const server_t *server_store_get(int i) { preview_store_init(); return i >= 0 && i < s_preview_server_n ? &s_preview_servers[i] : NULL; }
int server_store_add(const server_t *s) {
    preview_store_init(); s_store_adds++;
    if (s_preview_server_n >= SERVER_MAX) return -1;
    s_preview_servers[s_preview_server_n] = *s;
    return s_preview_server_n++;
}
bool server_store_update(int i, const server_t *s) {
    preview_store_init(); s_store_updates++;
    if (i < 0 || i >= s_preview_server_n) return false;
    s_preview_servers[i] = *s; return true;
}
bool server_store_remove(int i) {
    preview_store_init(); s_store_removes++;
    if (i < 0 || i >= s_preview_server_n) return false;
    memmove(&s_preview_servers[i], &s_preview_servers[i + 1], sizeof(server_t) * (size_t)(s_preview_server_n - i - 1));
    s_preview_server_n--; return true;
}
/* The link's known Host addresses (invented): the entry's host, the LAN fallback and the tailnet carry address,
   preview-host-c's IP (so the tailnet sheet must drop that peer). As cc_link.c decides it: index 0 or any address. */
#define PREVIEW_HOST_TS_IP "100.64.0.13"
/* PREVIEW_HOST_TSIP=0: the link never met the Host over the tailnet (no tailnet IP known), and preview-host-c's
   MagicDNS name is the Host's: studio-host.tail1234.ts.net (C1 by the first label). */
static bool pv_host_tsip(void) { return env_int("PREVIEW_HOST_TSIP", 1) != 0; }
void cc_link_host_addresses(hf_host_addrs_t *h) {
    hf_host_addrs_init(h);
    if (!pv_host()) return;
    hf_host_addrs_add_name(h, s_preview_servers[0].host);
    hf_host_addrs_add(h, "192.168.77.20");
    if (pv_host_tsip()) hf_host_addrs_add(h, PREVIEW_HOST_TS_IP);
}
bool cc_link_is_companion_server(int idx) {
    const server_t *s = server_store_get(idx);
    if (!s || !pv_host()) return false;
    if (idx == 0) return true;
    hf_host_addrs_t h; cc_link_host_addresses(&h);
    return hf_is_host(s->host, &h);
}
static int s_rename_calls; static char s_rename_old[48], s_rename_new[48];
bool cc_link_rename_server(const char *o, const char *n) {
    s_rename_calls++;
    snprintf(s_rename_old, sizeof(s_rename_old), "%s", o); snprintf(s_rename_new, sizeof(s_rename_new), "%s", n);
    return true;
}
/* known hosts: build-box (100.64.0.11:22) has a pin until it is forgotten */
#define PREVIEW_HOST_FP "SHA256:BuildBoxPreviewPinOnly0123456789abcdefghijk"
static bool s_kh_pinned = true;
static int s_kh_forget_calls;
bool ssh_known_host_fingerprint(const char *host, int port, char *fp, size_t cap) {
    if (cap) fp[0] = 0;
    if (!s_kh_pinned || strcmp(host, "100.64.0.11") || port != 22) return false;
    snprintf(fp, cap, PREVIEW_HOST_FP); return true;
}
bool ssh_known_host_forget(const char *host, int port) {
    s_kh_forget_calls++;
    bool had = s_kh_pinned && !strcmp(host, "100.64.0.11") && port == 22;
    s_kh_pinned = false; return had;
}
bool tailscale_resolve(const char *host, char *ip, size_t cap) { (void)host; if (cap) ip[0] = 0; return false; }
/* The SSH client under the real ui_terminal.c (Task 10). Each start is recorded. As ssh_task, an interactive session
   whose server presents a key the link pinned for the Host is refused before it authenticates (P4C-10): "alias-box"
   (10.20.30.40, an address the link doesn't know) is the Host reached another way and presents the Host's key. Any
   other start connects at once, except that with s_ssh_autoclose (the Sessions cases) a take-over ends at once, as
   the old terminal stub's did. Bytes sent are kept in s_ssh_sent. */
struct ssh_client { ssh_data_cb_t data; ssh_state_cb_t state; ssh_hostkey_cb_t hk; void *user; };
static int s_term_opens, s_term_idx = -1, s_ssh_stops;
static bool s_ssh_autoclose = true;
static char s_ssh_sent[256]; static size_t s_ssh_sent_n;
static ssh_client_t *s_ssh_live;
static const uint8_t k_pv_host_key[32] = { 0xAB, 0xAB, 0xAB, 0xAB, 1, 2, 3, 4 };
ssh_client_t *ssh_client_start(const ssh_params_t *p, ssh_data_cb_t data, ssh_state_cb_t state, ssh_hostkey_cb_t hk, void *user) {
    ssh_client_t *c = calloc(1, sizeof(*c));
    c->data = data; c->state = state; c->hk = hk; c->user = user;
    s_ssh_live = c;
    /* "newkey-box" (192.168.1.61) has no pin: ssh_task asks through the host-key callback. The preview is one thread
       and its semaphores never block, so the callback returns before anyone answers (as a cancelled request). */
    if (!strcmp(p->host, "192.168.1.61") && hk && !hk(p->host, p->port, "PreviewNewKeyOnly0123456789", false, user)) {
        state(SSH_ST_ERROR, "Host key not trusted", user);
        return c;
    }
    if (!p->cmd[0]) {
        s_term_opens++; s_term_idx = -1;
        for (int i = 0; i < server_store_count(); i++) {
            const server_t *e = server_store_get(i);
            if (e && !strcmp(e->host, p->host) && e->port == p->port) { s_term_idx = i; break; }
        }
        uint8_t keys[HF_HOSTKEYS_MAX][32], met[32] = { 0x11, 0x22 };
        int n = hf_hostkeys_add(keys, 0, k_pv_host_key);
        if (!strcmp(p->host, "10.20.30.40")) memcpy(met, k_pv_host_key, 32);
        hf_gate_t g = hf_shell_gate(true, HF_DENY_FOUND, met, (const uint8_t (*)[32])keys, n);   /* ssh_task's first step */
        if (g == HF_GATE_REFUSE_HOST) { state(SSH_ST_ERROR, tr(STR_HOSTS_NOT_LISTED), user); return c; }
    }
    state(SSH_ST_CONNECTING, NULL, user);
    state(p->cmd[0] && s_ssh_autoclose ? SSH_ST_CLOSED : SSH_ST_CONNECTED, NULL, user);
    return c;
}
void ssh_client_send(ssh_client_t *c, const uint8_t *d, size_t n) {
    (void)c;
    if (s_ssh_sent_n + n < sizeof(s_ssh_sent)) { memcpy(s_ssh_sent + s_ssh_sent_n, d, n); s_ssh_sent_n += n; s_ssh_sent[s_ssh_sent_n] = 0; }
}
void ssh_client_resize(ssh_client_t *c, int cols, int rows) { (void)c; (void)cols; (void)rows; }
/* I1: the real stop waits for a callback in flight on ssh_task. Model one of each finishing inside that wait: the
   screen's context must still be whole here (ASan catches a free before the stop), and a host-key question asked
   now must see the screen closing and answer "no". */
bool ssh_client_stop(ssh_client_t *c) {
    s_ssh_stops++;
    c->data((const uint8_t *)"~", 1, c->user);
    c->state(SSH_ST_CLOSED, NULL, c->user);
    if (c->hk && c->hk("192.168.1.61", 22, "PreviewInFlight", false, c->user)) { printf("check: FAIL a host-key question in flight during the stop was trusted\n"); exit(1); }
    if (s_ssh_live == c) s_ssh_live = NULL;
    free(c);
    return true;
}
int ssh_client_exit_status(ssh_client_t *c) { (void)c; return 0; }
/* The remote side writes (the pump draws it on its next run). */
static void pv_ssh_feed(const char *t) { assert(s_ssh_live); s_ssh_live->data((const uint8_t *)t, strlen(t), s_ssh_live->user); }
static void pv_ssh_state(ssh_state_t st, const char *msg) { assert(s_ssh_live); s_ssh_live->state(st, msg, s_ssh_live->user); }
/* The address the link verified the Host on (the take-over connects there, L25). */
bool cc_link_verified_address(char *out, size_t cap) { snprintf(out, cap, "192.168.77.20"); return true; }
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
/* A poll where session i is gone (it ended and left the list), or i stopped (no pid any more). */
static void preview_drop(int i) {
    assert(i >= 0 && i < s_preview_n);
    memmove(&s_preview_sessions[i], &s_preview_sessions[i + 1], sizeof(cc_session_t) * (size_t)(s_preview_n - i - 1));
    s_preview_n--;
    s_poll_gen++;
    if (s_poll_cb) s_poll_cb(s_preview_sessions, s_preview_n, s_poll_ctx);
}
static void preview_poll(void) {
    s_poll_gen++;
    if (s_poll_cb) s_poll_cb(s_preview_sessions, s_preview_n, s_poll_ctx);
}
static void preview_stopped(int i) {
    assert(i >= 0 && i < s_preview_n);
    s_preview_sessions[i].live = false;
    s_preview_sessions[i].state = CC_ENDED;
    s_poll_gen++;
    if (s_poll_cb) s_poll_cb(s_preview_sessions, s_preview_n, s_poll_ctx);
}
int cc_poll_snapshot(cc_session_t *out, int cap) {
    if (s_preview_n < 0) return -1;
    int n = s_preview_n < cap ? s_preview_n : cap;
    if (n > 0) memcpy(out, s_preview_sessions, sizeof(cc_session_t) * n);
    return n;
}
bool cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx) { s_poll_cb = cb; s_poll_ctx = ctx; return true; }
void cc_poll_set_rate(int s) { (void)s; }
void cc_poll_set_floor(int s) { (void)s; }
uint32_t cc_poll_generation(void) { return s_poll_gen; }
int64_t cc_poll_last_ok_ms(void) { return 1; }
bool cc_link_busy(void) { return false; }
void cc_link_watch_enable(bool on) { (void)on; }
void cc_link_watch_test_fallback(void) {}
const char *cc_watch_status_text(char *b, size_t cap) { snprintf(b, cap, "watch: on I=3s, last block 4s ago, heartbeat 12s ago"); return b; }
bool cc_link_connected(void) { return pv_wifi() && pv_host() && !pv_notfound() && !pv_newkey(); }
#define PREVIEW_NEW_FP "SHA256:NewPreviewHostKeyOnly0123456789abcdefghijk"
/* the device's own wording (cc_link.c), so the banner's cut is tested */
const char *cc_link_status_text(char *b, size_t cap) {
    if (!pv_host()) snprintf(b, cap, "no Host set up");
    else if (pv_notfound()) snprintf(b, cap, "Host not found at studio-host.local (set its fallback address in Settings \xe2\x80\xba Host) (retry in 12s)");
    else if (pv_newkey()) snprintf(b, cap, "new host key %s for 192.168.77.20; check and trust it in Settings \xe2\x80\xba Host (retry in 30s)", PREVIEW_NEW_FP);
    else snprintf(b, cap, cc_link_connected() ? "connected studio-host" : "connecting to studio-host");
    return b;
}
cc_link_fail_t cc_link_failure(void) { return pv_notfound() ? CC_LINK_FAIL_NOT_FOUND : pv_newkey() ? CC_LINK_FAIL_HOSTKEY : CC_LINK_FAIL_NONE; }
bool cc_link_mac_address(char *o, size_t cap) { snprintf(o, cap, "100.101.102.103"); return true; }
bool cc_link_mac_path(char *o, size_t cap) { snprintf(o, cap, "lan 192.168.77.20"); return true; }
bool cc_link_set_lan_fallback(const char *ip) { (void)ip; return true; }
const char *cc_link_lan_fallback(char *o, size_t cap) { snprintf(o, cap, "192.168.77.20"); return o; }
int cc_poll_failures(bool *too_big) { if (too_big) *too_big = false; return 0; }
const char *cc_settings_claude_path(char *o, size_t cap) { snprintf(o, cap, "~/.local/bin/claude"); return o; }
static const char *s_pv_server_name;   /* picker-fresh: a name no entry has (set from the console only) */
const char *cc_settings_server_name(char *o, size_t cap) { snprintf(o, cap, "%s", s_pv_server_name ? s_pv_server_name : pv_host() ? "studio-host" : ""); return o; }
int cc_settings_server(void) { return pv_host() ? 0 : -1; }
static int s_set_server_calls; static char s_set_server_name[48];
void cc_link_set_server(const char *n) { s_set_server_calls++; snprintf(s_set_server_name, sizeof(s_set_server_name), "%s", n ? n : ""); }
bool cc_link_set_claude_path(const char *p) { (void)p; return true; }
/* Plan 5 Task 3: the whisper settings (validated as the device does) and the check (PREVIEW_VOICE_RC; -1: not connected) */
static char s_pv_whisper_path[64] = CC_WHISPER_DEFAULT, s_pv_whisper_model[64] = CC_WHISPER_MODEL_DEFAULT;
static int s_voice_check_calls;
const char *cc_settings_whisper_path(char *o, size_t cap) { snprintf(o, cap, "%s", s_pv_whisper_path); return o; }
const char *cc_settings_whisper_model(char *o, size_t cap) { snprintf(o, cap, "%s", s_pv_whisper_model); return o; }
bool cc_link_set_whisper_path(const char *p) { if (!cc_path_valid(p)) return false; snprintf(s_pv_whisper_path, sizeof(s_pv_whisper_path), "%s", p); return true; }
bool cc_link_set_whisper_model(const char *p) { if (!cc_path_valid(p)) return false; snprintf(s_pv_whisper_model, sizeof(s_pv_whisper_model), "%s", p); return true; }
/* Plan 5 Task 6: a scriptable voice service. A case sets the phase, elapsed time and meter, and posts a result. */
static voice_status_t s_pv_voice;
static voice_result_t s_pv_vres; static bool s_pv_vres_ready;
static int s_voice_start_calls, s_voice_stop_calls, s_voice_cancel_calls;
vr_out_t voice_start(const char *key) {
    s_voice_start_calls++;
    if (s_pv_voice.phase != VOICE_IDLE) return VR_OUT_BUSY;
    memset(&s_pv_voice, 0, sizeof(s_pv_voice));
    s_pv_voice.phase = VOICE_STARTING;
    snprintf(s_pv_voice.key, sizeof(s_pv_voice.key), "%s", key);
    return VR_OUT_STARTED;
}
void voice_stop(void) {
    s_voice_stop_calls++;
    if (s_pv_voice.phase == VOICE_STARTING || s_pv_voice.phase == VOICE_RECORDING) { s_pv_voice.phase = VOICE_TRANSCRIBING; s_pv_voice.transcribing_ms = 0; }
}
/* As the service does: a recording ends at once (near enough); a transcription stays TRANSCRIBING, cancel pending,
   until the Host answers (review I1), and voice_start says BUSY meanwhile. */
void voice_cancel(void) {
    s_voice_cancel_calls++;
    if (s_pv_voice.phase == VOICE_TRANSCRIBING) s_pv_voice.cancelled = true;
    else s_pv_voice.phase = VOICE_IDLE;
}
void voice_status(voice_status_t *o) { *o = s_pv_voice; }
bool voice_take_result(voice_result_t *o) {
    if (!s_pv_vres_ready) return false;
    *o = s_pv_vres; memset(&s_pv_vres, 0, sizeof(s_pv_vres)); s_pv_vres_ready = false;
    return true;
}
bool voice_take_result_for(const char *key, voice_result_t *o) {
    if (!s_pv_vres_ready || !key || strcmp(s_pv_vres.key, key)) return false;
    return voice_take_result(o);
}
/* The service's end: a result for key, then idle. */
static void pv_voice_post(const char *key, vr_out_t out, const char *text, bool at_limit) {
    memset(&s_pv_vres, 0, sizeof(s_pv_vres));
    s_pv_vres.gen++; s_pv_vres.out = out; s_pv_vres.at_limit = at_limit;
    snprintf(s_pv_vres.key, sizeof(s_pv_vres.key), "%s", key);
    snprintf(s_pv_vres.text, sizeof(s_pv_vres.text), "%s", text ? text : "");
    s_pv_vres_ready = true;
    s_pv_voice.phase = VOICE_IDLE; s_pv_voice.cancelled = false;
}
esp_err_t cc_link_voice_check(int *rc) {
    s_voice_check_calls++;
    int v = env_int("PREVIEW_VOICE_RC", 0);
    if (v < 0) return ESP_ERR_INVALID_STATE;
    *rc = v;
    return ESP_OK;
}

/* Plan 6 Task 7: a scriptable update service. PREVIEW_FW_CHECK=none|checking|uptodate|newer|older|nofile|notapp is the
   check's result (none: no check cached; checking: the check stays pending), PREVIEW_FW_PREV=1 a runnable other slot,
   PREVIEW_FW_PHASE=download|check|write|readback|restart with PREVIEW_FW_PCT the update's progress. */
static char s_pv_fw_path[64] = CC_FW_PATH_DEFAULT;
const char *cc_settings_fw_path(char *o, size_t cap) { snprintf(o, cap, "%s", s_pv_fw_path); return o; }
bool cc_link_set_fw_path(const char *p) { if (!cc_fw_path_valid(p)) return false; snprintf(s_pv_fw_path, sizeof(s_pv_fw_path), "%s", p); return true; }
static const char *pv_fw_mode(void) { return env_str("PREVIEW_FW_CHECK", "none"); }
bool pv_fw_check_hold(void) { return !strcmp(pv_fw_mode(), "checking"); }
static int s_fw_check_calls, s_fw_start_calls, s_fw_cancel_calls, s_fw_goback_calls;
static fwu_phase_t s_pv_fw_phase = FWU_IDLE;
static fwu_note_t s_pv_fw_note; static bool s_pv_fw_note_ready;
static bool s_pv_fw_restaged;   /* fix I3: another build was staged after the confirm opened */
static bool pv_fw_fill(fwu_check_t *c) {
    const char *m = pv_fw_mode();
    memset(c, 0, sizeof(*c));
    c->size = 6374957;
    snprintf(c->sha, sizeof(c->sha), "3f2a9c1e0123456789abcdef0123456789abcdef0123456789abcdef01234567");
    snprintf(c->desc.project, sizeof(c->desc.project), "claude_companion");
    snprintf(c->desc.version, sizeof(c->desc.version), "1.6.1+3f2a9c1");
    if (!strcmp(m, "uptodate")) { c->offer = FW_OFFER_UP_TO_DATE; snprintf(c->desc.version, sizeof(c->desc.version), "1.6.0+80ab00e"); }
    else if (!strcmp(m, "newer")) c->offer = FW_OFFER_NEWER;
    else if (!strcmp(m, "older")) c->offer = FW_OFFER_OLDER;
    else if (!strcmp(m, "nofile")) { c->out = FW_OUT_NO_FILE; c->rc = 120; }
    else if (!strcmp(m, "notapp")) c->out = FW_OUT_NOT_APP;
    else return false;
    if (s_pv_fw_restaged) {
        c->out = FW_OUT_OK; c->offer = FW_OFFER_OLDER;
        snprintf(c->desc.version, sizeof(c->desc.version), "1.5.0+0ffee00");
        snprintf(c->sha, sizeof(c->sha), "0ffee0000123456789abcdef0123456789abcdef0123456789abcdef01234567");
    }
    return true;
}
void fwu_check(fwu_check_t *o) { s_fw_check_calls++; fwu_check_t c; pv_fw_fill(&c); if (o) *o = c; }
bool fwu_last_check(fwu_check_t *o) { fwu_check_t c; if (!pv_fw_fill(&c)) return false; if (o) *o = c; return true; }
fw_out_t fwu_start_expect(const char *sha) {
    fwu_check_t c;
    if (!pv_fw_fill(&c) || (sha && !fw_confirm_matches(sha, c.sha))) return FW_OUT_CHANGED;
    s_fw_start_calls++; s_pv_fw_phase = FWU_DOWNLOADING; return FW_OUT_OK;
}
fw_out_t fwu_start(void) { return fwu_start_expect(NULL); }
bool fwu_cancel(void) { s_fw_cancel_calls++; return s_pv_fw_phase != FWU_RESTARTING; }
bool fwu_pending(void) { return false; }
bool fwu_rollback_ok(void) { return env_int("PREVIEW_FW_ROLLBACK", 1) != 0; }
bool fwu_layout_ok(void) { return env_int("PREVIEW_FW_LAYOUT", 1) != 0; }
bool fwu_busy(void) { return s_pv_fw_phase != FWU_IDLE; }
void fwu_status(fwu_status_t *o) {
    o->phase = s_pv_fw_phase;
    o->total = 6374957;
    o->done = (o->total * (uint64_t)env_int("PREVIEW_FW_PCT", 50) + 99) / 100;   /* fw_pct gives back PREVIEW_FW_PCT */
}
static void pv_fw_post(fwu_note_kind_t kind, fw_out_t out, const char *a, const char *b) {
    memset(&s_pv_fw_note, 0, sizeof(s_pv_fw_note));
    s_pv_fw_note.gen++; s_pv_fw_note.kind = kind; s_pv_fw_note.out = out;
    snprintf(s_pv_fw_note.ver_a, sizeof(s_pv_fw_note.ver_a), "%s", a ? a : "");
    snprintf(s_pv_fw_note.ver_b, sizeof(s_pv_fw_note.ver_b), "%s", b ? b : "");
    s_pv_fw_note_ready = true;
}
bool fwu_take_note(fwu_note_t *o) { if (!s_pv_fw_note_ready) return false; *o = s_pv_fw_note; s_pv_fw_note_ready = false; return true; }
bool fwu_take_boot_note(fwu_note_t *o) { if (!s_pv_fw_note_ready || s_pv_fw_note.kind == FWU_NOTE_RESULT) return false; return fwu_take_note(o); }
bool fwu_take_result(fwu_note_t *o) { if (!s_pv_fw_note_ready || s_pv_fw_note.kind != FWU_NOTE_RESULT) return false; return fwu_take_note(o); }
bool fwu_peek_note(fwu_note_t *o) { *o = s_pv_fw_note; return s_pv_fw_note.gen != 0; }
const char *fwu_running_version(void) { return "1.6.0+80ab00e"; }
bool fwu_previous(fw_desc_t *o) {
    if (!env_int("PREVIEW_FW_PREV", 0)) return false;
    if (o) { memset(o, 0, sizeof(*o)); snprintf(o->version, sizeof(o->version), "1.5.9+0c1d2e3"); }
    return true;
}
fw_out_t fwu_go_back(void) { s_fw_goback_calls++; return FW_OUT_WRITE; }
/* PREVIEW_LOG=long: a 300-character line with no spaces, CJK and a numbered list (Review Focus 3).
   PREVIEW_LOG=screen:<fixture>: a captured `claude logs` stream from the host-test fixtures (output-pane fix). */
static int s_paste_calls;
esp_err_t cc_link_exec_tail(const char *cmd, char *out, size_t cap, int *st, char *err, size_t ecap, int ms) {
    (void)cmd; (void)ms; if (ecap) err[0] = 0; if (st) *st = 0;
    const char *lg = env_str("PREVIEW_LOG", "");
    if (!strncmp(lg, "screen:", 7)) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, lg + 7);
        FILE *f = fopen(path, "rb");
        size_t n = f ? fread(out, 1, cap - 1, f) : 0;
        if (f) fclose(f);
        out[n] = 0;
        return f ? ESP_OK : ESP_FAIL;
    }
    if (!strcmp(env_str("PREVIEW_LOG", ""), "long")) {
        char line[301];
        for (int i = 0; i < 300; i++) line[i] = (char)('a' + i % 26);
        line[300] = 0;
        snprintf(out, cap, "\xe2\x8f\xba Read src/%s.ts\r\n%s\r\n"
                 "Symbols \xe2\x96\x91\xe2\x96\x91\xe2\x96\x88\xe2\x96\x88 \xe2\x80\xa6 \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 \xe2\x9c\x93 \xe2\x97\x8f \xe2\x86\x90\xe2\x86\x93[\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91] 7%% of the width test \xe2\x80\xa6\r\n"   /* not chrome: the fallback keeps it */
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
esp_err_t cc_link_exec_screen(const char *cmd, char *out, size_t cap, int *st, char *err, size_t ecap, int ms) {
    return cc_link_exec_tail(cmd, out, cap, st, err, ecap, ms);   /* the stubs' streams fit: all of them is the tail */
}
esp_err_t cc_link_paste(const char *id, const char *t, char *err, size_t cap) { (void)id; (void)t; s_paste_calls++; if (cap) err[0] = 0; return ESP_OK; }
static int s_check_calls;
esp_err_t cc_link_check_live(const char *id, char *err, size_t cap) { (void)id; s_check_calls++; if (cap) err[0] = 0; return ESP_OK; }
/* Ctrl+X (the stop job): the only cc_link_exec on Sessions. It records the command line, answers with the exit status
   s_pv_stop_rc and output that must never be shown, and the list refresh after a stop is counted. */
static int s_stop_calls, s_poll_now_calls, s_pv_stop_rc;
static char s_stop_cmd[192];
#define PV_STOP_OUTPUT "secret-stop-output"
esp_err_t cc_link_exec_slow(const char *cmd, char *out, size_t cap, int *st, char *err, size_t ecap, int ms) {
    (void)ms; s_stop_calls++;
    snprintf(s_stop_cmd, sizeof(s_stop_cmd), "%s", cmd ? cmd : "");
    if (cap) snprintf(out, cap, PV_STOP_OUTPUT);
    if (ecap) snprintf(err, ecap, PV_STOP_OUTPUT);
    if (st) *st = s_pv_stop_rc;
    return ESP_OK;
}
esp_err_t cc_link_poll_now(int ms) { (void)ms; s_poll_now_calls++; return ESP_OK; }
static int s_forget_calls;
#define PREVIEW_FP "SHA256:Xq3v9yPreviewFingerprintOnly0123456789abcd"
esp_err_t cc_link_forget_host_key(char *fp, size_t cap, int ms) {
    (void)ms; s_forget_calls++;
    snprintf(fp, cap, PREVIEW_FP);
    return ESP_OK;
}
static int s_trust_calls;
static char s_trust_fp[80];
esp_err_t cc_link_trust_host_key(const char *want, char *fp, size_t cap, int ms) {
    (void)ms; s_trust_calls++;
    snprintf(s_trust_fp, sizeof(s_trust_fp), "%s", want);
    s_pv_trusted = true;   /* the link met the confirmed key and pinned it */
    snprintf(fp, cap, "%s", want);
    return ESP_OK;
}
#define PREVIEW_OTHER_FP "SHA256:OtherPreviewHostKeyOnly9876543210zyxwvutsr"
static bool s_pv_other_key;   /* newkey: the Host now presents another unknown key (fix round 1, C1) */
static bool s_pv_key_cleared;   /* newkey: a reconnect cleared the unknown key (polish 2, N3) */
bool cc_link_unknown_host_key(char *fp, size_t cap) {
    bool on = pv_newkey() && !s_pv_key_cleared;
    snprintf(fp, cap, "%s", on ? (s_pv_other_key ? PREVIEW_OTHER_FP : PREVIEW_NEW_FP) : "");
    return on;
}
uint32_t cc_link_forget_done(char *fp, size_t cap) {
    snprintf(fp, cap, "%s", s_trust_calls ? s_trust_fp : PREVIEW_FP);
    return (uint32_t)(s_forget_calls + s_trust_calls);
}
bool cc_link_pinned_fingerprint(char *fp, size_t cap) {
    if (pv_newkey()) { if (cap) fp[0] = 0; return false; }   /* first run: no key pinned yet */
    snprintf(fp, cap, "%s", s_pv_trusted ? PREVIEW_NEW_FP : "SHA256:OldPreviewKeyOnly9876543210zyxwvutsrqponm");
    return true;
}
esp_err_t cc_link_debug_corrupt_host_key(char *h, size_t cap) { if (cap) h[0] = 0; return ESP_OK; }
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
static int keyboard_pick=-2;
static char *s_prompt_got; static int s_prompt_calls;
static void keyboard_prompted(const char *t, void *ud) { (void)ud; s_prompt_calls++; free(s_prompt_got); s_prompt_got = t ? strdup(t) : NULL; }
static void keyboard_picked(int index, void *ud) { (void)ud; keyboard_pick=index; }
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
    /* opened after a key, Cancel has the focus: one Tab reaches the action */
    ui_confirm("test",NULL,keyboard_confirmed,NULL);
    press_key(SK_TAB,0,true); press_key(SK_ENTER,0,true);
    assert(keyboard_answer==1);
    /* stop review I1: on every confirm (Forget, Delete, Disconnect, Stop ...) an arrow never moves onto the action, so
       arrow keys then Enter cancel; Tab or Shift+Tab then Enter confirms */
    {
        static const special_key_t arrows[] = { SK_UP, SK_LEFT, SK_DOWN, SK_RIGHT };
        for (int destructive = 0; destructive < 2; destructive++) {
            for (int a = 0; a < 4; a++) {
                keyboard_answer=0; ui_confirm_ex("test",NULL,"Go",destructive,keyboard_confirmed,NULL);
                press_key(arrows[a],0,true); press_key(arrows[a],0,true); press_key(SK_ENTER,0,true);
                assert(keyboard_answer==-1);
            }
            keyboard_answer=0; ui_confirm_ex("test",NULL,"Go",destructive,keyboard_confirmed,NULL);
            press_key(SK_TAB,MOD_SHIFT,true); press_key(SK_ENTER,0,true); assert(keyboard_answer==1);
            keyboard_answer=0; ui_confirm_ex("test",NULL,"Go",destructive,keyboard_confirmed,NULL);
            press_key(SK_TAB,0,true); press_key(SK_UP,0,true); press_key(SK_ENTER,0,true);   /* off the action by arrow: fine */
            assert(keyboard_answer==-1);
        }
    }
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
    /* T9-18: in an action sheet the first Down lands on the first action, not on the scrim (Enter there closed it) */
    const char *acts[]={"first","second"};
    ui_action_sheet("sheet",acts,2,0,keyboard_picked,NULL);
    press_key(SK_DOWN,0,false); press_key(SK_ENTER,0,false); assert(keyboard_pick==0 && !ui_keyboard_owns_input());
    ui_action_sheet("sheet",acts,2,0,keyboard_picked,NULL);
    press_key(SK_DOWN,0,false); press_key(SK_DOWN,0,false); press_key(SK_ENTER,0,false); assert(keyboard_pick==1);
    /* a fresh picker's first key lands on its current row, so a bare Enter changes nothing (review M6) */
    ui_picker("picker",acts,2,1,keyboard_picked,NULL);
    press_key(SK_TAB,0,false); press_key(SK_ENTER,0,false); assert(keyboard_pick==1);
    keyboard_pick=-2; ui_picker("picker",acts,2,1,keyboard_picked,NULL);
    press_key(SK_ENTER,0,false); assert(keyboard_pick==1);
    keyboard_pick=-2; ui_picker("picker",acts,2,1,keyboard_picked,NULL);
    press_key(SK_DOWN,0,false); press_key(SK_UP,0,false); press_key(SK_ENTER,0,false); assert(keyboard_pick==0);
    /* M1 (final review): a picker with no current row: a bare Enter (or Space) only lands on the first row */
    keyboard_pick=-2; ui_picker("picker",acts,2,-1,keyboard_picked,NULL);
    press_key(SK_ENTER,0,false); assert(keyboard_pick==-2 && ui_keyboard_owns_input());
    press_key(SK_ENTER,0,false); assert(keyboard_pick==0);   /* the row it now shows focused */
    keyboard_pick=-2; ui_menu("menu",acts,2,keyboard_picked,NULL);
    { key_event_t sp={.type=KEY_EV_CHAR,.cp=' '}; ui_keyboard_route(&sp,false); } assert(keyboard_pick==-2);
    press_key(SK_DOWN,0,false); press_key(SK_ENTER,0,false); assert(keyboard_pick==1);
    /* T10-8 stays: a confirm's first control is Cancel, so Enter alone cancels */
    keyboard_answer=0; ui_confirm_ex("test",NULL,"Go",false,keyboard_confirmed,NULL);
    press_key(SK_ENTER,0,false); assert(keyboard_answer==-1);
    /* polish 1: Enter in a prompt's one-line field presses OK. The router leaves a textarea's keys to the keyboard
       sink (input/keyboard.c, not built here), whose Enter sends a one-line field READY: that step is done by hand */
    s_prompt_got = NULL; s_prompt_calls = 0;
    ui_prompt("prompt","","~/bin/claude",false,keyboard_prompted,NULL);
    press_key(SK_TAB,0,false);
    lv_obj_t *pta=keyboard_lvgl_focused();
    assert(pta && lv_textarea_get_one_line(pta));
    assert(!press_key(SK_ENTER,0,false));                      /* the router passes Enter on */
    lv_obj_send_event(pta,LV_EVENT_READY,NULL);                /* the sink's one-line Enter */
    assert(s_prompt_calls==1 && s_prompt_got && !strcmp(s_prompt_got,"~/bin/claude") && !ui_keyboard_owns_input());
    puts("Keyboard navigation, slider persistence, modal isolation, mixed input, switches, deletion, sheet focus and SSH routing checks passed.");
}

static void terminal_spacing_check(void) {
    lv_obj_t *scr=ui_screen_base(); lv_screen_load(scr);
    fonts_set_term_line_height(32);
    vt_t *vt=vt_create(80,16,10);
    assert(vt);
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
    assert(snapshot(scr,"term-spacing.ppm")==0);
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
    assert(vt);
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
    ui_srow(col, UI_ROW_VALUE, "Fallback address", "192.168.77.20", noop_cb, NULL);
    ui_srow(col, UI_ROW_SWITCH, "Tailscale", "on", noop_cb, NULL);
    ui_srow(col, UI_ROW_NAV, "Host", "studio-host.local", noop_cb, NULL);
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

/* ------------------------------------------------------------ voice input on Sessions (Plan 5 Task 7) */
static int timers_with_period(uint32_t ms);
static int shown_index(int raw);   /* the list hides interactive sessions */
/* The shown 64 x 64 button in the detail pane: the mic. */
static lv_obj_t *mic_button_of(lv_obj_t *o) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_button_class) && lv_obj_get_width(o) == UI_TK_TARGET && lv_obj_get_height(o) == UI_TK_TARGET) {
        lv_area_t a; lv_obj_get_coords(o, &a);
        if (a.x1 >= 440) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = mic_button_of(lv_obj_get_child(o, i));
        if (r) return r;
    }
    return NULL;
}
static bool label_red(lv_obj_t *l) { return l && lv_color_eq(lv_obj_get_style_text_color(l, 0), lv_color_hex(UI_TK_RED)); }
static bool mic_ring_red(lv_obj_t *scr) {
    lv_obj_t *m = mic_button_of(scr);
    return m && lv_obj_get_style_border_width(m, 0) == 2 && lv_color_eq(lv_obj_get_style_border_color(m, 0), lv_color_hex(UI_TK_RED));
}
/* Ctrl+Space starts a recording on the selected session; the stub then records for ms at meter m. */
static void pv_record(lv_display_t *d, int ms, int m) {
    int calls = s_voice_start_calls;
    assert(sink_ch(' ', MOD_CTRL) && s_voice_start_calls == calls + 1);
    s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = ms; s_pv_voice.meter = m;
    step(d, 400); step(d, 400);   /* the 100 ms meter and the 300 ms tick; past the 700 ms toggle guard */
}
/* A second Ctrl+Space stops it: transcribing. */
static void pv_stop(lv_display_t *d) {
    int calls = s_voice_stop_calls;
    assert(sink_ch(' ', MOD_CTRL) && s_voice_stop_calls == calls + 1);
    assert(s_pv_voice.phase == VOICE_TRANSCRIBING);
    step(d, 400); step(d, 400);
}
static char *key_of(int i, char *k) { sm_key(&s_preview_sessions[i], k, SM_KEY_MAX); return k; }
static lv_obj_t *find_clickable_shown(lv_obj_t *o, const char *t);
/* A toast showing text whole: its label is as wide as the text, so nothing is cut with dots. */
static bool toast_whole(lv_obj_t *top, const char *text) {
    lv_obj_t *l = find_label(top, text, false);
    if (!l) return false;
    lv_point_t sz;
    lv_text_get_size(&sz, text, lv_obj_get_style_text_font(l, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x <= lv_obj_get_width(l);
}
/* Review M1: while recording or transcribing, Send, Open and a tap on the selected row do nothing, like Enter. */
static int pv_taps_inert(lv_display_t *d, lv_obj_t *s, lv_obj_t *list, const char *name) {
    int pastes = s_paste_calls, checks = s_check_calls;
    voice_phase_t ph = s_pv_voice.phase;
    char before[SM_ANSWER_MAX + 1]; snprintf(before, sizeof(before), "%s", field_text(s) ? field_text(s) : "");
    lv_obj_t *send = find_clickable_shown(s, tr(STR_SES_SEND)), *open = find_clickable_shown(s, tr(STR_SES_OPEN));
    assert(send && open);
    lv_obj_send_event(send, LV_EVENT_CLICKED, NULL); step(d, 400);
    lv_obj_send_event(open, LV_EVENT_CLICKED, NULL); step(d, 400);
    lv_obj_t *row = lv_obj_get_child(list, selected_index(list));
    lv_obj_send_event(row, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 400);
    if (s_paste_calls != pastes || s_check_calls != checks || !ui_nav_is_top(ui_sessions_create) || s_pv_voice.phase != ph
        || strcmp(field_text(s), before)) {
        printf("check %s: FAIL a tap acted while busy (%d pastes, %d checks, draft \"%s\")\n", name,
               s_paste_calls - pastes, s_check_calls - checks, field_text(s));
        return 1;
    }
    return 0;
}

static int voice_case(lv_display_t *d, lv_obj_t *s, lv_obj_t *list, const char *name, const char *v) {
    char why[256], k0[SM_KEY_MAX];
    int r = 0;
    if (!strcmp(v, "voice-rec")) {
        sink_text("ab"); step(d, 400);
        pv_record(d, 7000, 60);
        lv_obj_t *t = find_label(s, "0:07 / 0:30", false);
        if (!t || !obj_shown(t)) { printf("check %s: FAIL no timer 0:07 / 0:30\n", name); return 1; }
        assert(!label_red(t) && find_label(s, "Recording", false) && find_label(s, "Ctrl+Space stops \xc2\xb7 Esc cancels", false));
        lv_obj_t *bar = find_shown(s, &lv_bar_class);
        assert(bar && lv_bar_get_value(bar) == 60 && lv_obj_get_width(bar) == 160 && lv_obj_get_height(bar) == 8);
        if (!mic_ring_red(s)) { printf("check %s: FAIL the mic has no 2 px red border\n", name); return 1; }
        if (timers_with_period(100) != 1 || !ui_sessions_voice_meter()) { printf("check %s: FAIL %d 100 ms timers\n", name, timers_with_period(100)); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);   /* the amber scan, the fit, the targets, the timers (the meter exempt) */
        /* every key but Ctrl+Space and Esc is eaten: the draft, the paste and the selection stay */
        int sel = selected_index(list);
        sink_text("x"); assert(sink_sk(SK_ENTER)); assert(sink_sk(SK_DOWN)); assert(sink_sk(SK_BACKSPACE)); step(d, 400);
        if (strcmp(field_text(s), "ab") || s_paste_calls || selected_index(list) != sel) {
            printf("check %s: FAIL keys acted while recording (draft \"%s\", %d pastes, row %d)\n", name, field_text(s), s_paste_calls, selected_index(list));
            return 1;
        }
        if (pv_taps_inert(d, s, list, name)) return 1;   /* with a draft: Send would paste it */
        /* Esc cancels (and keeps the draft) */
        assert(sink_sk(SK_ESC)); step(d, 400);
        if (s_voice_cancel_calls != 1 || strcmp(field_text(s), "ab")) { printf("check %s: FAIL Esc: %d cancels, draft \"%s\"\n", name, s_voice_cancel_calls, field_text(s)); return 1; }
        step(d, 400);
        if (timers_with_period(100) != 0 || ui_sessions_voice_meter()) { printf("check %s: FAIL the meter outlived the cancel\n", name); return 1; }
        if (find_label(s, "0:07 / 0:30", false) && obj_shown(find_label(s, "0:07 / 0:30", false))) { printf("check %s: FAIL the strip outlived the cancel\n", name); return 1; }
        /* a new recording; Ctrl+Space past the 700 ms guard stops it */
        pv_record(d, 2000, 40);
        pv_stop(d);
        if (s_voice_stop_calls != 1) { printf("check %s: FAIL %d stops\n", name, s_voice_stop_calls); return 1; }
        assert(s_paste_calls == 0);
        return r;
    }
    if (!strcmp(v, "voice-rec-limit")) {
        pv_record(d, 27000, 80);
        lv_obj_t *t = find_label(s, "0:27 / 0:30", false);
        if (!label_red(t)) { printf("check %s: FAIL the timer at 0:27 is not red\n", name); return 1; }
        return pv_run_checks(d, s, name, true, 0);
    }
    if (!strcmp(v, "voice-busy")) {
        pv_record(d, 4000, 50);
        pv_stop(d);
        s_pv_voice.transcribing_ms = 5000; step(d, 400);
        lv_obj_t *n = find_label(s, "Transcribing on the Host\xe2\x80\xa6 5 s", false);
        if (!n || !obj_shown(n)) { printf("check %s: FAIL no transcribing note with its seconds\n", name); return 1; }
        if (timers_with_period(100) != 0) { printf("check %s: FAIL a 100 ms timer while transcribing\n", name); return 1; }
        if (mic_ring_red(s)) { printf("check %s: FAIL the mic is still red\n", name); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);
        /* Ctrl+Space while transcribing says so and starts nothing; Enter is eaten */
        int starts = s_voice_start_calls;
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        assert(s_voice_start_calls == starts && find_label(lv_layer_top(), "Still transcribing\xe2\x80\xa6", false));
        sink_text("q"); assert(sink_sk(SK_ENTER)); step(d, 400);
        assert(!strcmp(field_text(s), "") && s_paste_calls == 0);
        if (pv_taps_inert(d, s, list, name)) return 1;   /* an empty draft: Send would open a take-over */
        return r;
    }
    if (!strcmp(v, "voice-cancel-busy")) {
        /* review I1: Esc while transcribing gives control back at once, though the service stays TRANSCRIBING with
           the cancel pending until the Host answers (a cold Host, DERP: up to 60 s) */
        pv_record(d, 4000, 50);
        pv_stop(d);
        s_pv_voice.transcribing_ms = 5000; step(d, 400);
        lv_obj_t *n = find_label(s, "Transcribing on the Host\xe2\x80\xa6 5 s", false);
        assert(n && obj_shown(n));
        assert(sink_sk(SK_ESC)); step(d, 400);
        assert(s_voice_cancel_calls == 1 && s_pv_voice.phase == VOICE_TRANSCRIBING && s_pv_voice.cancelled);
        s_pv_voice.transcribing_ms = 9000; step(d, 400);
        n = find_label_has(s, "Transcribing on the Host", 0);
        if (n && obj_shown(n)) { printf("check %s: FAIL the transcribing note outlived the cancel: \"%s\"\n", name, lv_label_get_text(n)); return 1; }
        if (mic_ring_red(s) || timers_with_period(100) != 0) { printf("check %s: FAIL the mic isn't idle after the cancel\n", name); return 1; }
        /* the keys pass again: typing reaches the draft, the arrows move the selection */
        sink_text("q"); step(d, 400);
        if (strcmp(field_text(s), "q")) { printf("check %s: FAIL keys still eaten after the cancel (draft \"%s\")\n", name, field_text(s)); return 1; }
        int sel = selected_index(list);
        assert(sink_sk(SK_DOWN)); step(d, 400);
        if (selected_index(list) == sel) { printf("check %s: FAIL Down didn't move the selection after the cancel\n", name); return 1; }
        assert(sink_sk(SK_UP)); step(d, 400);
        assert(selected_index(list) == sel && !strcmp(field_text(s), "q"));
        r |= pv_run_checks(d, s, name, true, 0);
        /* Ctrl+Space while the old transcription is still in flight: voice_start says BUSY, the UI says so, nothing
           records */
        int starts = s_voice_start_calls;
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        if (s_voice_start_calls != starts + 1 || s_pv_voice.phase != VOICE_TRANSCRIBING || !s_pv_voice.cancelled) {
            printf("check %s: FAIL a second recording started over the cancelled transcription\n", name); return 1;
        }
        if (!find_label(lv_layer_top(), "Still transcribing\xe2\x80\xa6", false)) { printf("check %s: FAIL no busy toast\n", name); return 1; }
        if (timers_with_period(100) != 0 || mic_ring_red(s)) { printf("check %s: FAIL the mic shows a recording\n", name); return 1; }
        /* the Host answers; the service drops the result and goes idle: the draft is only what was typed */
        s_pv_voice.phase = VOICE_IDLE; s_pv_voice.cancelled = false; step(d, 400); step(d, 400);
        if (strcmp(field_text(s), "q") || s_paste_calls) { printf("check %s: FAIL draft \"%s\", %d pastes\n", name, field_text(s), s_paste_calls); return 1; }
        if (s_note_activity_calls) { printf("check %s: FAIL a cancelled run counted as activity\n", name); return 1; }
        return r;
    }
    if (!strcmp(v, "voice-auto-stop")) {
        /* review M7: the check fails 0.4 s in and the service ends the recording; the user's stop press, just after,
           starts nothing. A press past the guard does start. */
        key_of(0, k0);
        pv_record(d, 400, 50);
        pv_voice_post(k0, VR_OUT_NO_WHISPER, "", false); step(d, 100);
        int starts = s_voice_start_calls;
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        if (s_voice_start_calls != starts || s_pv_voice.phase != VOICE_IDLE) { printf("check %s: FAIL the late stop press started a recording\n", name); return 1; }
        step(d, 400); step(d, 400);
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        if (s_voice_start_calls != starts + 1) { printf("check %s: FAIL a press past the guard didn't start\n", name); return 1; }
        voice_cancel(); step(d, 400);
        return pv_run_checks(d, s, name, true, 0);
    }
    if (!strcmp(v, "voice-done")) {
        /* record for the first session; while transcribing, select the second by touch and re-sort the list */
        char first[96]; snprintf(first, sizeof(first), "%s", s_preview_sessions[0].name);
        key_of(0, k0);
        sink_text("old"); step(d, 400);
        pv_record(d, 3000, 50);
        pv_stop(d);
        lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(selected_index(list) == 1);
        char second[96]; snprintf(second, sizeof(second), "%s", s_preview_sessions[1].name);
        preview_resort_to_last(0); step(d, 400);
        assert(find_label_x(s, second, 440));
        pv_voice_post(k0, VR_OUT_TEXT, "Please run the tests.", false); step(d, 400);
        /* polish 2: the result restarts the idle clock (power_note_activity), so the screen doesn't dim as it lands */
        if (s_note_activity_calls != 1) { printf("check %s: FAIL power_note_activity called %d times\n", name, s_note_activity_calls); return 1; }
        if (strcmp(field_text(s), "")) { printf("check %s: FAIL the second session's draft is \"%s\"\n", name, field_text(s)); return 1; }
        /* select the first session again (now the last row): its draft has the text, and the review note */
        lv_obj_t *row = row_named(s, first);
        assert(row);
        lv_obj_send_event(row, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(find_label_x(s, first, 440));
        if (strcmp(field_text(s), "old Please run the tests.")) { printf("check %s: FAIL the first session's draft is \"%s\"\n", name, field_text(s)); return 1; }
        lv_obj_t *n = find_label(s, "Check the text, then press Enter to send", false);
        if (!n || !obj_shown(n)) { printf("check %s: FAIL no review note\n", name); return 1; }
        /* the whole note doesn't fit beside Stop: Stop gives way (stop_fits), and comes back when the note goes */
        lv_obj_t *stopb = find_clickable_shown(s, "\xe2\x8c\x83X Stop");
        if (!stopb || obj_shown(stopb)) { printf("check %s: FAIL Stop shown beside a note it would cut\n", name); return 1; }
        if (s_paste_calls) { printf("check %s: FAIL cc_link_paste was called\n", name); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);
        step(d, 5200);   /* the review note goes after 5 s; the text stays */
        assert(!find_label(s, "Check the text, then press Enter to send", false) || !obj_shown(find_label(s, "Check the text, then press Enter to send", false)));
        assert(!strcmp(field_text(s), "old Please run the tests.") && s_paste_calls == 0);
        if (!obj_shown(stopb)) { printf("check %s: FAIL Stop didn't come back after the note\n", name); return 1; }
        return r;
    }
    if (!strcmp(v, "voice-gone")) {
        pv_record(d, 3000, 50);
        pv_stop(d);
        pv_voice_post("gone:no-such-session", VR_OUT_TEXT, "Lost words.", false); step(d, 400);
        if (!find_label(lv_layer_top(), "That session is gone, so the text was dropped", false)) { printf("check %s: FAIL no gone toast\n", name); return 1; }
        assert(!strcmp(field_text(s), "") && s_paste_calls == 0);
        for (int i = 0; i < s_preview_n; i++) assert(!find_label_has(s, "Lost words", 0));
        return pv_run_checks(d, s, name, true, 0);
    }
    if (!strcmp(v, "voice-error")) {
        key_of(0, k0);
        pv_record(d, 3000, 50);
        pv_stop(d);
        pv_voice_post(k0, VR_OUT_NO_MODEL, "", false); step(d, 400);
        lv_obj_t *n = find_label_has(s, "The whisper model isn't on the Host", 440);   /* dotted to its slot */
        if (!n || !obj_shown(n) || !label_red(n)) { printf("check %s: FAIL no red NO_MODEL note\n", name); return 1; }
        printf("sessions voice-error: note \"%s\"\n", lv_label_get_text(n));
        lv_obj_t *tl = find_label(lv_layer_top(), tr(STR_VOICE_NO_MODEL), false);
        if (!tl) { printf("check %s: FAIL no toast with the whole copy\n", name); return 1; }
        /* polish 2: the 6 s toast sits above the answer row; the field, the mic, Send and Open stay in view */
        lv_area_t ta, oa;
        lv_obj_get_coords(lv_obj_get_parent(tl), &ta);
        lv_obj_t *keep[] = { find_clickable_shown(s, tr(STR_SES_SEND)), find_clickable_shown(s, tr(STR_SES_OPEN)), mic_button_of(s),
                             find_shown(s, &lv_textarea_class) };
        for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
            assert(keep[i]);
            lv_obj_get_coords(keep[i], &oa);
            if (ta.y2 >= oa.y1 && ta.y1 <= oa.y2 && ta.x2 >= oa.x1 && ta.x1 <= oa.x2) {
                printf("check %s: FAIL the toast (y %d-%d) covers control %u (y %d-%d)\n", name, (int)ta.y1, (int)ta.y2, i, (int)oa.y1, (int)oa.y2);
                return 1;
            }
        }
        printf("sessions voice-error: toast y %d-%d\n", (int)ta.y1, (int)ta.y2);
        assert(!strcmp(field_text(s), ""));
        return pv_run_checks(d, s, name, true, 0);   /* the note dots inside its slot */
    }
    if (!strcmp(v, "voice-norow")) {
        /* M3: recording on the first session, then a tap on a stopped one, which has no answer field (so no mic). The
           strip stays while recording and the transcribing note after the stop; once the result is in, the stopped
           session's pane is back to its plain look. */
        key_of(0, k0);
        pv_record(d, 5000, 50);
        lv_obj_t *row = row_named(s, "done one");
        assert(row);
        lv_obj_send_event(row, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(find_label_x(s, "done one", 440) && !field_text(s) && !mic_button_of(s));
        lv_obj_t *rec = find_label(s, "Recording", false), *t = find_label(s, "0:05 / 0:30", false);
        if (!rec || !obj_shown(rec) || !t || !obj_shown(t)) { printf("check %s: FAIL no voice strip on a row with no field\n", name); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);
        pv_stop(d);
        s_pv_voice.transcribing_ms = 4000; step(d, 400);
        lv_obj_t *n = find_label(s, "Transcribing on the Host\xe2\x80\xa6 4 s", false);
        if (!n || !obj_shown(n)) { printf("check %s: FAIL no transcribing note on a row with no field\n", name); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);   /* the snapshot shows this: the note beside Open */
        pv_voice_post(k0, VR_OUT_TEXT, "Please run the tests.", false); step(d, 400); step(d, 400);
        n = find_label_has(s, "Transcribing on the Host", 0);
        rec = find_label(s, "Recording", false);
        if ((n && obj_shown(n)) || (rec && obj_shown(rec))) { printf("check %s: FAIL the voice row outlived the result\n", name); return 1; }
        if (!find_label_x(s, "done one", 440) || s_paste_calls) { printf("check %s: FAIL the selection or a paste changed\n", name); return 1; }
        return r;
    }
    if (!strcmp(v, "voice-nofield")) {
        int k = -1;
        for (int i = 0; i < s_preview_n; i++) if (s_preview_sessions[i].background && s_preview_sessions[i].state == CC_DONE) k = i;
        assert(k >= 0);
        k = shown_index(k);
        for (int i = 0; i < k; i++) { assert(sink_sk(SK_DOWN)); step(d, 400); }
        assert(selected_index(list) == k && !field_text(s));
        assert(sink_ch(' ', MOD_CTRL)); step(d, 100);
        if (s_voice_start_calls) { printf("check %s: FAIL voice_start called\n", name); return 1; }
        if (!find_label(lv_layer_top(), "Voice input works in a session's answer field", false)) { printf("check %s: FAIL no needs-field toast\n", name); return 1; }
        return pv_run_checks(d, s, name, true, 0);
    }
    if (!strcmp(v, "voice-kbd-banner")) {
        /* review I1: a banner up (low battery) and the soft keyboard up: the whole action row (Send, Open, and Cancel
           while recording) ends above the keys; the banner going and coming back with the keyboard up keeps it there;
           the keyboard hidden puts it back above the footer */
        const char *bat = "Battery 14 %. The Tab5 sleeps at 10 %.";
        lv_obj_t *ta = find_shown(s, &lv_textarea_class), *send = find_clickable_shown(s, tr(STR_SES_SEND));
        while (send && lv_obj_get_parent(send) && lv_obj_get_height(lv_obj_get_parent(send)) != UI_TK_TARGET) send = lv_obj_get_parent(send);
        lv_obj_t *row = send ? lv_obj_get_parent(send) : NULL;   /* the action row: Send's 64 px parent */
        lv_obj_t *foot = lv_obj_get_parent(lv_obj_get_parent(find_clickable(s, tr(STR_SES_SELECT))));
        if (!ta || !row || !foot || !find_label(s, bat, false) || !obj_shown(find_label(s, bat, false))) { printf("check %s: FAIL setup (banner, field, action row)\n", name); return 1; }
        lv_area_t fo; lv_obj_get_coords(foot, &fo);
        for (int round = 0; round < 3; round++) {
            /* round 0: banner and keyboard; 1: recording with them (Cancel in the row); 2: the banner goes under the
               keyboard and comes back */
            lv_obj_send_event(ta, LV_EVENT_CLICKED, NULL); step(d, 400);
            if (!softkbd_visible()) { printf("check %s: FAIL no soft keyboard\n", name); return 1; }
            if (round == 1) {
                lv_obj_send_event(mic_button_of(s), LV_EVENT_CLICKED, NULL); step(d, 100);
                s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 3000; step(d, 400); step(d, 400);
                lv_obj_t *c = find_clickable_shown(s, "Cancel");
                if (!c || !obj_shown(c)) { printf("check %s: FAIL no Cancel while recording\n", name); return 1; }
            }
            if (round == 2) {
                setenv("PREVIEW_BAT", "52", 1); step(d, 400); step(d, 400);
                if (obj_shown(find_label(s, bat, false))) { printf("check %s: FAIL the banner didn't go\n", name); return 1; }
            }
            for (int pass = 0; pass < (round == 2 ? 2 : 1); pass++) {
                if (pass) { setenv("PREVIEW_BAT", "14", 1); step(d, 400); step(d, 400); }
                lv_area_t ra, ka;
                lv_obj_update_layout(s);
                lv_obj_get_coords(row, &ra);
                lv_obj_get_coords(lv_obj_get_child(s, (int32_t)lv_obj_get_child_count(s) - 1), &ka);
                if (ra.y2 >= ka.y1) { printf("check %s: FAIL round %d.%d: the action row (y2 %d) is under the keyboard (y %d)\n", name, round, pass, (int)ra.y2, (int)ka.y1); return 1; }
            }
            if (round == 0) r |= pv_run_checks(d, s, name, true, 0);
            if (round == 1) { r |= pv_run_checks(d, s, "sessions-voice-kbd-banner-rec", true, 0); voice_cancel(); step(d, 400); step(d, 400); }
            softkbd_hide_all(); step(d, 400);
            lv_area_t ra; lv_obj_update_layout(s); lv_obj_get_coords(row, &ra);
            if (ra.y2 >= fo.y1 || !obj_shown(foot)) { printf("check %s: FAIL round %d: after the keyboard the action row (y2 %d) is under the footer (y %d)\n", name, round, (int)ra.y2, (int)fo.y1); return 1; }
            step(d, 800);
        }
        printf("%s: with the banner up the soft keyboard keeps the action row (and Cancel) above it, also across the banner going and coming; hidden, the row is back above the footer\n", name);
        return r;
    }
    if (!strcmp(v, "voice-cancel-touch")) {
        /* touch alone (PREVIEW_KBD=0): the field brings up the soft keyboard with the field and the mic above it; the
           mic starts, the strip's Cancel drops the recording and then a transcription, and nothing is applied */
        lv_obj_t *ta = find_shown(s, &lv_textarea_class), *mic = mic_button_of(s);
        assert(ta && mic);
        lv_area_t fa0; lv_obj_get_coords(ta, &fa0);
        lv_obj_send_event(ta, LV_EVENT_CLICKED, NULL); step(d, 400);
        if (!softkbd_visible()) { printf("check %s: FAIL a tap on the field brought no soft keyboard\n", name); return 1; }
        lv_area_t ka, fa, ma;
        lv_obj_get_coords(lv_obj_get_child(s, (int32_t)lv_obj_get_child_count(s) - 1), &ka);   /* the keyboard: the last child */
        lv_obj_get_coords(ta, &fa); lv_obj_get_coords(mic, &ma);
        if (fa.y2 >= ka.y1 || ma.y2 >= ka.y1) { printf("check %s: FAIL the soft keyboard (y %d) covers the field (y2 %d) or the mic (y2 %d)\n", name, (int)ka.y1, (int)fa.y2, (int)ma.y2); return 1; }
        {
            lv_area_t sa2; lv_obj_get_coords(lv_obj_get_parent(find_clickable_shown(s, tr(STR_SES_SEND))), &sa2);
            if (sa2.y2 >= ka.y1) { printf("check %s: FAIL the soft keyboard covers the action row (y2 %d)\n", name, (int)sa2.y2); return 1; }
        }
        sink_text("ab"); step(d, 400);
        r |= pv_run_checks(d, s, "sessions-voice-touch-kbd", true, 0);
        softkbd_hide_all(); step(d, 400);
        lv_obj_get_coords(ta, &fa);
        lv_obj_t *selchip = find_clickable(s, tr(STR_SES_SELECT));
        if (fa.y2 != fa0.y2 || !selchip || !obj_shown(selchip)) { printf("check %s: FAIL the pane or the footer didn't come back after the keyboard (field y2 %d, was %d)\n", name, (int)fa.y2, (int)fa0.y2); return 1; }
        /* the mic starts a recording */
        int starts = s_voice_start_calls;
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 100);
        if (s_voice_start_calls != starts + 1) { printf("check %s: FAIL the mic didn't start\n", name); return 1; }
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 7000; s_pv_voice.meter = 60;
        step(d, 400); step(d, 400);
        lv_obj_t *cancel = find_clickable_shown(s, "Cancel"), *hint = find_label(s, "Tap the mic to stop", false);
        if (!cancel || !obj_shown(cancel) || !hint || !obj_shown(hint)) { printf("check %s: FAIL no Cancel or no \"Tap the mic to stop\"\n", name); return 1; }
        lv_obj_t *kh = find_label(s, "Ctrl+Space stops \xc2\xb7 Esc cancels", false);   /* built, hidden (review M3) */
        if (kh && obj_shown(kh)) { printf("check %s: FAIL a key hint with no keyboard\n", name); return 1; }
        if (lv_obj_get_height(cancel) < UI_TK_TARGET) { printf("check %s: FAIL Cancel is %d px\n", name, (int)lv_obj_get_height(cancel)); return 1; }
        lv_obj_t *open = find_clickable_shown(s, tr(STR_SES_OPEN));
        lv_area_t oa;
        lv_obj_get_coords(lv_obj_get_parent(cancel), &oa);
        if (open && obj_shown(open) && lv_obj_get_parent(open) == lv_obj_get_parent(cancel)) { printf("check %s: FAIL Open still shown beside Cancel\n", name); return 1; }
        /* the whole strip lies inside the action row, left of Cancel: "Recording" isn't pushed out */
        lv_obj_t *rec = find_label(s, "Recording", false);
        lv_area_t ra, ca, sa;
        lv_obj_get_coords(rec, &ra); lv_obj_get_coords(cancel, &ca); lv_obj_get_coords(lv_obj_get_parent(lv_obj_get_parent(rec)), &sa);
        if (ra.x1 < sa.x1 || sa.x2 >= ca.x1 || ra.x1 < oa.x1) { printf("check %s: FAIL the strip doesn't fit (Recording x %d, strip %d-%d, Cancel x %d)\n", name, (int)ra.x1, (int)sa.x1, (int)sa.x2, (int)ca.x1); return 1; }
        r |= pv_run_checks(d, s, name, true, 0);
        /* Cancel: the recording is dropped, the draft stays, Open comes back */
        lv_obj_send_event(cancel, LV_EVENT_CLICKED, NULL); step(d, 400);
        if (s_voice_cancel_calls != 1 || s_pv_voice.phase != VOICE_IDLE || strcmp(field_text(s), "ab")) { printf("check %s: FAIL Cancel: %d cancels, draft \"%s\"\n", name, s_voice_cancel_calls, field_text(s)); return 1; }
        step(d, 400);
        if (obj_shown(cancel) || (rec && obj_shown(rec)) || mic_ring_red(s) || timers_with_period(100)) { printf("check %s: FAIL the strip, Cancel or the meter outlived the cancel\n", name); return 1; }
        if (!open || !obj_shown(open)) { printf("check %s: FAIL Open didn't come back\n", name); return 1; }
        /* the mic again, the mic to stop: transcribing; Cancel shows with the note and drops it */
        step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 100);
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 3000; step(d, 400); step(d, 400);
        int stops = s_voice_stop_calls;
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 400);
        if (s_voice_stop_calls != stops + 1 || s_pv_voice.phase != VOICE_TRANSCRIBING) { printf("check %s: FAIL the mic didn't stop\n", name); return 1; }
        step(d, 400);
        if (!obj_shown(cancel) || !find_label_has(s, "Transcribing on the Host", 0)) { printf("check %s: FAIL no Cancel while transcribing\n", name); return 1; }
        r |= pv_run_checks(d, s, "sessions-voice-cancel-touch-busy", true, 0);
        key_of(0, k0);
        lv_obj_send_event(cancel, LV_EVENT_CLICKED, NULL); step(d, 400);
        if (s_voice_cancel_calls != 2 || !s_pv_voice.cancelled || obj_shown(cancel)) { printf("check %s: FAIL Cancel while transcribing\n", name); return 1; }
        /* the Host answers; the service drops the text: nothing reaches the draft, no note, no paste */
        pv_voice_post(k0, VR_OUT_CANCELLED, "", false); step(d, 400); step(d, 400);
        if (strcmp(field_text(s), "ab") || s_paste_calls || s_note_activity_calls || find_label_has(s, "Check the text", 0)) {
            printf("check %s: FAIL something was applied after Cancel (draft \"%s\", %d pastes)\n", name, field_text(s), s_paste_calls); return 1;
        }
        /* review M5: the touch wording of the review note and of "too short" */
        step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 100);
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 3000; step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 400);
        pv_voice_post(k0, VR_OUT_TEXT, "Hello.", false); step(d, 400);
        lv_obj_t *rn = find_label(s, "Check the text, then tap Send", false);
        if (!rn || !obj_shown(rn) || find_label(s, "Check the text, then press Enter to send", false)) { printf("check %s: FAIL the review note isn't touch wording\n", name); return 1; }
        step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 100);
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 300; step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 400);
        pv_voice_post(k0, VR_OUT_TOO_SHORT, "", false); step(d, 400);
        if (!find_label(lv_layer_top(), "Too short. Tap the mic, speak, then tap it again.", false)) { printf("check %s: FAIL \"too short\" isn't touch wording\n", name); return 1; }
        printf("%s: the field brings the soft keyboard with the field and mic above it; the mic starts and stops; Cancel (64 px, in Open's room) drops a recording and a transcription; \"Tap the mic to stop\"; nothing applied\n", name);
        return r;
    }
    (void)why;
    fprintf(stderr, "preview: unknown sessions case %s\n", v);
    return 2;
}

/* The list hides interactive sessions: these map the stub's raw sessions to list rows. */
static int shown_n(void)
{
    int n = 0;
    for (int i = 0; i < s_preview_n; i++) n += s_preview_sessions[i].background;
    return n;
}
static int shown_index(int raw)
{
    int n = 0;
    for (int i = 0; i < raw; i++) n += s_preview_sessions[i].background;
    return n;
}

/* ------------------------------------------------------------ stop (Ctrl+X) */
static void pv_key(key_event_t ev);   /* the device's routing: a dialog takes every key, else the sink */
#define STOP_BODY "It stops running. Its conversation stays on the Host, and Open starts it again while it is listed here."
#define STOP_CHIP "\xe2\x8c\x83X Stop"
#define STOP_NOT_RUNNING "This session isn't running, so there is nothing to stop"
#define STOP_GONE "Not stopped: that session changed before you confirmed"
static bool stop_dialog_up(void) { return find_label(lv_layer_top(), STOP_BODY, false) != NULL; }
static void pv_ctrl_x(lv_display_t *d) { pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'x', .mods = MOD_CTRL }); step(d, 100); }
static void stop_answer(lv_display_t *d, const char *button) {
    lv_obj_t *b = find_clickable_shown(lv_layer_top(), button);
    assert(b);
    lv_obj_send_event(b, LV_EVENT_CLICKED, NULL); step(d, 400);
}
static int raw_of(const char *id) {
    for (int i = 0; i < s_preview_n; i++) if (!strcmp(s_preview_sessions[i].id, id)) return i;
    return -1;
}
static bool stop_output_shown(lv_obj_t *s) {
    return find_label_has(lv_layer_top(), PV_STOP_OUTPUT, 0) || find_label_has(s, PV_STOP_OUTPUT, 0);
}
static void stop_cmd_of(const char *id, char *out, size_t cap) { snprintf(out, cap, "~/.local/bin/claude stop %s", id); }

/* sessions stop: Ctrl+X asks first (the session's name, Cancel first; Enter and Esc cancel), a confirmed stop runs
   exactly `claude stop <id>` for the session the confirm named and not the one selected when it is answered, then
   toasts and refreshes; a stopped session has nothing to stop; a failure shows the exit status only; the Stop button
   is the touch path to the same confirm; Ctrl+X is inert while recording and while the rows are inert.
   sessions stop-stale: the list changes under the confirm: a session that left the list or stopped is refused, and
   one that a re-sort moved is still the one stopped. */
static int stop_case(lv_display_t *d, lv_obj_t *s, lv_obj_t *list, const char *name, const char *v) {
    int r = 0;
    char want[96], cmd_a[96];
    if (!strcmp(v, "stop")) {
        const cc_session_t a = s_preview_sessions[0];   /* "needs input", selected */
        assert(selected_index(list) == 0 && a.background && a.live);
        stop_cmd_of(a.id, cmd_a, sizeof(cmd_a));
        snprintf(want, sizeof(want), "Stop %s?", a.name);
        lv_obj_t *chip = find_clickable_shown(s, STOP_CHIP);
        if (!chip || !obj_shown(chip)) { printf("check %s: FAIL no Stop button for a running session\n", name); return 1; }
        /* Ctrl+X: the confirm, named, Cancel first and the red Stop after it */
        pv_ctrl_x(d);
        if (!stop_dialog_up() || !find_label(lv_layer_top(), want, false)) { printf("check %s: FAIL no \"%s\" confirm\n", name, want); return 1; }
        lv_obj_t *cancel = find_clickable_shown(lv_layer_top(), "Cancel"), *stop = find_clickable_shown(lv_layer_top(), "Stop");
        assert(cancel && stop && lv_obj_get_parent(cancel) == lv_obj_get_parent(stop));
        lv_area_t ca, sa; lv_obj_get_coords(cancel, &ca); lv_obj_get_coords(stop, &sa);
        if (lv_obj_get_index(cancel) > lv_obj_get_index(stop) || ca.x2 >= sa.x1) { printf("check %s: FAIL Cancel isn't first\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);   /* the confirm over the rows (the scrim tints the amber) */
        /* Enter on the dialog cancels: Cancel has the first focus */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 100);
        if (stop_dialog_up() || s_stop_calls) { printf("check %s: FAIL Enter on the confirm didn't cancel (%d stops)\n", name, s_stop_calls); return 1; }
        /* Esc too, and Sessions stays */
        pv_ctrl_x(d); assert(stop_dialog_up());
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 100);
        if (stop_dialog_up() || s_stop_calls || !ui_nav_is_top(ui_sessions_create)) { printf("check %s: FAIL Esc on the confirm\n", name); return 1; }
        /* stop review I1: no arrow, Space or second Ctrl+X confirms; only Tab reaches Stop */
        static const struct { special_key_t k1, k2, k3; } seqs[] = {
            { SK_UP, SK_ENTER, SK_NONE }, { SK_LEFT, SK_ENTER, SK_NONE }, { SK_DOWN, SK_DOWN, SK_ENTER },
            { SK_RIGHT, SK_ENTER, SK_NONE }, { SK_UP, SK_UP, SK_ENTER },
        };
        for (size_t q = 0; q < sizeof(seqs) / sizeof(seqs[0]); q++) {
            pv_ctrl_x(d); assert(stop_dialog_up());
            special_key_t ks[3] = { seqs[q].k1, seqs[q].k2, seqs[q].k3 };
            for (int k = 0; k < 3 && ks[k] != SK_NONE; k++) { pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = ks[k] }); step(d, 50); }
            if (stop_dialog_up() || s_stop_calls) { printf("check %s: FAIL key sequence %d on the confirm stopped it or left it open\n", name, (int)q); return 1; }
        }
        pv_ctrl_x(d); assert(stop_dialog_up());
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ' ' }); step(d, 50);
        if (stop_dialog_up() || s_stop_calls) { printf("check %s: FAIL Space on the confirm\n", name); return 1; }
        pv_ctrl_x(d); assert(stop_dialog_up());
        pv_ctrl_x(d);   /* a second Ctrl+X: eaten by the dialog */
        if (!stop_dialog_up() || s_stop_calls) { printf("check %s: FAIL a second Ctrl+X on the confirm\n", name); return 1; }
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 50);
        assert(!stop_dialog_up() && s_stop_calls == 0);
        /* confirmed after the selection moved (a stand-in for anything that moves it while the confirm is up) */
        pv_ctrl_x(d); assert(stop_dialog_up());
        assert(sink_sk(SK_DOWN)); step(d, 400);
        assert(selected_index(list) == 1);
        /* by keyboard: Tab to Stop (from Cancel, which has the focus), then Enter */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 50);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 400);
        if (s_stop_calls != 1 || strcmp(s_stop_cmd, cmd_a)) { printf("check %s: FAIL stop ran \"%s\" (%d), want \"%s\"\n", name, s_stop_cmd, s_stop_calls, cmd_a); return 1; }
        if (s_poll_now_calls != 1 || !find_label(lv_layer_top(), "Stopped", false) || stop_output_shown(s)) { printf("check %s: FAIL no toast or refresh after the stop\n", name); return 1; }
        assert(selected_index(list) == 1);
        /* the refresh shows it stopped: no Stop button for it, and Ctrl+X says so without a confirm */
        preview_stopped(raw_of(a.id)); step(d, 400);
        assert(sink_sk(SK_UP)); step(d, 400);
        assert(selected_index(list) == 0 && find_label_x(s, a.name, 440));
        if (obj_shown(chip)) { printf("check %s: FAIL a Stop button for a stopped session\n", name); return 1; }
        pv_ctrl_x(d);
        if (stop_dialog_up() || s_stop_calls != 1 || !find_label(lv_layer_top(), STOP_NOT_RUNNING, false)) { printf("check %s: FAIL Ctrl+X on a stopped session\n", name); return 1; }
        /* a failure: the exit status, never the output, and no refresh */
        const cc_session_t b = s_preview_sessions[1];
        assert(sink_sk(SK_DOWN)); step(d, 400);
        assert(find_label_x(s, b.name, 440));
        s_pv_stop_rc = 1;
        pv_ctrl_x(d); stop_answer(d, "Stop");
        s_pv_stop_rc = 0;
        char cmd_b[96]; stop_cmd_of(b.id, cmd_b, sizeof(cmd_b));
        if (s_stop_calls != 2 || strcmp(s_stop_cmd, cmd_b) || s_poll_now_calls != 1) { printf("check %s: FAIL the second stop ran \"%s\"\n", name, s_stop_cmd); return 1; }
        if (!find_label(lv_layer_top(), "Stop failed (exit 1)", false) || stop_output_shown(s)) { printf("check %s: FAIL the failure toast\n", name); return 1; }
        r |= pv_run_checks(d, s, "sessions-stop-failed", true, 0);
        /* touch: the Stop button opens the same confirm */
        lv_obj_send_event(chip, LV_EVENT_CLICKED, NULL); step(d, 100);
        snprintf(want, sizeof(want), "Stop %s?", b.name);
        if (!find_label(lv_layer_top(), want, false)) { printf("check %s: FAIL the Stop button's confirm\n", name); return 1; }
        stop_answer(d, "Cancel");
        assert(!stop_dialog_up() && s_stop_calls == 2);
        /* recording: the Stop button hides and Ctrl+X does nothing (the keys are eaten) */
        step(d, 4000);   /* the toasts go */
        pv_record(d, 2000, 40);
        pv_ctrl_x(d);
        if (obj_shown(chip) || stop_dialog_up() || find_label(lv_layer_top(), STOP_NOT_RUNNING, false)) { printf("check %s: FAIL Ctrl+X or Stop while recording\n", name); return 1; }
        assert(sink_sk(SK_ESC)); step(d, 400); step(d, 400);
        if (!obj_shown(chip)) { printf("check %s: FAIL Stop didn't come back after the recording\n", name); return 1; }
        /* inert rows (no Wi-Fi): Ctrl+X does nothing */
        s_rc_preview = true; step(d, 400);
        pv_ctrl_x(d);
        bool acted = stop_dialog_up() || find_label(lv_layer_top(), STOP_NOT_RUNNING, false);
        s_rc_preview = false; step(d, 400);
        if (acted) { printf("check %s: FAIL Ctrl+X acted on inert rows\n", name); return 1; }
        assert(s_stop_calls == 2);
        r |= pv_run_checks(d, s, "sessions-stop-after", true, 0);
        return r;
    }
    if (!strcmp(v, "stop-stale")) {
        /* the confirmed session left the list before the answer: refused, whatever is selected now */
        const cc_session_t a = s_preview_sessions[0];
        pv_ctrl_x(d);
        snprintf(want, sizeof(want), "Stop %s?", a.name);
        assert(find_label(lv_layer_top(), want, false));
        preview_drop(raw_of(a.id)); step(d, 400);
        const cc_session_t b = s_preview_sessions[0];   /* "needs approval", now selected in a's place */
        assert(selected_index(list) == 0 && find_label_x(s, b.name, 440));
        stop_answer(d, "Stop");
        if (s_stop_calls || !find_label(lv_layer_top(), STOP_GONE, false)) { printf("check %s: FAIL a stop for a session that left the list (\"%s\")\n", name, s_stop_cmd); return 1; }
        /* a re-sort moves the confirmed session to the end: it is still the one stopped, not the row in its old place */
        step(d, 3000);
        pv_ctrl_x(d);
        snprintf(want, sizeof(want), "Stop %s?", b.name);
        assert(find_label(lv_layer_top(), want, false));
        preview_resort_to_last(raw_of(b.id)); step(d, 400);
        assert(selected_index(list) == shown_n() - 1);
        /* and the selection moves off it, onto the row that took its old place (row 0): stopping the selected row, or
           the row index captured at Ctrl+X, would both stop that one */
        assert(sink_sk(SK_HOME)); step(d, 400);
        assert(selected_index(list) == 0 && !find_label_x(s, b.name, 440));
        stop_answer(d, "Stop");
        char cmd_b[96]; stop_cmd_of(b.id, cmd_b, sizeof(cmd_b));
        if (s_stop_calls != 1 || strcmp(s_stop_cmd, cmd_b)) { printf("check %s: FAIL after a re-sort it ran \"%s\", want \"%s\"\n", name, s_stop_cmd, cmd_b); return 1; }
        /* the session stopped (lost its pid) before the answer: refused */
        step(d, 3000);
        assert(sink_sk(SK_HOME)); step(d, 400);
        int c = raw_of(s_preview_sessions[0].background ? s_preview_sessions[0].id : s_preview_sessions[1].id);
        const cc_session_t cs = s_preview_sessions[c];
        assert(find_label_x(s, cs.name, 440) && cs.live);
        pv_ctrl_x(d);
        snprintf(want, sizeof(want), "Stop %s?", cs.name);
        assert(find_label(lv_layer_top(), want, false));
        preview_stopped(c); step(d, 400);
        stop_answer(d, "Stop");
        if (s_stop_calls != 1 || !find_label(lv_layer_top(), STOP_NOT_RUNNING, false)) { printf("check %s: FAIL a stop for a session that stopped first\n", name); return 1; }
        return pv_run_checks(d, s, name, true, 0);
    }
    if (!strcmp(v, "stop-pinned")) {
        /* stop review I2: a session stopped here stays listed past the 4-finished limit. agents_16: make the 4 sessions
           just newer than log-rotation finished, then stop log-rotation (older than all 4). */
        int t = raw_of("b000000c");   /* log-rotation, idle */
        assert(t > 4);
        for (int i = t - 4; i < t; i++) { s_preview_sessions[i].state = CC_DONE; s_preview_sessions[i].live = false; }
        preview_poll(); step(d, 400);
        lv_obj_t *row = row_named(s, "log-rotation");
        assert(row);
        lv_obj_send_event(row, LV_EVENT_PRESSED, NULL); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(find_label_x(s, "log-rotation", 440));
        pv_ctrl_x(d); assert(stop_dialog_up());
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 50);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 400);
        if (s_stop_calls != 1 || strcmp(s_stop_cmd, "~/.local/bin/claude stop b000000c")) { printf("check %s: FAIL stop ran \"%s\"\n", name, s_stop_cmd); return 1; }
        preview_stopped(t); step(d, 400);
        step(d, 400);
        row = row_named(s, "log-rotation");
        if (!row || !obj_shown(row) || !row_selected(row) || !find_label_x(s, "log-rotation", 440)) { printf("check %s: FAIL the stopped session left the list\n", name); return 1; }
        if (!find_label_x(s, "This session has stopped. Open it to start it again.", 440) || !find_clickable_shown(s, "\xe2\x8c\x83O Open")) { printf("check %s: FAIL it can't be opened\n", name); return 1; }
        /* the limit still holds for the rest: the older finished ones (release-notes, dead-code, old-spike) are gone */
        if (row_named(s, "release-notes") && obj_shown(row_named(s, "release-notes"))) { printf("check %s: FAIL the finished limit\n", name); return 1; }
        /* another poll keeps it */
        preview_poll(); step(d, 400);
        assert(row_named(s, "log-rotation") && obj_shown(row_named(s, "log-rotation")));
        return pv_run_checks(d, s, name, true, 0);
    }
    fprintf(stderr, "preview: unknown sessions case %s\n", v);
    return 2;
}

static int sessions_case(lv_display_t *d, const char *v) {
    char name[64];
    snprintf(name, sizeof(name), !strcmp(v, "ctrlk") ? "ctrlk" : "sessions-%s", v);
    bool needs = false;
    if (!strcmp(v, "working")) setenv("PREVIEW_FIXTURE", "agents_working.json", 1);
    if (!strcmp(v, "loading")) setenv("PREVIEW_FIXTURE", "loading", 1);
    if (!strcmp(v, "empty")) setenv("PREVIEW_FIXTURE", "none", 1);
    if (!strcmp(v, "16") || !strcmp(v, "resort-scroll") || !strcmp(v, "stop-pinned")) setenv("PREVIEW_FIXTURE", "agents_16.json", 1);
    if (!strcmp(v, "long") || !strcmp(v, "interactive")) { setenv("PREVIEW_FIXTURE", "agents_long.json", 1); setenv("PREVIEW_LOG", "long", 1); }
    if (!strcmp(v, "screen-log")) setenv("PREVIEW_LOG", "screen:logs_numbered.bin", 1);
    if (!strcmp(v, "screen-dialog")) setenv("PREVIEW_LOG", "screen:logs_question.bin", 1);
    if (!strcmp(v, "no-wifi")) setenv("PREVIEW_WIFI", "0", 1);
    if (!strcmp(v, "no-host")) setenv("PREVIEW_HOST", "0", 1);
    if (!strcmp(v, "not-found")) setenv("PREVIEW_FAIL", "notfound", 1);
    if (!strcmp(v, "new-key")) setenv("PREVIEW_FAIL", "newkey", 1);
    if (!strcmp(v, "low-battery")) setenv("PREVIEW_BAT", "14", 1);
    if (!strcmp(v, "reconnecting")) s_rc_preview = true;
    if (!strcmp(v, "voice-cancel-touch")) setenv("PREVIEW_KBD", "0", 1);   /* touch alone */
    if (!strcmp(v, "voice-kbd-banner")) { setenv("PREVIEW_KBD", "0", 1); setenv("PREVIEW_BAT", "14", 1); }   /* review I1 */
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
        !strcmp(v, "tap-resort") || !strcmp(v, "resort-scroll") || !strncmp(v, "voice-", 6)) needs = true;
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
        assert(find_label(s, "Reconnecting to Host\xe2\x80\xa6", false));
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
        assert(find_label(s, "Host not found at studio-host.local", false) && find_clickable(s, "Host settings"));
    } else if (!strcmp(v, "new-key")) {
        /* polish 1: the banner points at Settings › Host, where the key can be trusted; its chip goes there */
        const char *want = "The Host has a new host key. Check and trust it in Settings \xe2\x80\xba Host.";
        if (!find_label(s, want, false) || !find_clickable(s, "Host settings")) { printf("check %s: FAIL the new-key banner\n", name); return 1; }
        lv_obj_send_event(find_clickable(s, "Host settings"), LV_EVENT_CLICKED, NULL); step(d, 200);
        if (!ui_nav_is_top(ui_settings_create) || !find_clickable(lv_screen_active(), "Trust this key")) {
            printf("check %s: FAIL Host settings doesn't reach Trust this key\n", name); return 1;
        }
        ui_nav_back(); step(d, 400);
        s = lv_screen_active();
    } else if (!strcmp(v, "no-host")) {
        lv_obj_t *b = find_clickable(s, "Set up Host");
        assert(b && obj_shown(b));
        assert(!find_shown(s, &lv_textarea_class));
        assert(!row0 || !obj_shown(row0));
    } else if (!strcmp(v, "empty")) {
        assert(find_label(s, "No sessions on this Host. Start one with `claude --bg`.", false));
    } else if (!strcmp(v, "interactive")) {
        /* a plain `claude` in a terminal can't be opened from the Tab5, so it isn't listed at all */
        int raw = 0;
        for (int i = 0; i < s_preview_n; i++) raw += !s_preview_sessions[i].background;
        assert(raw > 0);   /* the fixture has one */
        assert(!find_label_has(list, "on Host \xc2\xb7 ", 0));
        assert(!find_label(s, "Running in a terminal on the Host. Start sessions with claude --bg to open them here.", false));
    } else if (!strcmp(v, "sent")) {
        assert(sink_ch('2', 0)); step(d, 400);
        assert(!strcmp(field_text(s), "2"));
        assert(sink_sk(SK_ENTER)); step(d, 400);
        assert(s_paste_calls == 1);
        assert(find_label(s, "Sent \xe2\x9c\x93", false));
        assert(!strcmp(field_text(s), ""));   /* the draft is cleared after a send */
    } else if (!strcmp(v, "screen-log") || !strcmp(v, "screen-dialog")) {
        /* the pane shows the conversation read off the rendered screen, never the footer or the status line */
        static const char *chrome[] = { "tokens |", "auto mode on", "shift+tab", "agents", "/effort", "Claude Code v",
                                        "Cogitated", "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" };
        for (size_t i = 0; i < sizeof(chrome) / sizeof(chrome[0]); i++)
            if (find_label_has(s, chrome[i], 0)) { printf("check %s: FAIL the pane shows \"%s\"\n", name, chrome[i]); return 1; }
        bool dialog = !strcmp(v, "screen-dialog");
        const char *want = dialog ? "Which colour do you prefer for the test banner?" : "Which test fruit do you want?";
        if (!find_label_has(s, want, 440) || !find_label_has(s, dialog ? "5. Chat about this" : "3. Plum", 440)) {
            printf("check %s: FAIL the pane lacks the conversation\n", name); return 1;
        }
        /* a typed answer picks from a plain-text list; a selector dialog can't be answered by typing */
        bool picks = find_label(s, "1\xe2\x80\x93" "3 picks an option", false) != NULL;
        if (picks == dialog) { printf("check %s: FAIL the options line\n", name); return 1; }
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
        for (int i = 0; i < shown_index(k); i++) { assert(sink_sk(SK_DOWN)); step(d, 400); }
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
        assert(row_selected(row_named(s, bname)) && selected_index(list) == shown_n() - 1);
    } else if (!strcmp(v, "tap-resort")) {
        /* review I1: a tap belongs to the session under the finger at the press. Press the last row, then a
           poll moves the selected session (row 0) into that slot before the release: nothing opens, and the
           selection stays on the session it was on. */
        int last = shown_n() - 1;
        char sel[96]; snprintf(sel, sizeof(sel), "%s", s_preview_sessions[0].name);
        lv_obj_t *r = lv_obj_get_child(list, last);
        lv_obj_send_event(r, LV_EVENT_PRESSED, NULL);
        preview_resort_to_last(0); step(d, 400);
        assert(row_selected(row_named(s, sel)) && selected_index(list) == last);
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); step(d, 400);
        assert(s_check_calls == 0 && ui_nav_is_top(ui_sessions_create));
        assert(row_selected(row_named(s, sel)) && find_label_x(s, sel, 440));
        /* a whole tap on the selected row still opens it (the check runs; the stub session ends at once) */
        lv_obj_send_event(r, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); step(d, 400);
        step(d, 50);   /* the take-over's pump sees the stub's end and goes back */
        assert(!ui_terminal_is_top() && ui_nav_is_top(ui_sessions_create));
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
        assert(s_voice_start_calls == 1);       /* Plan 5: it starts voice input */
        assert(sink_sk(SK_ESC) && s_voice_cancel_calls == 1);
        assert(!strcmp(field_text(s), "cd"));   /* Esc while recording cancels it and keeps the draft */
        step(d, 3100);
        assert(sink_ch('p', MOD_CTRL)); step(d, 400);
        assert(ui_nav_is_top(ui_settings_create));
        ui_nav_back(); step(d, 400);
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_sessions_create) && !strcmp(field_text(s), "cd"));   /* the draft outlives the visit */
    } else if (!strncmp(v, "voice-", 6)) {
        return voice_case(d, s, list, name, v);
    } else if (!strcmp(v, "stop") || !strcmp(v, "stop-stale") || !strcmp(v, "stop-pinned")) {
        return stop_case(d, s, list, name, v);
    } else if (!strcmp(v, "ctrlk")) {
        assert(sink_ch('k', MOD_CTRL) && ui_sessions_keys_overlay_shown());
        step(d, 100);
        assert(find_label(s, "voice input: press to start, again to stop", false));
        assert(find_label(s, "Ctrl+X", false) && find_label(s, "stop the selected session", false));
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
        /* M6: a first line that isn't a key, or is longer than the setting holds, is refused whole */
        char before[sizeof(g_settings.ts_auth_key)];
        strcpy(before, g_settings.ts_auth_key);
        setenv("PREVIEW_SD", "bad", 1);
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Load from SD card");
        assert(find_label(top, "The key file doesn't hold a Tailscale auth key", false) && !strcmp(before, g_settings.ts_auth_key));
        setenv("PREVIEW_SD", "long", 1);
        click_label(d, settings_panel(s), "Auth key"); click_label(d, top, "Load from SD card");
        assert(find_label(top, "The key in the file is too long", false) && !strcmp(before, g_settings.ts_auth_key));
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
        /* T8-13: without a key the body doesn't promise the saved key */
        bool extra = find_label(top, "Removes this device's tailnet identity. You'll need a new auth key to register it again.", false) != NULL;
        assert(had_key == (find_label_has(top, "saved auth key", 0) != NULL));
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
        /* the current network: a sheet with Other networks first, then Disconnect and Forget, both confirmed */
        assert(sheet_rows("Clientre Wi-Fi") == 3 && find_label(top, "Disconnect", false));
        *r |= pv_run_checks(d, s, name, false, 0);
        /* review M6: Enter on the fresh sheet changes nothing: it opens the list, asks nothing, disconnects nothing */
        press_key(SK_ENTER, 0, false); step(d, 200);
        assert(s_wifi_disconnect_calls == 0 && s_wifi_forget_calls == 0 && !ui_keyboard_owns_input());
        assert(ui_nav_is_top(ui_settings_sub_create));
        ui_nav_back(); step(d, 200);
        s = lv_screen_active();
        click_label(d, settings_panel(s), "Clientre Wi-Fi");
        assert(sheet_rows("Clientre Wi-Fi") == 3);
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

/* network-live (I1): Status, Peers and the switch follow Tailscale events in place; the slot goes with the panel. */
/* picker-fresh (M1): the Host picker when the saved Host names no entry (`companion server <name>`), and the time
   zone picker with a custom rule: a bare Enter changes nothing (no Host switch, so no lost pin carry-over and no
   deny-list growth), and a deliberate pick still works. */
static int picker_fresh_case(lv_display_t *d) {
    const char *name = "picker-fresh";
    preview_sessions_load();
    s_pv_server_name = "console-only-name";
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_HOST); step(d, 200);
    lv_obj_t *s = lv_screen_active();
    { lv_obj_t *row = row_labeled(settings_panel(s), "Host"); assert(row); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 200); }
    if (sheet_rows("Host") < 3) { printf("check %s: FAIL no Host picker\n", name); return 1; }
    int calls = s_set_server_calls;
    press_key(SK_ENTER, 0, false); step(d, 200);
    if (s_set_server_calls != calls || sheet_rows("Host") < 3) { printf("check %s: FAIL a bare Enter on the Host picker switched the Host (to \"%s\")\n", name, s_set_server_name); return 1; }
    int r = pv_run_checks(d, s, "picker-fresh-host", false, 0);
    press_key(SK_ESC, 0, false); step(d, 200);
    s_pv_server_name = NULL;
    strcpy(g_settings.tz_rule, "XYZ-3");   /* a custom rule: no preset row is current */
    ui_settings_open(UI_SET_DISPLAY); step(d, 200);
    s = lv_screen_active();
    { lv_obj_t *row = row_labeled(settings_panel(s), "Time zone"); assert(row); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 200); }
    if (sheet_rows("Time zone") < 2) { printf("check %s: FAIL no time zone picker\n", name); return 1; }
    press_key(SK_ENTER, 0, false); step(d, 200);
    if (strcmp(g_settings.tz_rule, "XYZ-3")) { printf("check %s: FAIL a bare Enter on the time zone picker set \"%s\"\n", name, g_settings.tz_rule); return 1; }
    press_key(SK_DOWN, 0, false); press_key(SK_ENTER, 0, false); step(d, 200);   /* a deliberate pick */
    if (!strcmp(g_settings.tz_rule, "XYZ-3")) { printf("check %s: FAIL a deliberate pick didn't apply\n", name); return 1; }
    printf("%s: a bare Enter on a picker with no current row picks nothing (Host, time zone); a move then Enter picks\n", name);
    return r;
}

static int network_live_case(lv_display_t *d) {
    setenv("PREVIEW_TS", "1", 1);
    g_settings.ts_enable = 1;
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_NETWORK); step(d, 200);
    lv_obj_t *s = lv_screen_active(), *panel = settings_panel(s);
    if (!s_ts_cb) { printf("check network-live: FAIL the Network panel doesn't follow Tailscale\n"); return 1; }
    lv_obj_t *row = row_labeled(panel, "Status"), *v = ui_srow_value(row);
    lv_obj_t *peers = ui_srow_value(row_labeled(panel, "Peers"));
    assert(!strcmp(lv_label_get_text(v), "connecting") && !strcmp(lv_label_get_text(peers), "0 online"));
    setenv("PREVIEW_TS", "2", 1);
    int reads = s_peer_reads;
    s_ts_cb(s_ts_cb_user); s_ts_cb(s_ts_cb_user); s_ts_cb(s_ts_cb_user); step(d, 200);
    if (row_labeled(panel, "Status") != row || strcmp(lv_label_get_text(v), "connected \xc2\xb7 100.101.102.103 \xc2\xb7 DERP sea \xc2\xb7 2 peers online")) {
        printf("check network-live: FAIL Status reads \"%s\" after the event\n", lv_label_get_text(v)); return 1;
    }
    assert(!strcmp(lv_label_get_text(peers), "2 online"));
    assert(s_peer_reads - reads <= 4);   /* three events, one update (two peer walks: Status and Peers) */
    /* M2 (I2's bounded wait): an event whose ui_async times out is dropped, and the next one still updates */
    pv_lvgl_lock_fails = 1;
    s_ts_cb(s_ts_cb_user); step(d, 200);
    if (pv_lvgl_lock_fails) { printf("check network-live: FAIL the event didn't post\n"); return 1; }
    setenv("PREVIEW_TS", "3", 1);
    s_ts_cb(s_ts_cb_user); step(d, 200);
    if (strcmp(lv_label_get_text(v), "Auth key expired: enter a new one") ||
        !lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(UI_TK_RED))) {
        printf("check network-live: FAIL the error isn't shown in red (\"%s\")\n", lv_label_get_text(v)); return 1;
    }
    int r = pv_run_checks(d, s, "network-live", false, 0);
    /* the switch turned off elsewhere (`ts off`): the row follows, back in the muted tone */
    g_settings.ts_enable = 0;
    setenv("PREVIEW_TS", "0", 1);
    s_ts_cb(s_ts_cb_user); step(d, 200);
    lv_obj_t *sw_row = row_labeled(panel, "Tailscale");
    assert(find_label(sw_row, "off \xc2\xb7 saved", false) && !lv_obj_has_state(ui_srow_value(sw_row), LV_STATE_CHECKED));
    assert(!strncmp(lv_label_get_text(v), "Off: doesn't run", 16) &&
           lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(UI_TK_MUTED)));
    lv_obj_send_event(rail_row(s, UI_SET_POWER), LV_EVENT_CLICKED, NULL); step(d, 200);
    if (s_ts_cb) { printf("check network-live: FAIL the slot is set after a rail click away\n"); return 1; }
    printf("network-live: Status, Peers and the switch updated in place; slot NULL after the rail click\n");
    return r;
}

/* net-sub-open (M7): `ui net <sub>` with Settings on top switches section; one Back reaches Settings, two Sessions. */
static int net_sub_open_case(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_HOST); step(d, 200);
    ui_settings_open_net_sub(UI_NET_SUB_PEERS); step(d, 200);
    assert(ui_nav_is_top(ui_settings_sub_create));
    ui_nav_back(); step(d, 200);
    assert(ui_nav_is_top(ui_settings_create) && !strcmp(lv_label_get_text(lv_obj_get_child(settings_panel(lv_screen_active()), 0)), "Network"));
    ui_nav_back(); step(d, 200);
    if (!ui_nav_is_top(ui_sessions_create)) { printf("check net-sub-open: FAIL a second Settings entry was stacked\n"); return 1; }
    printf("check net-sub-open: ok\n");
    return 0;
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
        /* M1: the saved table changes under the page (a join saved a network first): a row still means its SSID */
        memmove(&s_preview_saved[1], &s_preview_saved[0], sizeof(s_preview_saved[0]) * (size_t)s_preview_saved_n);
        snprintf(s_preview_saved[0].ssid, sizeof(s_preview_saved[0].ssid), "Attic");
        snprintf(s_preview_saved[0].pass, sizeof(s_preview_saved[0].pass), "preview-pass-3");
        s_preview_saved_n++;
        click_label(d, body, "Cabin");
        if (s_wifi_connect_calls != 4 || strcmp(s_wifi_connect_ssid, "Cabin")) {
            printf("check %s: FAIL a tap on Cabin joined \"%s\"\n", name, s_wifi_connect_ssid); return 1;
        }
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
        assert(s_wifi_connect_calls == 5 && !strcmp(s_wifi_connect_ssid, "Hidden lab"));
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
    pv_lvgl_lock_fails = 1;   /* M2: this event's ui_async times out (I2): dropped */
    s_ts_cb(s_ts_cb_user); step(d, 200);
    reads = s_peer_reads;
    s_ts_cb(s_ts_cb_user); step(d, 200);
    if (pv_lvgl_lock_fails || s_peer_reads != reads + 1) { printf("check %s: FAIL Peers stopped following Tailscale after a failed ui_async\n", name); return 1; }
    if (!strcmp(variant, "deleted-event")) {
        ts_event_cb_t old = s_ts_cb; void *old_user = s_ts_cb_user;
        ui_nav_back(); step(d, 200);
        lv_obj_t *now = lv_screen_active();
        /* I1: the slot is now Settings › Network's (its Status row follows Tailscale), never Peers' */
        if (s_ts_cb == old || !s_ts_cb) { printf("check %s: FAIL the Tailscale slot after Peers went: %s\n", name, s_ts_cb ? "Peers'" : "empty"); return 1; }
        reads = s_peer_reads;
        old(old_user); step(d, 200);   /* a late event from MicroLink's task: nothing happens */
        if (lv_screen_active() != now || s_peer_reads != reads) { printf("check %s: FAIL a stale event acted\n", name); return 1; }
        assert(ui_nav_is_top(ui_settings_create));
        r = pv_run_checks(d, now, name, false, 0);
        lv_obj_send_event(rail_row(now, UI_SET_HOST), LV_EVENT_CLICKED, NULL); step(d, 200);
        if (s_ts_cb) { printf("check %s: FAIL the Tailscale slot is set after leaving Network\n", name); return 1; }
        printf("%s: Peers' callback gone after back; a stale event did nothing; slot NULL off Network\n", name);
        return r;
    }
    r |= pv_run_checks(d, s, name, false, 0);
    click_label(d, body, "preview-host-a");
    assert(sheet_rows("preview-host-a") == 1 && find_label(top, "Add as SSH host", false));
    r |= pv_run_checks(d, s, "settings-sub-peers-sheet", false, 0);
    click_label(d, top, "Add as SSH host");   /* Task 9: the host form, prefilled, over Terminal */
    lv_obj_t *f = lv_screen_active();
    if (!ui_nav_is_top(ui_host_form_create) || !find_label(f, "preview-host-a", false) || !find_label(f, "100.64.0.11", false)) {
        printf("check %s: FAIL Add as SSH host didn't open the prefilled form\n", name); return 1;
    }
    ui_nav_back(); step(d, 200);
    assert(ui_nav_is_top(ui_hosts_create));
    lv_obj_clean(top);
    return r;
}

/* ------------------------------------------------------------ Plan 6: firmware updates */
/* settings 5 (About): the Update firmware row for each PREVIEW_FW_CHECK, the Previous version row, and the two
   confirms (PREVIEW_CLICK). True when it ran its own checks (r set). */
static int timer_count_300(void);
/* A label under o that shows want, whole or cut with LVGL's dots (a long value in a 64 px row). */
static lv_obj_t *label_shows(lv_obj_t *o, const char *want) {
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *g = lv_label_get_text(o);
        size_t gl = strlen(g);
        if (!strcmp(g, want) || (gl > 3 && !strcmp(g + gl - 3, "...") && !strncmp(g, want, gl - 3))) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) { lv_obj_t *r = label_shows(lv_obj_get_child(o, i), want); if (r) return r; }
    return NULL;
}
static void pv_key(key_event_t ev);
static void tap(lv_display_t *d, lv_indev_t *in, int x, int y);
static bool pv_fw_about(lv_display_t *d, lv_obj_t *s, const char *click, const char *name, int *r) {
    lv_obj_t *panel = settings_panel(s), *top = lv_layer_top();
    const char *m = pv_fw_mode();
    if (!click[0] && !env_int("PREVIEW_FW_LAYOUT", 1)) {
        /* round 2, N8: an old partition table has its own diagnosis, not "Rollback off" */
        lv_obj_t *row = row_labeled(panel, "Update firmware");
        lv_obj_t *v = row ? ui_srow_value(row) : NULL;
        if (!v || strcmp(lv_label_get_text(v), "Old partition table") || !label_red(v) || s_fw_check_calls ||
            find_label_has(panel, "Rollback off", 0)) {
            printf("check %s: FAIL Old partition table not shown, or a check ran\n", name); *r = 1; return true;
        }
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 200);
        if (!toast_whole(top, tr(STR_FW_ERR_LAYOUT)) || s_fw_check_calls) {
            printf("check %s: FAIL a tap didn't refuse with the layout copy\n", name); *r = 1; return true;
        }
        lv_obj_clean(top);
        return false;
    }
    if (!click[0] && !env_int("PREVIEW_FW_ROLLBACK", 1)) {
        /* fix C1: Rollback off on the row (red) and the Version note; no check; a tap refuses */
        lv_obj_t *row = row_labeled(panel, "Update firmware");
        lv_obj_t *v = row ? ui_srow_value(row) : NULL;
        if (!v || strcmp(lv_label_get_text(v), "Rollback off") || !label_red(v) || s_fw_check_calls ||
            !find_label_has(row_labeled(panel, "Version"), "Rollback off", 0)) {
            printf("check %s: FAIL Rollback off not shown, or a check ran\n", name); *r = 1; return true;
        }
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 200);
        if (!toast_whole(top, tr(STR_FW_ERR_NO_ROLLBACK)) || s_fw_check_calls) {
            printf("check %s: FAIL a tap didn't refuse (%d checks)\n", name, s_fw_check_calls); *r = 1; return true;
        }
        lv_obj_clean(top);
        return false;
    }
    if (!click[0]) {
        lv_obj_t *row = row_labeled(panel, "Update firmware");
        if (!row) { printf("check %s: FAIL no Update firmware row\n", name); *r = 1; return true; }
        lv_obj_t *v = ui_srow_value(row);
        const char *want = !strcmp(m, "checking") ? "Checking\xe2\x80\xa6" : !strcmp(m, "uptodate") ? "Up to date"
                         : !strcmp(m, "newer") ? "1.6.1+3f2a9c1 available" : !strcmp(m, "older") ? "1.6.1+3f2a9c1 (older) available"
                         : !strcmp(m, "nofile") ? "No firmware file on the Host at the firmware path (Settings \xe2\x80\xba Host)."
                         : !strcmp(m, "notapp") ? "That file isn't Claude Companion firmware for the Tab5." : "Check";
        /* an error longer than the row is cut with dots inside it (LVGL writes the dots into the text) */
        const char *got = v ? lv_label_get_text(v) : "";
        size_t gl = strlen(got);
        bool cut = gl > 3 && !strcmp(got + gl - 3, "...") && !strncmp(got, want, gl - 3);
        if (!v || (strcmp(got, want) && !cut)) {
            printf("check %s: FAIL the row reads \"%s\", not \"%s\"\n", name, v ? lv_label_get_text(v) : "(none)", want); *r = 1; return true;
        }
        bool err = !strcmp(m, "nofile") || !strcmp(m, "notapp");
        if (err != label_red(v)) { printf("check %s: FAIL the row's tone (red %d)\n", name, label_red(v)); *r = 1; return true; }
        if (!err && (!strcmp(m, "newer") || !strcmp(m, "older")) != lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(UI_TK_FG))) {
            printf("check %s: FAIL an available build reads fg, the rest muted\n", name); *r = 1; return true;
        }
        /* shown with no cached check, About runs one (every fill, while none is cached); a fresh one is reused */
        if (strcmp(m, "checking") && (!strcmp(m, "none") ? s_fw_check_calls < 1 : s_fw_check_calls != 0)) {
            printf("check %s: FAIL %d background checks\n", name, s_fw_check_calls); *r = 1; return true;
        }
        lv_obj_t *prev = row_labeled(panel, "Previous version");
        if (env_int("PREVIEW_FW_PREV", 0) != (prev != NULL)) { printf("check %s: FAIL Previous version row %s\n", name, prev ? "shown" : "missing"); *r = 1; return true; }
        if (prev && !find_label(prev, "1.5.9+0c1d2e3 \xc2\xb7 Go back", false)) { printf("check %s: FAIL Previous version's value\n", name); *r = 1; return true; }
        assert(find_label(panel, tr(STR_SET_FW_NOTE), false));
        int32_t yv = label_y(panel, "Version"), yu = label_y(panel, "Update firmware");
        assert(yv >= 0 && yu > yv);
        if (timer_count_300() != 0) { printf("check %s: FAIL a 300 ms timer on About\n", name); *r = 1; return true; }
        return false;   /* the generic checks: fit (the long error cut inside the row), amber, targets */
    }
    if (!strcmp(click, "Update firmware")) {
        bool older = !strcmp(m, "older");
        if (!find_label(top, "Install 1.6.1+3f2a9c1?", false)) { printf("check %s: FAIL no Install confirm\n", name); *r = 1; return true; }
        lv_obj_t *body = find_label_has(top, "1.6.0+80ab00e by itself", 0);
        assert(body);
        if (older != !strncmp(lv_label_get_text(body), "This is older than the running version.\n", 40)) {
            printf("check %s: FAIL the older line\n", name); *r = 1; return true;
        }
        *r |= pv_run_checks(d, s, name, false, 0);
        /* Esc cancels: nothing starts */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 200);
        assert(!find_label(top, "Install 1.6.1+3f2a9c1?", false) && s_fw_start_calls == 0);
        /* again: opened by a key Cancel has the focus, so Enter alone cancels; the arrows never reach Install */
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Update firmware"), LV_EVENT_CLICKED, NULL); step(d, 200);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_RIGHT }); step(d, 20);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
        if (s_fw_start_calls != 0 || find_label(top, "Install 1.6.1+3f2a9c1?", false)) {
            printf("check %s: FAIL an arrow then Enter installed or left the confirm open\n", name); *r = 1; return true;
        }
        /* Tab reaches Install; Enter starts it once and the progress screen opens */
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Update firmware"), LV_EVENT_CLICKED, NULL); step(d, 200);
        if (getenv("PREVIEW_FW_RESTAGE")) s_pv_fw_restaged = true;   /* fix I3: a different build staged meanwhile */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 20);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
        if (s_pv_fw_restaged) {
            /* nothing starts; a new confirm names the new build, with the older line */
            lv_obj_t *nb = find_label_has(top, "1.6.0+80ab00e by itself", 0);
            if (s_fw_start_calls != 0 || !find_label(top, "Install 1.5.0+0ffee00?", false) || !nb ||
                strncmp(lv_label_get_text(nb), "This is older than the running version.\n", 40)) {
                printf("check %s: FAIL a restaged build: %d starts, no new confirm\n", name, s_fw_start_calls); *r = 1; return true;
            }
            *r |= pv_run_checks(d, s, name, false, 0);
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 20);
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
        }
        if (s_fw_start_calls != 1 || !ui_fwupdate_shown()) {
            printf("check %s: FAIL Enter: %d starts, progress %s\n", name, s_fw_start_calls, ui_fwupdate_shown() ? "shown" : "not shown"); *r = 1; return true;
        }
        s_pv_fw_phase = FWU_IDLE; pv_fw_post(FWU_NOTE_RESULT, FW_OUT_CANCELLED, NULL, NULL); step(d, 400);
        assert(!ui_fwupdate_shown());
        return true;
    }
    /* Previous version: the Go back confirm; Enter goes back (the stub fails, so it toasts) */
    if (!find_label(top, "Go back to 1.5.9+0c1d2e3?", false) || !find_label(top, tr(STR_FW_GO_BACK_BODY), false)) {
        printf("check %s: FAIL no Go back confirm\n", name); *r = 1; return true;
    }
    *r |= pv_run_checks(d, s, name, false, 0);
    /* opened by touch: no focus until a key; Tab goes Cancel, then Go back; it runs on the worker and the stub's
       failure toasts */
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
    if (s_fw_goback_calls != 1 || !find_label(top, tr(STR_FW_ERR_WRITE), false)) {
        printf("check %s: FAIL go back called %d times, or no failure toast\n", name, s_fw_goback_calls); *r = 1;
    }
    return true;
}

static int timer_count_300(void) { return timers_with_period(300); }

/* fwupdate <phase>: the progress screen over Settings › About, PREVIEW_FW_PCT percent into the phase. */
static int fwupdate_case(lv_display_t *d, const char *phase) {
    static const struct { const char *n; fwu_phase_t p; const char *line; } k[] = {
        { "download", FWU_DOWNLOADING, "Downloading from the Host" }, { "check", FWU_CHECKING, "Checking the download" },
        { "write", FWU_WRITING, "Writing" }, { "readback", FWU_READBACK, "Checking what was written" },
        { "restart", FWU_RESTARTING, "Restarting\xe2\x80\xa6" } };
    int i = 0;
    while (i < 5 && strcmp(k[i].n, phase)) i++;
    assert(i < 5);
    char name[48];
    snprintf(name, sizeof(name), "fwupdate-%s", phase);
    lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, pointer_read);
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_ABOUT); step(d, 200);
    lv_obj_t *s = lv_screen_active();
    s_pv_fw_phase = k[i].p;
    ui_fwupdate_show(NULL); step(d, 400);
    lv_obj_t *top = lv_layer_top();
    int r = 0;
    bool restart = k[i].p == FWU_RESTARTING;
    int pct = restart ? 100 : env_int("PREVIEW_FW_PCT", 50);
    char bytes[64], mb[32];
    fw_mb_text(restart ? 6374957 : (6374957ull * env_int("PREVIEW_FW_PCT", 50) + 99) / 100, 6374957, mb, sizeof(mb));
    snprintf(bytes, sizeof(bytes), "%s \xc2\xb7 %d %%", mb, pct);
    lv_obj_t *bar = find_shown(top, &lv_bar_class);
    if (!find_label(top, "Updating firmware", false) || !find_label(top, k[i].line, false) || !bar || lv_bar_get_value(bar) != pct ||
        !find_label(top, bytes, false)) {
        printf("check %s: FAIL title, phase, bar (%d) or bytes (\"%s\")\n", name, bar ? (int)lv_bar_get_value(bar) : -1, bytes); return 1;
    }
    if (!restart && env_int("PREVIEW_FW_PCT", 50) == 50 && strcmp(bytes, "3.2 of 6.4 MB \xc2\xb7 50 %")) { printf("check %s: FAIL bytes \"%s\"\n", name, bytes); return 1; }
    lv_obj_t *hint = find_label(top, tr(STR_FW_HINT), false), *cancel = find_clickable(top, "Cancel");
    if (restart == (obj_shown(hint) || obj_shown(cancel))) { printf("check %s: FAIL hint and Cancel %s\n", name, restart ? "shown at restart" : "hidden"); return 1; }
    lv_obj_t *fill = lv_obj_get_child(lv_obj_get_child(top, lv_obj_get_child_count(top) - 1), 0);
    assert(lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR), lv_color_hex(UI_TK_FG)) && fill);
    r |= pv_run_checks(d, s, name, false, 0);   /* fit, no amber, targets, timers (the screen's own 300 ms) */
    if (timer_count_300() != 1 || !ui_fwupdate_timer()) { printf("check %s: FAIL %d 300 ms timers\n", name, timer_count_300()); return 1; }
    /* keys: everything but Esc is eaten; a tap outside Cancel does nothing */
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'x' }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'p', .mods = MOD_CTRL }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 20);
    tap(d, in, 100, 100);
    if (s_fw_cancel_calls || !ui_fwupdate_shown() || !ui_nav_is_top(ui_settings_create)) {
        printf("check %s: FAIL a key or a tap did something (%d cancels)\n", name, s_fw_cancel_calls); return 1;
    }
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 20);
    if (s_fw_cancel_calls != 1) { printf("check %s: FAIL Esc: %d cancels\n", name, s_fw_cancel_calls); return 1; }
    if (!restart) {
        lv_area_t a; lv_obj_get_coords(cancel, &a);
        tap(d, in, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        if (s_fw_cancel_calls != 2) { printf("check %s: FAIL a tap on Cancel: %d cancels\n", name, s_fw_cancel_calls); return 1; }
    }
    /* the update ends cancelled: the screen goes, its timer with it, and the outcome toasts */
    s_pv_fw_phase = FWU_IDLE; pv_fw_post(FWU_NOTE_RESULT, FW_OUT_CANCELLED, NULL, NULL);
    step(d, 400);
    if (ui_fwupdate_shown() || ui_fwupdate_timer() || timer_count_300() != 0) { printf("check %s: FAIL the screen or its timer stayed\n", name); return 1; }
    if (!find_label(top, "Update cancelled. Nothing was changed.", false)) { printf("check %s: FAIL no cancelled toast\n", name); return 1; }
    printf("check %s: closed ok\n", name);
    return r;
}

/* fw-notes: a boot note toasts once on Sessions (Updated, rolled back, boot loop, rolling back). */
static int fw_notes_case(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d, 400);
    lv_obj_t *s = lv_screen_active(), *top = lv_layer_top();
    static const struct { fwu_note_kind_t k; const char *a, *b, *want; } n[] = {
        { FWU_NOTE_UPDATED, "1.6.1+3f2a9c1", NULL, "Updated to 1.6.1+3f2a9c1" },
        { FWU_NOTE_ROLLED_BACK, "1.6.1+3f2a9c1", "1.6.0+80ab00e", "The update to 1.6.1+3f2a9c1 didn't start properly, so the Tab5 went back to 1.6.0+80ab00e." },
        { FWU_NOTE_BOOTLOOP, "1.6.0+80ab00e", NULL, "The Tab5 kept restarting, so it went back to 1.6.0+80ab00e." },
        { FWU_NOTE_ROLLING_BACK, "1.6.0+80ab00e", NULL, "The new version can't reach the Host. Going back to 1.6.0+80ab00e\xe2\x80\xa6" },
        { FWU_NOTE_BOOTLOOP_BOTH, NULL, NULL, "Both versions keep restarting. Flash a good build over USB (README \xe2\x80\xba Update over Wi-Fi)." },
        { FWU_NOTE_BOOTLOOP_ALONE, NULL, NULL, "Restarting repeatedly; no earlier version to go back to. Flash a good build over USB." },
        { FWU_NOTE_ROLLBACK_FAILED, NULL, NULL, "The new version can't reach the Host and the Tab5 couldn't go back; it keeps this version." } };
    int r = 0;
    for (int i = 0; i < 7; i++) {
        lv_obj_clean(top);
        pv_fw_post(n[i].k, FW_OUT_OK, n[i].a, n[i].b);
        step(d, 400);
        if (!toast_whole(top, n[i].want)) { printf("check fw-notes: FAIL no whole toast \"%s\"\n", n[i].want); return 1; }
        if (s_pv_fw_note_ready) { printf("check fw-notes: FAIL the note wasn't taken\n"); return 1; }
        char nm[32]; snprintf(nm, sizeof(nm), "fw-notes-%d", i);
        r |= pv_run_checks(d, s, nm, false, 0);
    }
    /* a RESULT belongs to the progress screen: Sessions leaves it */
    lv_obj_clean(top);
    pv_fw_post(FWU_NOTE_RESULT, FW_OUT_CANCELLED, NULL, NULL); step(d, 400);
    if (!s_pv_fw_note_ready || find_label(top, "Update cancelled. Nothing was changed.", false)) { printf("check fw-notes: FAIL Sessions took a result\n"); return 1; }
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
    else if (pv_newkey()) snprintf(name, sizeof(name), "settings-%d-new-key", section);
    else if (scroll) snprintf(name, sizeof(name), "settings-%d-scroll-%d", section, scroll);
    else snprintf(name, sizeof(name), "settings-%d", section);
    if (env_int("PREVIEW_TS", 2) != 2) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-ts%d", env_int("PREVIEW_TS", 2));
    if (!env_int("PREVIEW_KEY", 1)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-nokey");
    if (env_int("PREVIEW_VOICE_RC", 0)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-rc%d", env_int("PREVIEW_VOICE_RC", 0));
    if (getenv("PREVIEW_FW_CHECK")) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-fw%s", pv_fw_mode());
    if (env_int("PREVIEW_FW_PREV", 0)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-prev");
    if (getenv("PREVIEW_FW_RESTAGE")) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-restaged");
    if (!env_int("PREVIEW_FW_ROLLBACK", 1)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-norollback");
    if (!env_int("PREVIEW_FW_LAYOUT", 1)) snprintf(name + strlen(name), sizeof(name) - strlen(name), "-oldlayout");
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
        assert(find_label(top, "Forget the Host's key?", false));
        assert(find_label(top, "Only do this if the Host's SSH key changed. The Tab5 will trust the next key it sees.", false));
        r |= pv_run_checks(d, s, name, false, 0);   /* the confirm: wrapped title, fit, every button's 80 px hit */
        lv_obj_t *ok = find_clickable_shown(top, "Forget");
        assert(ok && s_forget_calls == 0);
        /* stop review I1, by keyboard: Up then Enter cancels (an arrow never reaches Forget) */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP }); step(d, 50);
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
        if (s_forget_calls || find_label(top, "Forget the Host's key?", false)) { printf("check %s: FAIL Up, Enter on the Forget confirm\n", name); return 1; }
        /* opened again after a key (Cancel has the focus): Tab, Enter forgets */
        lv_obj_send_event(find_clickable_shown(panel, "Forget"), LV_EVENT_CLICKED, NULL); step(d, 100);
        assert(find_label(top, "Forget the Host's key?", false));
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 200);
        if (s_forget_calls != 1) { printf("check %s: FAIL Tab, Enter: forget called %d times\n", name, s_forget_calls); return 1; }
        /* and by touch */
        lv_obj_send_event(find_clickable_shown(panel, "Forget"), LV_EVENT_CLICKED, NULL); step(d, 100);
        ok = find_clickable_shown(top, "Forget");
        assert(ok);
        lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL); step(d,200);
        if (s_forget_calls != 2) { printf("check %s: FAIL forget called %d times\n", name, s_forget_calls); r = 1; }
        lv_obj_clean(top);
        return r;
    } else if (section == UI_SET_HOST && !strcmp(click, "Trust this key")) {
        /* polish 1: the confirm shows the key again and the command that prints the Host's own; only then is it trusted */
        assert(find_label(top, "Trust this key?", false));
        lv_obj_t *body = find_label_has(top, PREVIEW_NEW_FP, 0);
        if (!body || !strstr(lv_label_get_text(body), "ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub") ||
            !strstr(lv_label_get_text(body), "Trust it only if they match")) {
            printf("check %s: FAIL the confirm doesn't show the key and how to check it\n", name); return 1;
        }
        if (s_trust_calls || s_forget_calls) { printf("check %s: FAIL trusted before the confirm\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);   /* the confirm: the wrapped fingerprint fits, every button's hit */
        lv_obj_t *cancel = find_clickable_shown(top, "Cancel");
        assert(cancel);
        lv_obj_send_event(cancel, LV_EVENT_CLICKED, NULL); step(d, 200);
        if (s_trust_calls) { printf("check %s: FAIL Cancel trusted the key\n", name); return 1; }
        /* fix round 1, C1: the confirm trusts its own copy. The Host presents key B and the panel refills while the
           confirm (still reading A) is open: Trust trusts nothing, says so, and the panel shows B */
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Trust this key"), LV_EVENT_CLICKED, NULL); step(d, 200);
        s_pv_other_key = true;
        ui_settings_refill(); step(d, 100);
        if (!find_label_has(top, PREVIEW_NEW_FP, 0) || !find_label(settings_panel(s), PREVIEW_OTHER_FP, false)) {
            printf("check %s: FAIL the refill under the confirm\n", name); return 1;
        }
        lv_obj_send_event(find_clickable_shown(top, "Trust"), LV_EVENT_CLICKED, NULL); step(d, 200); step(d, 200);
        if (s_trust_calls || !find_label(top, tr(STR_SET_TRUST_STALE), false)) {
            printf("check %s: FAIL a refill changed what the confirm trusts (%d calls, \"%s\")\n", name, s_trust_calls, s_trust_fp); return 1;
        }
        if (!toast_whole(top, tr(STR_SET_TRUST_STALE))) { printf("check %s: FAIL the stale-key toast is cut\n", name); return 1; }
        lv_obj_clean(top);
        /* polish 2 (N3): a reconnect clears the unknown key while the confirm is open: the same refusal, the same copy
           (it names both cases) */
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Trust this key"), LV_EVENT_CLICKED, NULL); step(d, 200);
        s_pv_key_cleared = true;
        lv_obj_send_event(find_clickable_shown(top, "Trust"), LV_EVENT_CLICKED, NULL); step(d, 200); step(d, 200);
        s_pv_key_cleared = false;
        if (s_trust_calls || !find_label(top, tr(STR_SET_TRUST_STALE), false)) {
            printf("check %s: FAIL a cleared key: %d trust calls, or no stale-key toast\n", name, s_trust_calls); return 1;
        }
        lv_obj_clean(top);
        /* the trust worker's own refusals fit the toast whole too */
        ui_toast_ms(tr(STR_SES_TRUST_ENDED), 3000); step(d, 50);
        if (!toast_whole(top, tr(STR_SES_TRUST_ENDED))) { printf("check %s: FAIL the trust-ended toast is cut\n", name); return 1; }
        lv_obj_clean(top);
        /* the same key after a refill under the confirm: A is trusted, exactly the copy the confirm showed. The
           keyboard focus is on the Trust row (M7: after the pin it must not land on Forget) */
        s_pv_other_key = false;
        ui_settings_refill(); step(d, 100);
        lv_obj_t *trow = find_clickable_shown(settings_panel(s), "Trust this key");
        assert(trow && ui_keyboard_focus_in(trow, 0));
        lv_obj_send_event(trow, LV_EVENT_CLICKED, NULL); step(d, 200);
        ui_settings_refill(); step(d, 100);
        lv_obj_t *ok = find_clickable_shown(top, "Trust");
        assert(ok);
        lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL); step(d, 200); step(d, 200);
        if (s_trust_calls != 1 || strcmp(s_trust_fp, PREVIEW_NEW_FP) || s_forget_calls) {
            printf("check %s: FAIL trust called %d times with \"%s\" (forget %d)\n", name, s_trust_calls, s_trust_fp, s_forget_calls); return 1;
        }
        /* pinned: the toast names the key, and the panel shows it pinned, with Forget */
        panel = settings_panel(s);
        if (!find_label_has(top, "New host key pinned: " PREVIEW_NEW_FP, 0) || !find_label(panel, PREVIEW_NEW_FP, false) ||
            find_clickable_shown(panel, "Trust this key") || !find_clickable_shown(panel, "Forget")) {
            printf("check %s: FAIL after the trust: no toast, or the panel didn't follow\n", name); return 1;
        }
        lv_obj_t *fz = ui_keyboard_focused(), *fg = find_clickable_shown(panel, "Forget");
        for (lv_obj_t *o = fz; o; o = lv_obj_get_parent(o))
            if (o == fg) { printf("check %s: FAIL the focus landed on Forget after the trust (M7)\n", name); return 1; }
        if (!fz || fz != row_labeled(panel, "Host")) { printf("check %s: FAIL the focus after the trust isn't the panel's first control\n", name); return 1; }
        printf("%s: the confirm shows the key and ssh-keygen; Cancel trusts nothing; a key changed under the confirm is refused; Trust pins the confirmed key once; focus off Forget\n", name);
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
        /* T8 review M5: a value change rebuilds the panel where it was, not at the top */
        int32_t y = lv_obj_get_scroll_y(panel);
        lv_obj_send_event(find_clickable_shown(panel, "Deep idle after"), LV_EVENT_CLICKED, NULL); step(d, 200);
        lv_obj_t *sheet = lv_obj_get_parent(find_label(top, "Deep idle after", false));
        lv_obj_send_event(lv_obj_get_child(sheet, 2), LV_EVENT_CLICKED, NULL); step(d, 200);
        if (lv_obj_get_scroll_y(settings_panel(s)) != y) { printf("check %s: FAIL the panel scrolled %d -> %d on a change\n", name, (int)y, (int)lv_obj_get_scroll_y(settings_panel(s))); return 1; }
        panel = settings_panel(s);
    } else if (section == UI_SET_HOST && !click[0] && pv_newkey()) {
        /* polish 1: first run, no key pinned: the key the Host presented, with Trust this key; never pinned by itself */
        const char *bad = !find_label(panel, "The key the Host presented. It isn't trusted until you check it on the Host and trust it.", false) ? "no note"
                        : !find_label(panel, PREVIEW_NEW_FP, false) ? "no fingerprint"
                        : !find_clickable_shown(panel, "Trust this key") ? "no Trust this key"
                        : find_label(panel, "No key pinned yet", false) ? "\"No key pinned yet\""
                        : find_clickable_shown(panel, "Forget") ? "a Forget" : NULL;
        if (bad) {
            printf("check %s: FAIL the Host key section: %s\n", name, bad);
            return 1;
        }
        lv_obj_t *fp = find_label(panel, PREVIEW_NEW_FP, false);
        if (lv_obj_get_style_text_font(fp, 0) != FONT_MONO) { printf("check %s: FAIL the fingerprint isn't mono\n", name); return 1; }
        if (!find_label(panel, "The Host has a new host key. Check and trust it in Settings \xe2\x80\xba Host.", false)) { printf("check %s: FAIL the Link row\n", name); return 1; }
        if (s_trust_calls || s_forget_calls) { printf("check %s: FAIL trusted without a confirm\n", name); return 1; }
    } else if (section == UI_SET_HOST && !click[0] && (!pv_host() || pv_notfound())) {
        /* U11 and the frozen countdown (T7 review I2): the Link row says Host, never Mac, and no "(retry in Ns)" */
        const char *want = !pv_host() ? "No Host set up" : "Host not found at studio-host.local";
        if (!find_label(panel, want, false)) { printf("check %s: FAIL no Link text \"%s\"\n", name, want); return 1; }
        if (find_label_has(panel, "Mac", 0) || find_label_has(panel, "retry in", 0)) {
            printf("check %s: FAIL the Link row says \"%s\"\n", name,
                   lv_label_get_text(find_label_has(panel, "Mac", 0) ? find_label_has(panel, "Mac", 0) : find_label_has(panel, "retry in", 0)));
            return 1;
        }
    } else if (section == UI_SET_HOST && !strcmp(click, "Voice input")) {
        /* P5-5: the check runs once on the worker and toasts its result (PREVIEW_VOICE_RC picks it) */
        step(d, 100);
        const char *want = env_int("PREVIEW_VOICE_RC", 0) == 121 ? tr(STR_VOICE_NO_MODEL) : tr(STR_SET_VOICE_READY);
        if (s_voice_check_calls != 1) { printf("check %s: FAIL voice check called %d times\n", name, s_voice_check_calls); return 1; }
        if (!find_label(top, want, false)) { printf("check %s: FAIL no toast \"%s\"\n", name, want); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);   /* the long error toast wraps inside the screen */
        lv_obj_clean(top);
        return r;
    } else if (section == UI_SET_HOST && !strcmp(click, "Whisper model on Host")) {
        /* the prompt shows the default; a path with a space is refused with the path copy and not saved */
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta && !strcmp(lv_textarea_get_text(ta), CC_WHISPER_MODEL_DEFAULT));
        prompt_answer(d, "~/my models/base.bin");
        assert(find_label(top, tr(STR_SET_CLAUDE_PATH_BAD), false));
        assert(!strcmp(s_pv_whisper_model, CC_WHISPER_MODEL_DEFAULT));
        lv_obj_clean(top); step(d, 200);
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Whisper model on Host"), LV_EVENT_CLICKED, NULL); step(d, 200);
        prompt_answer(d, "~/models/ggml-small.en.bin");
        assert(!strcmp(s_pv_whisper_model, "~/models/ggml-small.en.bin"));
        assert(find_label(settings_panel(s), "~/models/ggml-small.en.bin", false));
        snprintf(s_pv_whisper_model, sizeof(s_pv_whisper_model), "%s", CC_WHISPER_MODEL_DEFAULT);
        panel = settings_panel(s);
    } else if (section == UI_SET_HOST && !click[0]) {
        assert(!find_label(s, "(macOS)", true));
        /* Plan 5 (spec 6.5): Whisper path, Whisper model and Voice input follow Claude path, with the defaults */
        static const char *const order[] = { "Claude path on Host", "Whisper path on Host", "Whisper model on Host", "Voice input",
                                             "Firmware file on Host" };   /* Plan 6 */
        int32_t prev = -1;
        for (unsigned i = 0; i < 5; i++) {
            int32_t y = label_y(panel, order[i]);
            if (y <= prev) { printf("check %s: FAIL \"%s\" at y %d, not below the row before it\n", name, order[i], (int)y); return 1; }
            prev = y;
        }
        lv_obj_t *wp = row_labeled(panel, "Whisper path on Host"), *wm = row_labeled(panel, "Whisper model on Host");
        assert(wp && wm && row_labeled(panel, "Voice input"));
        assert(find_label(wp, CC_WHISPER_DEFAULT, false) && find_label(wm, CC_WHISPER_MODEL_DEFAULT, false));
        assert(find_label(row_labeled(panel, "Voice input"), "Check", false));
        assert(row_labeled(panel, "Firmware file on Host") && label_shows(row_labeled(panel, "Firmware file on Host"), CC_FW_PATH_DEFAULT));
        assert(find_label(panel, tr(STR_SET_HOST_NOTE), false));
        assert(find_label(panel, "\xe2\x97\x8f connected over home Wi-Fi", false));
        assert(find_label(panel, "ssh-ed25519 \xe2\x80\xa6 claude-companion \xc2\xb7 Show", false));
        for (lv_obj_t *o = rail_row(s, 0); o; o = NULL) assert(lv_obj_get_height(o) == UI_TK_RAIL_ROW_H);
        lv_obj_t *rail = lv_obj_get_child(lv_obj_get_child(s, 1), 0);
        assert(lv_obj_get_width(rail) == UI_TK_RAIL_W);
    } else if (section == UI_SET_HOST && !strcmp(click, "Firmware file on Host")) {
        /* Plan 6: the prompt shows the path; one with a space is refused with the path copy and not saved */
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta && !strcmp(lv_textarea_get_text(ta), CC_FW_PATH_DEFAULT));
        prompt_answer(d, "/a b");
        assert(find_label(top, tr(STR_SET_FW_PATH_BAD), false));
        assert(!strcmp(s_pv_fw_path, CC_FW_PATH_DEFAULT));
        lv_obj_clean(top); step(d, 200);
        lv_obj_send_event(find_clickable_shown(settings_panel(s), "Firmware file on Host"), LV_EVENT_CLICKED, NULL); step(d, 200);
        prompt_answer(d, "/tmp/cc6.ab12/stage/claude_companion.bin");
        assert(!strcmp(s_pv_fw_path, "/tmp/cc6.ab12/stage/claude_companion.bin"));
        assert(label_shows(settings_panel(s), "/tmp/cc6.ab12/stage/claude_companion.bin"));
        snprintf(s_pv_fw_path, sizeof(s_pv_fw_path), "%s", CC_FW_PATH_DEFAULT);
        panel = settings_panel(s);
    } else if (section == UI_SET_ABOUT && (!click[0] || !strcmp(click, "Update firmware") || !strcmp(click, "Previous version"))) {
        if (pv_fw_about(d, s, click, name, &r)) { lv_obj_clean(top); return r; }
        panel = settings_panel(s);
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

/* ------------------------------------------------------------ Terminal and the host form (Task 9) */
static lv_obj_t *hosts_body(lv_obj_t *scr) { return lv_obj_get_child(scr, 2); }
/* The host row (line > row > column > name) whose name is t. */
static lv_obj_t *host_row(lv_obj_t *o, const char *t) {
    lv_obj_t *l = find_label(o, t, false);
    return l ? lv_obj_get_parent(lv_obj_get_parent(l)) : NULL;
}
static lv_obj_t *host_edit_button(lv_obj_t *row) { return lv_obj_get_child(lv_obj_get_parent(row), 1); }
static int host_lines(lv_obj_t *body) {
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) {
        lv_obj_t *c = lv_obj_get_child(body, i);
        n += lv_obj_get_child_count(c) == 2 && lv_obj_check_type(lv_obj_get_child(c, 1), &lv_button_class);
    }
    return n;
}
/* -1 no dot, 0 line grey, 1 green */
static int host_dot_of(lv_obj_t *row) {
    lv_obj_t *dot = lv_obj_get_child(row, 0);
    if (lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN)) return -1;
    lv_color_t c = lv_obj_get_style_bg_color(dot, 0);
    return lv_color_eq(c, lv_color_hex(UI_TK_GREEN)) ? 1 : lv_color_eq(c, lv_color_hex(UI_TK_LINE)) ? 0 : 2;
}
static lv_obj_t *host_note(lv_obj_t *row) { lv_obj_t *t = lv_obj_get_child(row, 2); return lv_obj_has_flag(t, LV_OBJ_FLAG_HIDDEN) ? NULL : t; }
static bool red(lv_obj_t *l) { return l && lv_color_eq(lv_obj_get_style_text_color(l, 0), lv_color_hex(UI_TK_RED)); }
#define FOOTNOTE_TOUCH "The Host isn't listed here: its sessions are on the Sessions screen. In a session, tap the top of the screen for the bar; \xe2\x86\x90 Leave disconnects."
#define FOOTNOTE "The Host isn't listed here: its sessions are on the Sessions screen. Ctrl+\xe2\x86\x90 leaves an SSH session."
static lv_obj_t *open_terminal(lv_display_t *d) {
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_nav_push(ui_hosts_create, NULL); step(d, 200);
    assert(ui_nav_is_top(ui_hosts_create));
    return lv_screen_active();
}

static void form_set(lv_display_t *d, lv_obj_t *scr, const char *row, const char *text);
static void click_row(lv_display_t *d, lv_obj_t *panel, const char *t);
/* terminal [on|empty|keys|deleted-event|fail|add|add-fqdn]: spec §5.3. The default: Tailscale off. */
static int term_voice_case(lv_display_t *d, const char *v);   /* voice in a session, below the take-over cases */
static int term_scroll_case(lv_display_t *d, const char *v);   /* scrollback keys and chips, beside it */
static int terminal_case(lv_display_t *d, const char *v) {
    if (!strncmp(v, "voice-", 6)) return term_voice_case(d, v);
    if (!strncmp(v, "scroll-", 7)) return term_scroll_case(d, v);
    char name[48];
    snprintf(name, sizeof(name), "terminal%s%s", v[0] ? "-" : "", v);
    bool ts_off = !v[0] || !strcmp(v, "fail");
    g_settings.ts_enable = !ts_off;
    setenv("PREVIEW_TS", ts_off ? "0" : "2", 1);
    int timers0 = -1;
    if (!strcmp(v, "empty")) setenv("PREVIEW_HOSTS", "empty", 1);
    bool nokbd = !strcmp(v, "nokbd");
    if (nokbd) setenv("PREVIEW_KBD", "0", 1);   /* touch alone */
    lv_obj_t *s = open_terminal(d), *body = hosts_body(s), *banner = lv_obj_get_child(s, 1), *top = lv_layer_top();
    (void)timers0;
    assert(find_clickable_shown(s, "\xe2\x86\x90 Sessions") && find_label(s, "Terminal", false));
    if (find_label(body, "Desk", false) || find_label_has(body, "me@studio-host", 0)) { printf("check %s: FAIL the Host's entry is listed\n", name); return 1; }
    if (!find_label(body, nokbd ? FOOTNOTE_TOUCH : FOOTNOTE, false)) { printf("check %s: FAIL no footnote\n", name); return 1; }
    int r = 0;
    if (nokbd) {
        /* the footnote says how to leave a session by touch; the host form's fields bring the soft keyboard, with the
           prompt's OK and Cancel above it; Cancel, "← Terminal" and "← Sessions" leave by touch */
        r |= pv_run_checks(d, s, name, false, 0);
        click_label(d, body, "+ Add host");
        click_label(d, top, "Enter manually");
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_host_form_create));
        click_row(d, lv_obj_get_child(s, 1), "Address");
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta);
        lv_obj_send_event(ta, LV_EVENT_CLICKED, NULL); step(d, 200);
        lv_obj_t *ok = find_clickable_shown(top, "OK"), *cancel = find_clickable_shown(top, "Cancel");
        if (!softkbd_visible() || !ok || !cancel) { printf("check %s: FAIL no soft keyboard on a host-form field\n", name); return 1; }
        lv_area_t oa, ka;
        lv_obj_get_coords(ok, &oa);
        lv_obj_t *kb = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(ok))), -1);
        lv_obj_get_coords(kb, &ka);
        if (oa.y2 >= ka.y1) { printf("check %s: FAIL the soft keyboard (y %d) covers OK (y2 %d)\n", name, (int)ka.y1, (int)oa.y2); return 1; }
        r |= pv_run_checks(d, s, "terminal-nokbd-prompt", false, 0);
        click_label(d, top, "Cancel");
        if (softkbd_visible() || find_shown(top, &lv_textarea_class)) { printf("check %s: FAIL Cancel left the prompt or the keyboard up\n", name); return 1; }
        click_label(d, s, "\xe2\x86\x90 Terminal");
        if (!ui_nav_is_top(ui_hosts_create)) { printf("check %s: FAIL \"\xe2\x86\x90 Terminal\" didn't go back\n", name); return 1; }
        click_label(d, lv_screen_active(), "\xe2\x86\x90 Sessions");
        if (!ui_nav_is_top(ui_sessions_create)) { printf("check %s: FAIL \"\xe2\x86\x90 Sessions\" didn't go back\n", name); return 1; }
        printf("%s: touch footnote; host-form fields bring the soft keyboard under the prompt's OK; Cancel, \u2190 Terminal and \u2190 Sessions leave\n", name);
        return r;
    }
    if (!v[0]) {
        if (lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN) || !find_label_has(banner, "Tailscale is off", 0) || !find_label(banner, "Turn on", false)) {
            printf("check %s: FAIL no Tailscale-off banner\n", name); return 1;
        }
        if (host_lines(body) != 2) { printf("check %s: FAIL %d host rows\n", name, host_lines(body)); return 1; }
        assert(find_label(body, "pi-lan", false) && find_label(body, "pi@192.168.1.50", false) && find_label(body, "dev@100.64.0.11", false));
        assert(host_dot_of(host_row(body, "build-box")) == -1 && host_dot_of(host_row(body, "pi-lan")) == -1);   /* no dots while off */
        lv_area_t ba, bb; lv_obj_get_coords(banner, &ba); lv_obj_get_coords(body, &bb);
        assert(bb.y1 >= ba.y2);   /* the list starts under the banner */
        r |= pv_run_checks(d, s, name, false, 0);
        click_label(d, banner, "Turn on");
        if (!g_settings.ts_enable || !lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) { printf("check %s: FAIL Turn on\n", name); return 1; }
        assert(lv_screen_active() == s);   /* in place */
        printf("%s: banner with Turn on, 2 rows, no Host entry; Turn on switched Tailscale on in place\n", name);
        return r;
    }
    if (!strcmp(v, "on")) {
        assert(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));
        lv_obj_t *tn = host_row(body, "build-box"), *lan = host_row(body, "pi-lan");
        if (host_dot_of(tn) != 1 || host_dot_of(lan) != -1) { printf("check %s: FAIL dots tailnet %d LAN %d\n", name, host_dot_of(tn), host_dot_of(lan)); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        /* a peer event: the tailnet host went offline: the dot turns grey in place */
        s_peer_a_offline = true;
        lv_obj_t *old_row = tn;
        int reads = s_peer_reads;
        s_ts_cb(s_ts_cb_user); s_ts_cb(s_ts_cb_user); step(d, 200);   /* two events, one update */
        if (host_dot_of(tn) != 0 || host_row(body, "build-box") != old_row) { printf("check %s: FAIL the offline dot after the event (%d, reads %d)\n", name, host_dot_of(tn), s_peer_reads - reads); return 1; }
        assert(s_peer_reads == reads + 1);
        printf("%s: tailnet row dotted green, LAN row none; an event turned it grey in place\n", name);
        return r;
    }
    if (!strcmp(v, "empty")) {
        if (host_lines(body) != 0 || !find_label(body, "+ Add host", false)) { printf("check %s: FAIL rows\n", name); return 1; }
        assert(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));
        r |= pv_run_checks(d, s, name, false, 0);
        /* T13-4: the first Down lands on "+ Add host", never the header's "← Sessions" */
        lv_obj_t *add = find_clickable_shown(body, "+ Add host"), *back = find_clickable_shown(s, "\xe2\x86\x90 Sessions");
        press_key(SK_DOWN, 0, false); step(d, 50);
        if (!lv_obj_has_state(add, LV_STATE_FOCUS_KEY) || lv_obj_has_state(back, LV_STATE_FOCUS_KEY)) {
            printf("check %s: FAIL the first Down didn't land on + Add host\n", name); return 1;
        }
        press_key(SK_ENTER, 0, false); step(d, 200);
        if (!ui_nav_is_top(ui_hosts_create) || !find_label(top, "Add host", false)) {
            printf("check %s: FAIL Down, Enter didn't open the add sheet\n", name); return 1;
        }
        lv_obj_clean(top);
        printf("%s: the first Down lands on + Add host; Enter opens the add sheet\n", name);
        return r;
    }
    if (!strcmp(v, "keys")) {
        assert(!preview_have_sink);
        lv_obj_t *r0 = host_row(body, "pi-lan"), *r1 = host_row(body, "build-box");
        int n = 0;
        while (!lv_obj_has_state(r0, LV_STATE_FOCUS_KEY) && n < 4) { press_key(SK_DOWN, 0, false); step(d, 50); n++; }
        if (!lv_obj_has_state(r0, LV_STATE_FOCUS_KEY)) { printf("check %s: FAIL row 0 never focused\n", name); return 1; }
        lv_obj_t *hint = host_note(r0);
        assert(hint && !strcmp(lv_label_get_text(hint), "\xe2\x86\xb5 Connect \xc2\xb7 \xe2\x8c\x83\xe2\x86\xb5 Edit") && !host_note(r1));
        assert(find_label(body, "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 Enter choose \xc2\xb7 Esc back", false));   /* §5.13 hint */
        press_key(SK_DOWN, 0, false); step(d, 50);
        if (!lv_obj_has_state(r1, LV_STATE_FOCUS_KEY)) { printf("check %s: FAIL Down did not reach row 1 (the Edit button is touch only)\n", name); return 1; }
        assert(!host_note(r0) && host_note(r1));
        r |= pv_run_checks(d, s, name, false, 0);
        press_key(SK_ENTER, MOD_CTRL, false); step(d, 200);   /* Ctrl+Enter: Edit */
        if (!ui_nav_is_top(ui_host_form_create) || !find_label(lv_screen_active(), "build-box", false)) { printf("check %s: FAIL Ctrl+Enter did not edit build-box\n", name); return 1; }
        press_key(SK_ESC, 0, false); step(d, 200);
        assert(ui_nav_is_top(ui_hosts_create));
        s = lv_screen_active(); body = hosts_body(s);
        r1 = host_row(body, "build-box");
        lv_obj_send_event(r1, LV_EVENT_CLICKED, NULL);   /* Enter: connect */
        if (!lv_obj_get_child(r1, 3) || !find_label(r1, "Connecting\xe2\x80\xa6", false)) { printf("check %s: FAIL no spinner on the row\n", name); return 1; }
        step(d, 200);
        if (s_term_opens != 1 || s_term_idx != 2) { printf("check %s: FAIL connect opened %d (index %d)\n", name, s_term_opens, s_term_idx); return 1; }
        printf("%s: row 0 after %d Down, Down to row 1, Ctrl+Enter edits, Enter connects index 2\n", name, n);
        return r;
    }
    if (!strcmp(v, "deleted-event")) {
        if (!s_ts_cb) { printf("check %s: FAIL Terminal doesn't follow Tailscale\n", name); return 1; }
        ts_event_cb_t old = s_ts_cb; void *old_user = s_ts_cb_user;
        old(old_user);                 /* queued while Terminal shows ... */
        ui_nav_back();                 /* ... and Terminal goes before it runs */
        if (s_ts_cb) { printf("check %s: FAIL the Tailscale slot is set after Terminal went\n", name); return 1; }
        lv_obj_t *now = lv_screen_active();
        step(d, 200);
        old(old_user); step(d, 200);   /* a late event from MicroLink's task */
        if (lv_screen_active() != now || !ui_nav_is_top(ui_sessions_create) || s_ts_cb) { printf("check %s: FAIL a stale event acted\n", name); return 1; }
        printf("check %s: ok (slot NULL after Back; a queued and a late event did nothing)\n", name);
        return 0;
    }
    if (!strcmp(v, "fail")) {
        /* refusals before any socket: a red note on the row, no session screen */
        lv_obj_t *tn = host_row(body, "build-box");
        lv_obj_send_event(tn, LV_EVENT_CLICKED, NULL); step(d, 200);
        lv_obj_t *n = host_note(tn);
        if (!red(n) || strncmp(lv_label_get_text(n), "Tailscale is off.", 17) || s_term_opens) { printf("check %s: FAIL the Tailscale-off refusal (\"%s\")\n", name, n ? lv_label_get_text(n) : "none"); return 1; }
        r |= pv_run_checks(d, s, "terminal-refused", false, 0);
        if (strstr(lv_label_get_text(n), "...") || strstr(lv_label_get_text(n), "\xe2\x80\xa6")) { printf("check %s: FAIL the refusal is cut: \"%s\"\n", name, lv_label_get_text(n)); return 1; }
        setenv("PREVIEW_WIFI", "0", 1);
        lv_obj_t *lan = host_row(body, "pi-lan");
        lv_obj_send_event(lan, LV_EVENT_CLICKED, NULL); step(d, 200);
        setenv("PREVIEW_WIFI", "1", 1);
        assert(red(host_note(lan)) && !strcmp(lv_label_get_text(host_note(lan)), "No Wi-Fi") && !s_term_opens);
        /* a session that failed (B6's missing key file): the reason is on its row when Terminal shows again */
        ui_hosts_note_failure(1, tr(STR_HF_KEY_FILE_MISSING));
        ui_nav_refresh(); step(d, 200);
        s = lv_screen_active(); body = hosts_body(s); lan = host_row(body, "pi-lan");
        if (!red(host_note(lan)) || strcmp(lv_label_get_text(host_note(lan)), "Key file not found on SD card")) { printf("check %s: FAIL the failure isn't on the row\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        lv_obj_send_event(lan, LV_EVENT_CLICKED, NULL); step(d, 200);   /* a retry clears it and hands over */
        assert(s_term_opens == 1 && s_term_idx == 1);
        ui_nav_back(); step(d, 200);
        assert(ui_nav_is_top(ui_hosts_create) && !host_note(host_row(hosts_body(lv_screen_active()), "pi-lan")));
        /* P4C-10: the Host reached by an address the link doesn't know (an alias) presents the Host's key: refused at
           connect, before any authentication, with the footnote's sentence on the row */
        server_t alias = { .name = "alias-box", .host = "10.20.30.40", .port = 22, .user = "me", .use_key = true, .keypath = "device" };
        s_preview_servers[s_preview_server_n++] = alias;
        g_settings.ts_enable = 1;
        ui_nav_refresh(); step(d, 200);
        lv_obj_t *al = host_row(hosts_body(lv_screen_active()), "alias-box");
        if (!al) { printf("check %s: FAIL the alias isn't listed (the address check can't know it)\n", name); return 1; }
        lv_obj_send_event(al, LV_EVENT_CLICKED, NULL); step(d, 200); step(d, 50);
        if (s_term_opens != 2 || s_term_idx != s_preview_server_n - 1) { printf("check %s: FAIL the alias didn't connect\n", name); return 1; }
        /* Task 10: a session refused at connect goes back to Terminal by itself (the session bar swallows nothing) */
        if (!ui_nav_is_top(ui_hosts_create)) { printf("check %s: FAIL the refused session didn't return to Terminal\n", name); return 1; }
        al = host_row(hosts_body(lv_screen_active()), "alias-box");
        lv_obj_t *an = host_note(al);
        if (!red(an) || strncmp(lv_label_get_text(an), "The Host isn't listed here", 26)) {
            printf("check %s: FAIL the Host's key wasn't refused (\"%s\")\n", name, an ? lv_label_get_text(an) : "none"); return 1;
        }
        r |= pv_run_checks(d, lv_screen_active(), "terminal-hostkey", false, 0);
        s_preview_server_n--;
        printf("%s: Tailscale off and no Wi-Fi refused on the row; a session failure shown red on its row\n", name);
        return r;
    }
    if (!strcmp(v, "add")) {
        click_label(d, body, "+ Add host");
        if (sheet_rows("Add host") != 2 || !find_label(top, "From your tailnet", false)) { printf("check %s: FAIL the add sheet\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        click_label(d, top, "From your tailnet");
        /* online only, and never the Host's own peer (C1: preview-host-c's IP is the Host's tailnet address) */
        if (sheet_rows("From your tailnet") != 1 || find_label(top, "preview-host-c", false) || find_label(top, "preview-host-b", false)) {
            printf("check %s: FAIL the tailnet sheet offers %d peers (the Host's: %s)\n", name, sheet_rows("From your tailnet"),
                   find_label(top, "preview-host-c", false) ? "yes" : "no"); return 1;
        }
        click_label(d, top, "preview-host-a");
        lv_obj_t *f = lv_screen_active();
        if (!ui_nav_is_top(ui_host_form_create) || !find_label(f, "100.64.0.11", false) || !find_label(f, "preview-host-a", false)) { printf("check %s: FAIL the peer didn't prefill the form\n", name); return 1; }
        lv_obj_clean(top);
        ui_nav_back(); step(d, 200);   /* prefilled, unchanged: no discard question */
        assert(ui_nav_is_top(ui_hosts_create));
        g_settings.ts_enable = 0;
        click_label(d, hosts_body(lv_screen_active()), "+ Add host");
        assert(sheet_rows("Add host") == 1 && find_label(top, "Enter manually", false));
        click_label(d, top, "Enter manually");
        assert(ui_nav_is_top(ui_host_form_create) && find_label(lv_screen_active(), "Add host", false));
        lv_obj_clean(top);
        /* Settings › Network › Peers › Add as SSH host: the form, prefilled, over Terminal */
        g_settings.ts_enable = 1;
        ui_settings_add_peer_host(&k_preview_peers[0]); step(d, 200);
        assert(ui_nav_is_top(ui_host_form_create) && find_label(lv_screen_active(), "100.64.0.11", false));
        ui_nav_back(); step(d, 200);
        assert(ui_nav_is_top(ui_hosts_create));
        printf("%s: tailnet peers (online only) and manual entry; Peers' Add as SSH host opens the form over Terminal\n", name);
        return r;
    }
    if (!strcmp(v, "add-fqdn")) {
        /* C1 with no tailnet IP known: the Host's peer has its MagicDNS FQDN; the first label drops it everywhere */
        assert(!pv_host_tsip());
        click_label(d, body, "+ Add host");
        click_label(d, top, "From your tailnet");
        if (find_label(top, "studio-host.tail1234.ts.net", false) || sheet_rows("From your tailnet") != 1) {
            printf("check %s: FAIL the Host's FQDN peer is offered\n", name); return 1;
        }
        r |= pv_run_checks(d, s, name, false, 0);
        lv_obj_clean(top);
        ts_peer_t hp; tailscale_peer(2, &hp);
        ui_settings_add_peer_host(&hp); step(d, 200);   /* Peers › Add as SSH host */
        if (ui_nav_is_top(ui_host_form_create) || !find_label(top, "The Host isn't listed here: its sessions are on the Sessions screen.", false)) {
            printf("check %s: FAIL Peers' Add as SSH host offered the Host\n", name); return 1;
        }
        lv_obj_clean(top);
        /* typed by hand: the FQDN is refused at Save */
        click_label(d, hosts_body(lv_screen_active()), "+ Add host");
        click_label(d, top, "Enter manually");
        lv_obj_t *f = lv_screen_active();
        form_set(d, f, "Address", "studio-host.tail1234.ts.net");
        form_set(d, f, "User", "me");
        click_label(d, f, "Save");
        if (!find_label(top, "That's the Host: its sessions are on the Sessions screen.", false) || s_store_adds) {
            printf("check %s: FAIL the FQDN was saved\n", name); return 1;
        }
        lv_obj_clean(top);
        printf("%s: no tailnet IP known; the Host's FQDN peer not offered, not added from Peers, refused at Save\n", name);
        return r;
    }
    fprintf(stderr, "preview: unknown terminal variant %s\n", v);
    return 2;
}

/* The value label of the form row labelled t. */
static const char *form_value(lv_obj_t *scr, const char *t) {
    lv_obj_t *row = row_labeled(lv_obj_get_child(scr, 1), t);
    lv_obj_t *v = row ? ui_srow_value(row) : NULL;
    return v ? lv_label_get_text(v) : NULL;
}
/* Clicks the row labelled t (a direct child of panel). */
static void click_row(lv_display_t *d, lv_obj_t *panel, const char *t) {
    lv_obj_t *row = row_labeled(panel, t);
    if (!row) printf("preview: no row \"%s\"\n", t);
    assert(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); step(d, 200);
}
static void form_set(lv_display_t *d, lv_obj_t *scr, const char *row, const char *text) {
    click_row(d, lv_obj_get_child(scr, 1), row);
    prompt_answer(d, text);
}

/* hostform new|edit-pin|sd-key|settings: spec §5.5. */
static int hostform_case(lv_display_t *d, const char *v) {
    char name[48];
    snprintf(name, sizeof(name), "hostform-%s", v);
    g_settings.ts_enable = 1;
    if (!strcmp(v, "sd-key")) setenv("PREVIEW_HOSTS", "sd", 1);
    lv_obj_t *s = open_terminal(d), *top = lv_layer_top();
    int r = 0;
    if (!strcmp(v, "new")) {
        click_label(d, hosts_body(s), "+ Add host");
        click_label(d, top, "Enter manually");
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_host_form_create) && find_label(s, "Add host", false) && find_clickable_shown(s, "\xe2\x86\x90 Terminal"));
        if (find_label(s, "Host key", false) || find_label(s, "Delete host", false)) { printf("check %s: FAIL a new host shows Host key or Delete\n", name); return 1; }
        assert(!strcmp(form_value(s, "Name"), "user@host") && !strcmp(form_value(s, "Port"), "22") && !strcmp(form_value(s, "Sign in with"), "Device key"));
        assert(!row_labeled(lv_obj_get_child(s, 1), "Password"));   /* only for Password */
        r |= pv_run_checks(d, s, name, false, 0);
        form_set(d, s, "Address", " box.local ");
        form_set(d, s, "User", "me");
        form_set(d, s, "Port", "70000");
        assert(!strcmp(form_value(s, "Name"), "me@box.local"));
        click_label(d, s, "Save");
        if (!find_label(top, "The port is a number from 1 to 65535", false) || s_store_adds) { printf("check %s: FAIL port 70000 (adds %d)\n", name, s_store_adds); return 1; }
        lv_obj_clean(top);
        form_set(d, s, "Address", "STUDIO-HOST.local.");
        form_set(d, s, "Port", "2222");
        click_label(d, s, "Save");
        assert(find_label(top, "That's the Host: its sessions are on the Sessions screen.", false) && !s_store_adds);
        lv_obj_clean(top);
        /* C1: the Host's LAN fallback and its tailnet address, typed by hand, are the Host too */
        const char *host_addrs[] = { "192.168.77.20", PREVIEW_HOST_TS_IP };
        for (int i = 0; i < 2; i++) {
            form_set(d, s, "Address", host_addrs[i]);
            click_label(d, s, "Save");
            if (!find_label(top, "That's the Host: its sessions are on the Sessions screen.", false) || s_store_adds) {
                printf("check %s: FAIL a manual %s wasn't refused as the Host\n", name, i ? "tailnet address" : "LAN fallback"); return 1;
            }
            lv_obj_clean(top);
        }
        form_set(d, s, "Address", "box.local");
        /* I1: a name another entry has (any case) is refused */
        form_set(d, s, "Name", "PI-LAN");
        click_label(d, s, "Save");
        if (!find_label(top, "Another host already has that name", false) || s_store_adds) { printf("check %s: FAIL a taken name\n", name); return 1; }
        lv_obj_clean(top);
        form_set(d, s, "Name", "");
        /* M7: leaving by any way asks too: home (a wake, `ui home`) waits for the answer */
        ui_nav_home(); step(d, 200);
        if (!find_label(top, "Discard changes?", false) || !ui_nav_is_top(ui_host_form_create)) { printf("check %s: FAIL home didn't ask\n", name); return 1; }
        ui_nav_back(); step(d, 200);   /* M-new-2: a second way out while asking stacks no second dialog */
        int dialogs = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) dialogs += find_label(lv_obj_get_child(top, i), "Discard changes?", false) != NULL;
        if (dialogs != 1) { printf("check %s: FAIL %d discard dialogs\n", name, dialogs); return 1; }
        click_label(d, top, "Cancel");
        assert(ui_nav_is_top(ui_host_form_create) && lv_screen_active() == s && !find_label(top, "Discard changes?", false));
        /* Sign in with: Device key or Password; the password row then shows, masked, starting empty */
        click_row(d, lv_obj_get_child(s, 1), "Sign in with");
        assert(sheet_rows("Sign in with") == 2);
        click_label(d, top, "Password");
        assert(!strcmp(form_value(s, "Password"), "Not set"));
        click_row(d, lv_obj_get_child(s, 1), "Password");
        lv_obj_t *ta = find_shown(top, &lv_textarea_class);
        assert(ta && lv_textarea_get_password_mode(ta) && !lv_textarea_get_text(ta)[0]);
        prompt_answer(d, "preview-host-pass");
        if (find_label_has(s, "preview-host-pass", 0) || strcmp(form_value(s, "Password"), "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 set")) {
            printf("check %s: FAIL the password shows\n", name); return 1;
        }
        /* Esc with changes asks; Cancel stays */
        press_key(SK_ESC, 0, false); step(d, 200);
        if (!find_label(top, "Discard changes?", false) || !ui_nav_is_top(ui_host_form_create)) { printf("check %s: FAIL no discard question\n", name); return 1; }
        r |= pv_run_checks(d, s, "hostform-discard", false, 0);
        click_label(d, top, "Cancel");
        assert(ui_nav_is_top(ui_host_form_create) && lv_screen_active() == s);
        click_label(d, s, "Save");
        const server_t *e = server_store_get(3);
        if (s_store_adds != 1 || !e || strcmp(e->name, "me@box.local") || e->port != 2222 || e->use_key || strcmp(e->pass, "preview-host-pass")) {
            printf("check %s: FAIL the saved entry\n", name); return 1;
        }
        assert(ui_nav_is_top(ui_hosts_create) && find_label(hosts_body(lv_screen_active()), "me@box.local", false) && !s_set_server_calls);
        printf("%s: no Host key or Delete; 70000 and the Host refused; masked password; discard asked; saved me@box.local:2222\n", name);
        return r;
    }
    if (!strcmp(v, "edit-pin")) {
        lv_obj_send_event(host_edit_button(host_row(hosts_body(s), "build-box")), LV_EVENT_CLICKED, NULL); step(d, 200);
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_host_form_create) && find_label(s, "Edit host", false));
        lv_obj_t *fp = find_label(s, PREVIEW_HOST_FP, false);
        if (!fp || !find_label(lv_obj_get_parent(fp), "Forget", false) || !find_label(s, "Host key", false) || !find_label(s, "Delete host", false)) {
            printf("check %s: FAIL the Host key row, Forget or Delete host\n", name); return 1;
        }
        assert(lv_obj_get_style_text_font(fp, 0) == FONT_MONO);
        r |= pv_run_checks(d, s, name, false, 0);
        /* M8: the store changes under the form and the screen is rebuilt (`ui refresh`): still build-box */
        server_t extra = { .name = "aaa-first", .host = "10.9.9.9", .port = 22, .user = "x", .use_key = true, .keypath = "device" };
        memmove(&s_preview_servers[1], &s_preview_servers[0], sizeof(server_t) * (size_t)s_preview_server_n);
        s_preview_servers[0] = extra; s_preview_server_n++;
        ui_nav_refresh(); step(d, 200);
        s = lv_screen_active();
        if (!ui_nav_is_top(ui_host_form_create) || strcmp(form_value(s, "Name"), "build-box")) { printf("check %s: FAIL the re-create opened \"%s\"\n", name, form_value(s, "Name")); return 1; }
        memmove(&s_preview_servers[0], &s_preview_servers[1], sizeof(server_t) * (size_t)(s_preview_server_n - 1)); s_preview_server_n--;
        ui_nav_refresh(); step(d, 200);
        s = lv_screen_active();
        assert(!strcmp(form_value(s, "Name"), "build-box"));
        click_label(d, lv_obj_get_child(s, 1), PREVIEW_HOST_FP);
        if (!find_label(top, "Forget this host's key?", false) ||
            !find_label(top, "Only do this if the host's SSH key changed. The Tab5 will trust the next key it sees.", false) || s_kh_forget_calls) {
            printf("check %s: FAIL the forget confirm\n", name); return 1;
        }
        r |= pv_run_checks(d, s, "hostform-forget", false, 0);
        click_label(d, top, "Forget");
        if (s_kh_forget_calls != 1 || find_label(s, PREVIEW_HOST_FP, false)) { printf("check %s: FAIL forget called %d times\n", name, s_kh_forget_calls); return 1; }
        lv_obj_clean(top);
        /* the store changes under the form (an entry before it goes): Delete still removes build-box */
        memmove(&s_preview_servers[1], &s_preview_servers[2], sizeof(server_t)); s_preview_server_n--;
        click_row(d, lv_obj_get_child(s, 1), "Delete host");
        assert(find_label(top, "Delete build-box?", false) && !s_store_removes);
        /* stop review I1: Left then Enter cancels; the arrow never reaches Delete */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_LEFT });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 50);
        if (s_store_removes || find_label(top, "Delete build-box?", false)) { printf("check %s: FAIL Left, Enter on the Delete confirm\n", name); return 1; }
        /* by keyboard: Tab to Delete, then Enter (Cancel has the focus after a key) */
        click_row(d, lv_obj_get_child(s, 1), "Delete host");
        assert(find_label(top, "Delete build-box?", false));
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB });
        lv_obj_t *fz = ui_keyboard_focused();
        if (!fz || !find_label(fz, "Delete", false)) { printf("check %s: FAIL Tab didn't reach Delete\n", name); return 1; }
        /* (Esc instead of Enter: the touch leg below removes it) */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 50);
        assert(!s_store_removes);
        click_row(d, lv_obj_get_child(s, 1), "Delete host");
        assert(find_label(top, "Delete build-box?", false));
        click_label(d, top, "Delete");
        if (s_store_removes != 1 || s_preview_server_n != 1 || !ui_nav_is_top(ui_hosts_create)) { printf("check %s: FAIL delete\n", name); return 1; }
        printf("%s: fingerprint in mono with Forget; forget once after OK; delete confirmed, by identity\n", name);
        return r;
    }
    if (!strcmp(v, "sd-key")) {
        lv_obj_send_event(host_edit_button(host_row(hosts_body(s), "pi-lan")), LV_EVENT_CLICKED, NULL); step(d, 200);
        s = lv_screen_active();
        if (strcmp(form_value(s, "Sign in with"), "Key file on SD card")) { printf("check %s: FAIL sign in reads %s\n", name, form_value(s, "Sign in with")); return 1; }
        assert(!find_label_has(s, "/sdcard", 0) && !find_shown(s, &lv_textarea_class) && !row_labeled(lv_obj_get_child(s, 1), "Password"));   /* read-only */
        r |= pv_run_checks(d, s, name, false, 0);
        click_row(d, lv_obj_get_child(s, 1), "Sign in with");
        assert(sheet_rows("Sign in with") == 3);
        click_label(d, top, "Key file on SD card");
        assert(!strcmp(form_value(s, "Sign in with"), "Key file on SD card"));
        press_key(SK_ESC, 0, false); step(d, 200);   /* unchanged: leaves at once */
        assert(ui_nav_is_top(ui_hosts_create));
        printf("%s: Key file on SD card, read-only, kept as a choice\n", name);
        return r;
    }
    if (!strcmp(v, "settings")) {
        ui_nav_back(); step(d, 200);
        ui_settings_open(UI_SET_HOST); step(d, 200);
        click_row(d, settings_panel(lv_screen_active()), "Address");
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_host_form_create) && find_clickable_shown(s, "\xe2\x86\x90 Settings") && find_label(s, "Edit host", false));
        if (find_label(s, "Delete host", false) || find_label(s, "Host key", false)) { printf("check %s: FAIL Delete or Host key on the Host's entry\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        form_set(d, s, "User", "me2");
        click_label(d, s, "Save");
        if (s_store_updates != 1 || s_set_server_calls != 1 || strcmp(s_set_server_name, "Desk") || s_rename_calls || !ui_nav_is_top(ui_settings_create)) {
            printf("check %s: FAIL the Host's entry save (updates %d, set_server %d)\n", name, s_store_updates, s_set_server_calls); return 1;
        }
        /* M1: a rename of the Host's entry follows it (no carry-clearing switch of Host) */
        click_row(d, settings_panel(lv_screen_active()), "Address");
        s = lv_screen_active();
        form_set(d, s, "Name", "Studio");
        click_label(d, s, "Save");
        if (s_rename_calls != 1 || strcmp(s_rename_old, "Desk") || strcmp(s_rename_new, "Studio") || strcmp(s_set_server_name, "Studio")) {
            printf("check %s: FAIL the rename (%d calls)\n", name, s_rename_calls); return 1;
        }
        /* the Host picker's Add host…: the new entry becomes the Host (P4-6) */
        click_row(d, settings_panel(lv_screen_active()), "Host");
        click_label(d, top, "Add host\xe2\x80\xa6");
        s = lv_screen_active();
        assert(ui_nav_is_top(ui_host_form_create));
        form_set(d, s, "Address", "studio.local");
        form_set(d, s, "User", "me");
        form_set(d, s, "Name", "pi-lan");   /* I1: the picker can't add a second "pi-lan" (it would pick the wrong entry) */
        click_label(d, s, "Save");
        if (!find_label(top, "Another host already has that name", false) || s_store_adds) { printf("check %s: FAIL a taken name from the picker\n", name); return 1; }
        lv_obj_clean(top);
        form_set(d, s, "Name", "");
        click_label(d, s, "Save");
        if (s_store_adds != 1 || s_set_server_calls != 3 || strcmp(s_set_server_name, "me@studio.local")) { printf("check %s: FAIL picker add\n", name); return 1; }
        printf("%s: the Host's entry from Settings (no Delete, no Host key), save tells the link; picker add sets the Host\n", name);
        return r;
    }
    fprintf(stderr, "preview: unknown hostform variant %s\n", v);
    return 2;
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

/* T13-3: a rebuild of Settings › Network (the Tailscale switch's refill, an event's) or of Peers keeps the keyboard
   focus on its row, found again by its label, so a bare Enter never falls to "← Sessions". */
static bool focused_in(lv_obj_t *row) { lv_obj_t *f = ui_keyboard_focused(); while (f && f != row) f = lv_obj_get_parent(f); return f != NULL; }
static int network_focus_case(lv_display_t *d) {
    const char *name = "network-focus";
    g_settings.ts_enable = 0;
    preview_sessions_load();
    ui_nav_home(); step(d, 200);
    ui_settings_open(UI_SET_NETWORK); step(d, 200);
    lv_obj_t *s = lv_screen_active(), *back = find_clickable_shown(s, "\xe2\x86\x90 Sessions");
    lv_obj_t *sw = row_labeled(settings_panel(s), "Tailscale");   /* the row is the switch's control */
    int n = 0;
    while (!lv_obj_has_state(sw, LV_STATE_FOCUS_KEY) && n < 30) { press_key(SK_DOWN, 0, false); step(d, 50); n++; }
    if (!lv_obj_has_state(sw, LV_STATE_FOCUS_KEY)) { printf("check %s: FAIL the switch never focused\n", name); return 1; }
    press_key(SK_ENTER, 0, false); step(d, 200);   /* on: saved, toasted and the panel refilled */
    lv_obj_t *panel = settings_panel(s);
    lv_obj_t *sw2 = row_labeled(panel, "Tailscale");
    if (!g_settings.ts_enable) { printf("check %s: FAIL the switch didn't turn on\n", name); return 1; }
    if (ui_keyboard_focused() != sw2 || !lv_obj_has_state(sw2, LV_STATE_FOCUS_KEY) || lv_obj_has_state(back, LV_STATE_FOCUS_KEY)) {
        printf("check %s: FAIL the focus after the switch's refill is not on the switch\n", name); return 1;
    }
    /* a refill as an event makes it: the focused row (Device name) is found again by its label */
    while (!focused_in(row_labeled(panel, "Device name")) && n < 60) { press_key(SK_DOWN, 0, false); step(d, 50); n++; }
    assert(focused_in(row_labeled(panel, "Device name")));
    ui_settings_refill(); step(d, 100);
    panel = settings_panel(s);
    if (!focused_in(row_labeled(panel, "Device name"))) { printf("check %s: FAIL a refill lost the Device name row's focus\n", name); return 1; }
    /* and a bare Enter acts on that row, never "← Sessions" */
    press_key(SK_ENTER, 0, false); step(d, 200);
    if (!ui_nav_is_top(ui_settings_create) || !find_shown(lv_layer_top(), &lv_textarea_class)) {
        printf("check %s: FAIL Enter after the refill didn't open Device name\n", name); return 1;
    }
    press_key(SK_ESC, 0, false); step(d, 200);
    int r = pv_run_checks(d, s, name, false, 0);
    /* Peers: a Tailscale event rebuilds the list; the focused peer keeps the focus */
    ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)UI_NET_SUB_PEERS); step(d, 200);
    lv_obj_t *ps = lv_screen_active(), *body = lv_obj_get_child(ps, 1);
    lv_obj_t *pb = lv_obj_get_parent(find_label(body, "preview-host-b", false));
    while (pb && lv_obj_get_parent(pb) != body) pb = lv_obj_get_parent(pb);
    int m = 0;
    while (!focused_in(pb) && m < 20) { press_key(SK_DOWN, 0, false); step(d, 50); m++; }
    if (!focused_in(pb)) { printf("check %s: FAIL preview-host-b never focused\n", name); return 1; }
    assert(s_ts_cb);
    s_ts_cb(s_ts_cb_user); step(d, 200);
    lv_obj_t *pb2 = lv_obj_get_parent(find_label(body, "preview-host-b", false));
    while (pb2 && lv_obj_get_parent(pb2) != body) pb2 = lv_obj_get_parent(pb2);
    if (!focused_in(pb2)) { printf("check %s: FAIL a Tailscale event lost the peer's focus\n", name); return 1; }
    if (lv_obj_has_state(find_clickable_shown(ps, "\xe2\x86\x90 Settings"), LV_STATE_FOCUS_KEY)) { printf("check %s: FAIL focus on Back\n", name); return 1; }
    printf("%s: the switch, Device name and a peer keep the focus across their rebuilds\n", name);
    g_settings.ts_enable = 0;
    return r;
}

/* ------------------------------------------------------------ take-over and SSH session (Task 10) */
static int s_takeover_closed;
static void takeover_closed_cb(void *ctx) { (void)ctx; s_takeover_closed++; }
/* Timers under 300 ms that run, LVGL's own and the input devices' left out. */
static int fast_timers(unsigned period) {
    int n = 0;
    lv_display_t *d = lv_display_get_default();
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        if (t->paused || t == lv_display_get_refr_timer(d) || t == lv_anim_get_timer()) continue;
        bool indev = false;
        for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) if (t == lv_indev_get_read_timer(in)) indev = true;
        if (!indev && t->period == period) n++;
    }
    return n;
}
/* The session bar: the bar that holds the "← Leave" chip. */
static lv_obj_t *session_bar_of(lv_obj_t *scr) {
    lv_obj_t *l = find_label(scr, "\xe2\x86\x90 Leave", false);
    return l ? lv_obj_get_parent(lv_obj_get_parent(l)) : NULL;
}
/* As keyboard_dispatch: the UI's routing first (a modal takes every key), then the terminal's sink. */
static void pv_key(key_event_t ev) {
    if (!ui_keyboard_route(&ev, preview_have_sink) && preview_have_sink && !ui_keyboard_owns_input()) preview_sink.key(&ev, preview_sink.user);
}
static void tap(lv_display_t *d, lv_indev_t *in, int x, int y) {
    pointer_move(in, x, y, true); pointer_move(in, x, y, false); step(d, 20);
}
static const char k_takeover_sample[] =
    "\x1b[38;2;215;119;87m \xe2\x96\x90\xe2\x96\x9b\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x9c\xe2\x96\x8c\x1b[0m   Claude Code\r\n"
    "\x1b[38;2;215;119;87m\xe2\x96\x9d\xe2\x96\x9c\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x9b\xe2\x96\x98\x1b[0m  \x1b[90m~/src/companion-test\x1b[0m\r\n\r\n"
    "\x1b[1m\xe2\x8f\xba\x1b[0m What is your favourite colour?\r\n\r\n"
    "\x1b[90m\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\x1b[0m\r\n"
    "\x1b[90m\xe2\x94\x82\x1b[0m > \x1b[32mgreen\x1b[0m, \x1b[34mblue\x1b[0m or \x1b[31mred\x1b[0m\r\n";

static int takeover_case(lv_display_t *d, const char *v) {
    char name[48];
    snprintf(name, sizeof(name), "takeover%s%s", v[0] ? "-" : "", v);
    bool nokbd = !strcmp(v, "nokbd");
    if (nokbd) setenv("PREVIEW_KBD", "0", 1);
    lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, pointer_read);
    s_ssh_autoclose = false;
    ui_nav_home(); step(d, 200);
    ui_terminal_open_command(0, "~/.local/bin/claude attach 1a2b3c4d", "companion-test-colour", takeover_closed_cb, NULL);
    step(d, 20);
    lv_obj_t *s = lv_screen_active(), *bar = session_bar_of(s);
    if (!ui_terminal_is_top() || !bar) { printf("check %s: FAIL no take-over with a session bar\n", name); return 1; }
    pv_ssh_feed(k_takeover_sample); step(d, 20);
    if (!obj_shown(bar) || !find_label(bar, "companion-test-colour", false)) { printf("check %s: FAIL the bar doesn't show Leave and the name\n", name); return 1; }
    if (lv_obj_get_height(bar) != UI_TK_SESSION_BAR_H) { printf("check %s: FAIL the bar is %d px\n", name, (int)lv_obj_get_height(bar)); return 1; }
    lv_obj_t *esc = find_clickable_shown(bar, "Esc");
    assert(esc && lv_obj_get_height(esc) == UI_TK_BAR_CTRL && find_clickable_shown(bar, "Tab") && find_clickable_shown(bar, "^C") &&
           find_clickable_shown(bar, "\xe2\x86\x91") && find_clickable_shown(bar, "\xe2\x86\x93"));
    if ((find_clickable_shown(bar, LV_SYMBOL_KEYBOARD) != NULL) != nokbd) { printf("check %s: FAIL the keyboard chip with a keyboard %s\n", name, nokbd ? "absent" : "present"); return 1; }
    if (fast_timers(550) || fast_timers(16) != 1) { printf("check %s: FAIL timers: blink %d, 16 ms %d\n", name, fast_timers(550), fast_timers(16)); return 1; }
    if (pv_amber_pixels(s)) { printf("check %s: FAIL amber on the take-over\n", name); return 1; }
    int r = pv_run_checks(d, s, name, false, 0);   /* the bar up: touch targets, fit, idle, timers (only the 16 ms pump) */
    if (nokbd) {
        click_label(d, bar, LV_SYMBOL_KEYBOARD);
        if (!softkbd_visible() || lv_obj_get_height(lv_obj_get_child(s, 0)) >= 720) { printf("check %s: FAIL the keyboard chip\n", name); return 1; }
        click_label(d, bar, LV_SYMBOL_KEYBOARD);
        assert(!softkbd_visible());
        return r;
    }
    step(d, 3100);
    if (obj_shown(bar)) { printf("check %s: FAIL the bar is still up after 3 s\n", name); return 1; }
    if (fast_timers(3000)) { printf("check %s: FAIL the hide timer is still armed\n", name); return 1; }
    if (!strcmp(v, "idle")) {
        unsigned px = pv_idle_pixels(d, 2000);
        if (px) { printf("check %s: FAIL %u px redrawn with the bar hidden (a cursor blink?)\n", name, px); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        printf("%s: bar hidden after 3 s, 0 px redrawn over 2 s, no blink timer\n", name);
        return r;
    }
    /* no key shows the bar, and every key goes to the Host */
    size_t sent = s_ssh_sent_n;
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'a' }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'k', .mods = MOD_CTRL }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 't', .mods = MOD_CTRL }); step(d, 20);
    if (obj_shown(bar) || s_ssh_sent_n != sent + 3 || memcmp(s_ssh_sent + sent, "a\x0b\x14", 3)) { printf("check %s: FAIL a key showed the bar or didn't reach the Host\n", name); return 1; }
    if (lv_screen_active() != s || lv_obj_get_child_count(lv_layer_top())) { printf("check %s: FAIL Ctrl+K / Ctrl+T acted on the device\n", name); return 1; }
    /* a tap low on the screen does nothing; a tap in the top 80 px shows the bar for 3 s; neither sends a byte */
    tap(d, in, 640, 400);
    if (obj_shown(bar)) { printf("check %s: FAIL a tap at y=400 showed the bar\n", name); return 1; }
    tap(d, in, 640, 20);
    if (!obj_shown(bar)) { printf("check %s: FAIL a tap at y=20 didn't show the bar\n", name); return 1; }
    if (s_ssh_sent_n != sent + 3) { printf("check %s: FAIL a tap sent bytes\n", name); return 1; }
    step(d, 3100);
    assert(!obj_shown(bar));
    tap(d, in, 640, 70);   /* under the bar, still in the 80 px zone */
    assert(obj_shown(bar));
    /* the chips send their bytes */
    click_label(d, bar, "Esc"); click_label(d, bar, "Tab"); click_label(d, bar, "^C"); click_label(d, bar, "\xe2\x86\x91"); click_label(d, bar, "\xe2\x86\x93");
    if (memcmp(s_ssh_sent + sent + 3, "\x1b\t\x03\x1b[A\x1b[B", 9)) { printf("check %s: FAIL the chips' bytes\n", name); return 1; }
    /* link lost: the bar stays up, red; back: hides after 3 s */
    setenv("PREVIEW_WIFI", "0", 1); step(d, 20);
    if (!find_label(bar, "Link lost, reconnecting\xe2\x80\xa6", false)) { printf("check %s: FAIL no link-lost state\n", name); return 1; }
    r |= pv_run_checks(d, s, "takeover-link-lost", false, 0);
    step(d, 3100);
    assert(obj_shown(bar));
    setenv("PREVIEW_WIFI", "1", 1); step(d, 20);
    assert(!find_label(bar, "Link lost, reconnecting\xe2\x80\xa6", false));
    step(d, 3100);
    assert(!obj_shown(bar));
    /* Ctrl+] leaves at once: nothing sent, the toast, the owner told, the pump gone */
    sent = s_ssh_sent_n;
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
    if (ui_terminal_is_top() || s_ssh_sent_n != sent || s_takeover_closed != 1) { printf("check %s: FAIL Ctrl+] didn't leave\n", name); return 1; }
    if (!find_label(lv_layer_top(), "Session still running on the Host", false)) { printf("check %s: FAIL no \"still running\" toast\n", name); return 1; }
    if (fast_timers(16)) { printf("check %s: FAIL the 16 ms pump outlived the take-over\n", name); return 1; }
    /* "← Leave" leaves the same way and sends nothing (review Minor 1) */
    ui_terminal_open_command(0, "~/.local/bin/claude attach 1a2b3c4d", "companion-test-colour", takeover_closed_cb, NULL);
    step(d, 20);
    sent = s_ssh_sent_n;
    click_label(d, session_bar_of(lv_screen_active()), "\xe2\x86\x90 Leave");
    if (ui_terminal_is_top() || s_ssh_sent_n != sent || s_takeover_closed != 2) { printf("check %s: FAIL Leave sent %d bytes or didn't leave\n", name, (int)(s_ssh_sent_n - sent)); return 1; }
    printf("%s: bar with Leave and the name, hidden after 3 s; y=20 and y=70 taps show it, y=400 and keys don't; chips send; Ctrl+] leaves\n", name);
    return r;
}

/* The host-key question outlives a screen that goes before it shows (review Minor 4): no prompt for a dead session,
   nothing freed touched (run under ASan). */
static int ssh_hostkey_case(lv_display_t *d) {
    const char *name = "ssh-hostkey";
    preview_store_init();
    server_t nk = { .name = "newkey-box", .host = "192.168.1.61", .port = 22, .user = "pi", .use_key = true, .keypath = "device" };
    s_preview_servers[s_preview_server_n++] = nk;
    int idx = s_preview_server_n - 1;
    ui_nav_home(); step(d, 200);
    ui_nav_push(ui_hosts_create, NULL); step(d, 200);
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx);   /* the question is queued for the next LVGL cycle */
    ui_nav_back();                                            /* ... and the screen goes first */
    step(d, 50); step(d, 50);
    if (find_label(lv_layer_top(), "Connect", false)) { printf("check %s: FAIL a host-key prompt for a session that is gone\n", name); return 1; }
    s_preview_server_n--;
    printf("check %s: ok (the queued question was dropped with its screen)\n", name);
    return 0;
}

static int ssh_case(lv_display_t *d, const char *v) {
    if (!strcmp(v, "hostkey")) return ssh_hostkey_case(d);
    const char *name = "ssh";
    s_ssh_autoclose = false;
    preview_store_init();
    server_t box = { .name = "box", .host = "192.168.1.60", .port = 22, .user = "pi", .use_key = true, .keypath = "device" };
    s_preview_servers[s_preview_server_n++] = box;
    int idx = s_preview_server_n - 1;
    ui_nav_home(); step(d, 200);
    ui_nav_push(ui_hosts_create, NULL); step(d, 200);
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);
    lv_obj_t *s = lv_screen_active(), *bar = session_bar_of(s);
    if (!ui_terminal_is_top() || !bar || !find_label(bar, "box", false)) { printf("check %s: FAIL no SSH session with a bar naming the host\n", name); return 1; }
    pv_ssh_feed("pi@box:~ $ "); step(d, 20);
    int r = pv_run_checks(d, s, name, false, 0);
    /* Ctrl+] through the sink asks; while it asks, keys go to the dialog, never to the box */
    size_t sent = s_ssh_sent_n;
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
    lv_obj_t *top = lv_layer_top();
    if (!find_label(top, "Disconnect from box?", false) || !find_clickable_shown(top, "Disconnect")) { printf("check %s: FAIL no \"Disconnect from box?\"\n", name); return 1; }
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'x' }); pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'c', .mods = MOD_CTRL }); step(d, 20);
    if (s_ssh_sent_n != sent) { printf("check %s: FAIL a key reached the box under the confirm\n", name); return 1; }
    r |= pv_run_checks(d, s, "ssh-confirm", false, 0);
    click_label(d, top, "Cancel");
    if (!ui_terminal_is_top() || find_label(top, "Disconnect from box?", false)) { printf("check %s: FAIL Cancel didn't stay\n", name); return 1; }
    /* Enter on the fresh dialog is Cancel (the shared confirm's first control): an accidental Ctrl+] then Enter keeps the session */
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 20);
    assert(ui_terminal_is_top() && !find_label(top, "Disconnect from box?", false));
    /* "← Leave" asks the same; Disconnect leaves, and neither sends a byte */
    sent = s_ssh_sent_n;
    click_label(d, bar, "\xe2\x86\x90 Leave");
    assert(find_label(top, "Disconnect from box?", false));
    int stops = s_ssh_stops;
    click_label(d, top, "Disconnect");
    if (ui_terminal_is_top() || !ui_nav_is_top(ui_hosts_create) || s_ssh_stops != stops + 1 || fast_timers(16)) { printf("check %s: FAIL Disconnect didn't leave\n", name); return 1; }
    if (s_ssh_sent_n != sent) { printf("check %s: FAIL Leave sent %d bytes\n", name, (int)(s_ssh_sent_n - sent)); return 1; }
    /* the bar's 3 s timer dies with the screen (review Minor 2): leave while it is armed, then step past it */
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);
    if (fast_timers(3000) != 1) { printf("check %s: FAIL %d bar timers armed after the connect (want 1: an earlier screen's outlived it?)\n", name, fast_timers(3000)); return 1; }
    click_label(d, session_bar_of(lv_screen_active()), "\xe2\x86\x90 Leave");
    click_label(d, top, "Disconnect");
    if (fast_timers(3000)) { printf("check %s: FAIL the bar timer outlived its screen\n", name); return 1; }
    step(d, 3200);
    /* a confirm raised for one session never acts on a later one (review Minor 3) */
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
    assert(find_label(top, "Disconnect from box?", false));
    pv_ssh_state(SSH_ST_CLOSED, NULL); step(d, 20);   /* the box ends it under the confirm */
    assert(ui_nav_is_top(ui_hosts_create) && find_label(top, "Disconnect from box?", false));
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);   /* a new session, opened from code */
    stops = s_ssh_stops;
    click_label(d, top, "Disconnect");   /* the stale answer */
    if (!ui_terminal_is_top() || s_ssh_stops != stops) { printf("check %s: FAIL a stale confirm closed a later session\n", name); return 1; }
    ui_nav_back(); step(d, 20);
    /* stop review I1: an arrow never reaches Disconnect: Ctrl+], Up, Enter (or Left, Enter) keeps the session */
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);
    for (int a = 0; a < 2; a++) {
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
        assert(find_label(top, "Disconnect from box?", false));
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = a ? SK_LEFT : SK_UP });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 20);
        if (!ui_terminal_is_top() || find_label(top, "Disconnect from box?", false)) { printf("check %s: FAIL an arrow then Enter disconnected\n", name); return 1; }
    }
    /* from the keyboard: Ctrl+], Tab (Cancel has the focus), Enter disconnects */
    pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_TAB });
    pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER }); step(d, 20);
    if (ui_terminal_is_top() || !ui_nav_is_top(ui_hosts_create)) { printf("check %s: FAIL Tab, Enter didn't disconnect\n", name); return 1; }
    /* the box ends the session: back to Terminal with a toast */
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)idx); step(d, 20);
    pv_ssh_state(SSH_ST_CLOSED, NULL); step(d, 20);
    if (!ui_nav_is_top(ui_hosts_create) || !find_label(top, "Disconnected from box", false)) { printf("check %s: FAIL the end didn't go back with a toast\n", name); return 1; }
    s_preview_server_n--;
    printf("%s: Ctrl+] asks \"Disconnect from box?\", keys stay out of the box, Cancel and Enter stay, Leave then Disconnect leaves, an arrow then Enter stays, Tab Enter leaves\n", name);
    return r;
}

/* ------------------------------------------------------------ voice input in a session (follow-up 2026-10-24) */
/* The session bar's 12 px link dot, red. */
static bool bar_dot_red(lv_obj_t *bar) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *o = lv_obj_get_child(bar, i);
        if (lv_obj_check_type(o, &lv_label_class) || lv_obj_get_width(o) != 12 || lv_obj_get_height(o) != 12) continue;
        return lv_color_eq(lv_obj_get_style_bg_color(o, 0), lv_color_hex(UI_TK_RED));
    }
    return false;
}
#define TV_REC_7 "Recording 0:07 / 0:30"
#define TV_TRANSCRIBING "Transcribing on the Host\xe2\x80\xa6"
#define TV_NOT_AT_PROMPT "Not typed: the session isn't at an empty prompt. Press Ctrl+Space there to type it."
#define TV_NOT_READY "Not typed: the session isn't ready. Press Ctrl+Space when it is to type it."
/* Claude Code's empty input line, and a permission dialog over it (synthetic, as the cc_cmd tests' screens) */
#define TV_PROMPT "\r\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\r\n\xe2\x9d\xaf \r\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\r\n  ? for shortcuts\r\n"
#define TV_DIALOG "\x1b[2J\x1b[H Do you want to make this edit?\r\n \xe2\x9d\xaf 1. Yes\r\n   2. Yes, and don't ask again\r\n   3. No, and tell Claude what to do differently (esc)\r\n Esc to cancel\r\n"
static void pv_ctrl_space(lv_display_t *d) { pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ' ', .mods = MOD_CTRL }); step(d, 20); }
/* Ctrl+Space in the terminal on screen starts a recording (after the 700 ms toggle guard); the stub records ms. */
static bool pv_term_record(lv_display_t *d, int ms, char *key) {
    step(d, 800);
    int starts = s_voice_start_calls;
    size_t sent = s_ssh_sent_n;
    pv_ctrl_space(d);
    if (s_voice_start_calls != starts + 1 || s_pv_voice.phase != VOICE_STARTING || s_ssh_sent_n != sent) return false;
    if (strncmp(s_pv_voice.key, "term:", 5)) return false;
    snprintf(key, VR_KEY_MAX, "%s", s_pv_voice.key);
    s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = ms; s_pv_voice.meter = 60;
    step(d, 20);
    return true;
}
static bool pv_term_stop(lv_display_t *d) {
    step(d, 800);
    int stops = s_voice_stop_calls;
    pv_ctrl_space(d);
    return s_voice_stop_calls == stops + 1 && s_pv_voice.phase == VOICE_TRANSCRIBING;
}
static void pv_voice_reset(void) {
    memset(&s_pv_voice, 0, sizeof(s_pv_voice));
    memset(&s_pv_vres, 0, sizeof(s_pv_vres)); s_pv_vres_ready = false;
}
static lv_obj_t *pv_open_takeover(lv_display_t *d) {
    s_ssh_autoclose = false;
    ui_nav_home(); step(d, 200);
    ui_terminal_open_command(0, "~/.local/bin/claude attach 1a2b3c4d", "companion-test-colour", takeover_closed_cb, NULL);
    step(d, 20);
    assert(ui_terminal_is_top());
    return lv_screen_active();
}
static lv_obj_t *pv_open_ssh(lv_display_t *d) {
    s_ssh_autoclose = false;
    preview_store_init();
    server_t box = { .name = "box", .host = "192.168.1.60", .port = 22, .user = "pi", .use_key = true, .keypath = "device" };
    s_preview_servers[s_preview_server_n++] = box;
    ui_nav_home(); step(d, 200);
    ui_nav_push(ui_hosts_create, NULL); step(d, 200);
    ui_nav_push(ui_terminal_create, (void *)(intptr_t)(s_preview_server_n - 1)); step(d, 20);
    assert(ui_terminal_is_top());
    return lv_screen_active();
}
static bool sent_has_line_end(size_t from) {
    for (size_t i = from; i < s_ssh_sent_n; i++) if (s_ssh_sent[i] == '\r' || s_ssh_sent[i] == '\n') return true;
    return false;
}

/* The session bar's state slot (after ← Leave, the name and the dot) holds text whole: its width fits the text. */
static bool pv_state_fits(const char *name, lv_obj_t *bar, const char *text) {
    lv_obj_update_layout(bar);
    lv_obj_t *st = lv_obj_get_child(bar, 3);
    lv_point_t sz;
    lv_text_get_size(&sz, text, lv_obj_get_style_text_font(st, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    printf("%s: state slot %d px for \"%s\" (%d px)\n", name, (int)lv_obj_get_width(st), text, (int)sz.x);
    if (strncmp(lv_label_get_text(st), text, 4) || sz.x > lv_obj_get_width(st)) {
        printf("check %s: FAIL \"%s\" (%d px) is cut in a %d px slot (reads \"%s\")\n", name, text, (int)sz.x, (int)lv_obj_get_width(st), lv_label_get_text(st));
        return false;
    }
    return true;
}
const vt_t *ui_terminal_preview_vt(void);
/* The open terminal's screen holds text (the banner). */
static bool pv_screen_has(const char *text) {
    static char t[16384];
    const vt_t *vt = ui_terminal_preview_vt();
    if (!vt) return false;
    vt_screen_text(vt, t, sizeof(t));
    return strstr(t, text) != NULL;
}

/* Scrollback (follow-up 2026-10-25): Ctrl+Up / Ctrl+Down, the Tab5 keyboard's scroll keys, and by touch the bar's
   scroll chips and its "Scrolled back" chip. */
#define TS_SCROLLED "Scrolled back \xc2\xb7 Live \xe2\x86\x93"
#define TS_UP "\xe2\x87\x91"
#define TS_DOWN "\xe2\x87\x93"
static void pv_feed_lines(int n) {
    char l[40];
    for (int i = 0; i < n; i++) { snprintf(l, sizeof(l), "history line %03d\r\n", i); pv_ssh_feed(l); }
}
static int pv_view_off(void) { return vt_view_offset(ui_terminal_preview_vt()); }
static int pv_view_rows(void) { return vt_rows(ui_terminal_preview_vt()); }
/* The bar shows the live chips (↑ ↓) or the scrolled-back ones (⇑ ⇓ and the "Scrolled back" chip), never both. */
static bool pv_chip_shown(lv_obj_t *bar, const char *t) { lv_obj_t *c = find_clickable_shown(bar, t); return c && obj_shown(c); }
static bool pv_scroll_chips(lv_obj_t *bar, bool scrolled, bool indicator) {
    bool up = pv_chip_shown(bar, "\xe2\x86\x91"), down = pv_chip_shown(bar, "\xe2\x86\x93");
    bool sup = pv_chip_shown(bar, TS_UP), sdown = pv_chip_shown(bar, TS_DOWN), ind = pv_chip_shown(bar, TS_SCROLLED);
    return up == !scrolled && down == !scrolled && sup == scrolled && sdown == scrolled && ind == indicator;
}
static int term_scroll_case(lv_display_t *d, const char *v) {
    char name[48];
    snprintf(name, sizeof(name), "terminal-%s", v);
    int r = 0;
    pv_voice_reset();
    if (!strcmp(v, "scroll-keys")) {
        /* a take-over: Claude Code draws on the alternate screen and scrolls itself, so the scroll keys send it PgUp /
           PgDn (Ctrl+Up/Down, and from a USB keyboard Shift+Up/Down and Shift+PgUp/PgDn), the local view stays live and
           the bar never says "Scrolled back"; Alt+Up stays Alt+Up */
        {
            lv_obj_t *s = pv_open_takeover(d), *bar = session_bar_of(s);
            assert(bar);
            pv_ssh_feed("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h"); pv_feed_lines(120); pv_ssh_feed(TV_PROMPT); step(d, 20);
            static const struct { key_event_t ev; const char *want; const char *what; } k[] = {
                { { .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }, "\x1b[5~", "Ctrl+Up" },
                { { .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }, "\x1b[5~", "a second Ctrl+Up" },
                { { .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_CTRL }, "\x1b[6~", "Ctrl+Down" },
                { { .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL | MOD_SHIFT }, "\x1b[5~", "Ctrl+Shift+Up" },
                { { .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_SHIFT }, "\x1b[5~", "Shift+Up" },
                { { .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_SHIFT }, "\x1b[6~", "Shift+Down" },
                { { .type = KEY_EV_SPECIAL, .key = SK_PGUP, .mods = MOD_SHIFT }, "\x1b[5~", "Shift+PgUp" },
                { { .type = KEY_EV_SPECIAL, .key = SK_PGDN, .mods = MOD_SHIFT }, "\x1b[6~", "Shift+PgDn" },
                { { .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_ALT }, "\x1b[1;3A", "Alt+Up" },
            };
            for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
                size_t sent = s_ssh_sent_n, n = strlen(k[i].want);
                pv_key(k[i].ev); step(d, 20);
                if (s_ssh_sent_n != sent + n || memcmp(s_ssh_sent + sent, k[i].want, n)) {
                    printf("check %s: FAIL %s in a take-over sent %d bytes, not the %d of its sequence\n", name, k[i].what, (int)(s_ssh_sent_n - sent), (int)n); return 1;
                }
                if (pv_view_off() != 0 || !pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL %s moved the local view of a take-over (%d)\n", name, k[i].what, pv_view_off()); return 1; }
            }
            r |= pv_run_checks(d, s, "terminal-scroll-keys-takeover", false, 0);
            ui_nav_back(); step(d, 20);
        }
        /* an SSH session: Ctrl+Up scrolls the local view back half a screen, Ctrl+Down forward, and neither sends a
           byte; Alt+Up and a typed key go to the Host and return to the live view */
        for (int mode = 1; mode < 2; mode++) {
            const char *what = mode ? "SSH session" : "take-over";
            lv_obj_t *s = mode ? pv_open_ssh(d) : pv_open_takeover(d), *bar = session_bar_of(s);
            assert(bar);
            pv_feed_lines(120); pv_ssh_feed(mode ? "pi@box:~ $ " : TV_PROMPT); step(d, 20);
            int half = pv_view_rows() / 2;
            assert(half > 0 && pv_view_off() == 0);
            size_t sent = s_ssh_sent_n;
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }); step(d, 20);
            if (pv_view_off() != half) { printf("check %s: FAIL Ctrl+Up in a %s scrolled to %d, not %d\n", name, what, pv_view_off(), half); return 1; }
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }); step(d, 20);
            if (pv_view_off() != 2 * half) { printf("check %s: FAIL a second Ctrl+Up in a %s: %d\n", name, what, pv_view_off()); return 1; }
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_CTRL }); step(d, 20);
            if (pv_view_off() != half) { printf("check %s: FAIL Ctrl+Down in a %s: %d, not %d\n", name, what, pv_view_off(), half); return 1; }
            if (s_ssh_sent_n != sent) { printf("check %s: FAIL Ctrl+Up/Down sent %d bytes to the Host in a %s\n", name, (int)(s_ssh_sent_n - sent), what); return 1; }
            if (!obj_shown(bar) || !pv_scroll_chips(bar, true, true)) { printf("check %s: FAIL the bar doesn't say the %s is scrolled back\n", name, what); return 1; }
            r |= pv_run_checks(d, s, name, false, 0);
            /* the Shift keys stay: Shift+PgUp in both, Shift+Up in a take-over only */
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_PGUP, .mods = MOD_SHIFT }); step(d, 20);
            if (pv_view_off() != 2 * half || s_ssh_sent_n != sent) { printf("check %s: FAIL Shift+PgUp in a %s\n", name, what); return 1; }
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_CTRL }); step(d, 20);
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_CTRL }); step(d, 20);
            if (pv_view_off() != 0 || !pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL Ctrl+Down back to the bottom in a %s: %d\n", name, what, pv_view_off()); return 1; }
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_DOWN, .mods = MOD_CTRL }); step(d, 20);
            if (pv_view_off() != 0 || s_ssh_sent_n != sent) { printf("check %s: FAIL Ctrl+Down at the bottom in a %s\n", name, what); return 1; }
            /* Alt+Up is the Host's: sent, and the view goes live */
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }); step(d, 20);
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_ALT }); step(d, 20);
            if (s_ssh_sent_n == sent || pv_view_off() != 0) { printf("check %s: FAIL Alt+Up in a %s wasn't sent, or left the view scrolled (%d)\n", name, what, pv_view_off()); return 1; }
            /* a typed key while scrolled back is sent and returns to the live view */
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_CTRL }); step(d, 20);
            assert(pv_view_off() == half);
            sent = s_ssh_sent_n;
            pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'a' }); step(d, 20);
            if (s_ssh_sent_n != sent + 1 || s_ssh_sent[sent] != 'a' || pv_view_off() != 0) { printf("check %s: FAIL a typed key in a %s: %d bytes, view %d\n", name, what, (int)(s_ssh_sent_n - sent), pv_view_off()); return 1; }
            if (!pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL the bar still says scrolled back after a typed key\n", name); return 1; }
            /* Ctrl+Right is the Host's (the leave key is Ctrl+Left) */
            sent = s_ssh_sent_n;
            pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_RIGHT, .mods = MOD_CTRL }); step(d, 20);
            if (s_ssh_sent_n == sent || !ui_terminal_is_top()) { printf("check %s: FAIL Ctrl+Right in a %s\n", name, what); return 1; }
            ui_nav_back(); step(d, 20);
            if (mode) {
                lv_obj_t *top = lv_layer_top();
                if (find_clickable_shown(top, "Disconnect")) click_label(d, top, "Disconnect");
                s_preview_server_n--;
            }
        }
        printf("%s: in a take-over Ctrl+Up/Down (and Shift+Up/Down, Shift+PgUp/PgDn) send Claude Code PgUp/PgDn and leave the local view live, Alt+Up stays Alt+Up; in an SSH session Ctrl+Up/Down scroll half a screen and send nothing, Shift+PgUp still scrolls, Alt+Up, Ctrl+Right and a typed key go to the Host, and a typed key returns to the live view\n", name);
        return r;
    }
    if (!strcmp(v, "scroll-drag")) {
        /* a take-over of Claude Code, which has turned mouse reporting on: a drag sends it one SGR wheel report per text
           row (finger down = wheel up, back), at the cell the press began on; the local view stays live */
        lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, pointer_read);
        lv_obj_t *s = pv_open_takeover(d), *bar = session_bar_of(s);
        assert(bar);
        pv_ssh_feed("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h"); pv_feed_lines(120); pv_ssh_feed(TV_PROMPT); step(d, 20);
        int cell_h = lv_font_get_line_height(&g_term_font), cell_w = lv_font_get_glyph_width(&g_term_font, 'M', 0);
        char want[32];
        for (int dir = 0; dir < 2; dir++) {
            int y0 = dir ? 500 : 300, y1 = dir ? 300 : 500;
            size_t sent = s_ssh_sent_n;
            pointer_move(in, 640, y0, true); step(d, 20);
            pointer_move(in, 640, (y0 + y1) / 2, true); step(d, 20);
            pointer_move(in, 640, y1, true); step(d, 20);
            pointer_move(in, 640, y1, false); step(d, 20);
            snprintf(want, sizeof(want), "\x1b[<%d;%d;%dM", dir ? 65 : 64, 640 / cell_w + 1, y0 / cell_h + 1);
            size_t wn = strlen(want);
            int rows = 200 / cell_h, got = 0;
            for (size_t i = sent; i + wn <= s_ssh_sent_n && !memcmp(s_ssh_sent + i, want, wn); i += wn) got++;
            if (got != rows || s_ssh_sent_n != sent + (size_t)rows * wn) {
                printf("check %s: FAIL a %d px drag %s sent %d bytes, %d of them \"ESC%s\" reports, not %d\n", name, 200, dir ? "up" : "down", (int)(s_ssh_sent_n - sent), got, want + 1, rows); return 1;
            }
            if (pv_view_off() != 0 || !pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL the drag moved the local view (%d)\n", name, pv_view_off()); return 1; }
        }
        r |= pv_run_checks(d, s, name, false, 0);
        ui_nav_back(); step(d, 20);
        lv_indev_delete(in);
        printf("%s: with mouse reporting on, a %d px drag in a take-over sends Claude Code %d wheel reports (down: back, up: forward) and the local view stays live\n", name, 200, 200 / cell_h);
        return r;
    }
    if (!strcmp(v, "scroll-touch")) {
        /* the narrowest bar (no keyboard: ⌨ shown; a name at the cap), as the voice-touch-nokbd case. A take-over before
           Claude Code turns mouse reporting on: the drag falls back to the local view, as in an SSH session */
        setenv("PREVIEW_KBD", "0", 1);
        lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, pointer_read);
        s_ssh_autoclose = false;
        ui_nav_home(); step(d, 200);
        ui_terminal_open_command(0, "~/.local/bin/claude attach 1a2b3c4d", "companion-test-a-session-name-long-enough-to-reach-the-cap", takeover_closed_cb, NULL);
        lv_obj_t *s = lv_screen_active(), *bar = session_bar_of(s);
        assert(ui_terminal_is_top() && bar);
        step(d, 20);
        pv_feed_lines(120); pv_ssh_feed(TV_PROMPT); step(d, 20);
        step(d, 3100);
        if (obj_shown(bar)) { printf("check %s: FAIL the bar is still up\n", name); return 1; }
        tap(d, in, 640, 20);
        if (!obj_shown(bar) || !pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL the live bar's chips\n", name); return 1; }
        lv_obj_t *mic = find_clickable_shown(bar, "Mic");
        assert(mic);
        lv_area_t ma0; lv_obj_get_coords(mic, &ma0);
        lv_obj_t *up0 = find_clickable_shown(bar, "\xe2\x86\x91");
        lv_area_t ua0; lv_obj_get_coords(up0, &ua0);
        step(d, 3100);
        assert(!obj_shown(bar));
        /* a drag down scrolls back: the bar comes up and says so, and ⇑ ⇓ stand where ↑ ↓ were */
        size_t sent = s_ssh_sent_n;
        pointer_move(in, 640, 300, true); step(d, 20);
        pointer_move(in, 640, 340, true); step(d, 20);
        pointer_move(in, 640, 500, true); step(d, 20);
        pointer_move(in, 640, 500, false); step(d, 20);
        int off = pv_view_off();
        if (off <= 0) { printf("check %s: FAIL a drag didn't scroll back\n", name); return 1; }
        if (!obj_shown(bar) || !pv_scroll_chips(bar, true, true)) { printf("check %s: FAIL the bar doesn't say the view is scrolled back\n", name); return 1; }
        lv_obj_t *sup = find_clickable_shown(bar, TS_UP), *sdown = find_clickable_shown(bar, TS_DOWN), *ind = find_clickable_shown(bar, TS_SCROLLED);
        lv_obj_update_layout(bar);
        lv_area_t a, ma; lv_obj_get_coords(sup, &a); lv_obj_get_coords(mic, &ma);
        if (a.x1 != ua0.x1 || ma.x1 != ma0.x1) { printf("check %s: FAIL the chips moved (scroll chip at %d, the up key was at %d; Mic %d, was %d)\n", name, (int)a.x1, (int)ua0.x1, (int)ma.x1, (int)ma0.x1); return 1; }
        lv_obj_t *chips[] = { sup, sdown, ind };
        for (int i = 0; i < 3; i++)
            if (lv_obj_get_height(chips[i]) != UI_TK_BAR_CTRL || !lv_obj_has_flag(chips[i], UI_BAR_CONTROL)) { printf("check %s: FAIL a scroll chip isn't a bar chip\n", name); return 1; }
        {
            lv_obj_t *il = lv_obj_get_child(ind, 0);
            lv_point_t sz;
            lv_text_get_size(&sz, lv_label_get_text(il), lv_obj_get_style_text_font(il, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            printf("%s: \"%s\" chip %d px for %d px of text; scroll chips %d and %d px\n", name, TS_SCROLLED, (int)lv_obj_get_width(ind), (int)sz.x, (int)lv_obj_get_width(sup), (int)lv_obj_get_width(sdown));
            if (sz.x > lv_obj_get_width(il)) { printf("check %s: FAIL the indicator is cut\n", name); return 1; }
        }
        lv_obj_t *nr = lv_obj_get_child(bar, 1);
        if (lv_obj_get_style_max_width(nr, 0) != 160) { printf("check %s: FAIL the name didn't give way to the indicator\n", name); return 1; }
        step(d, 3100);
        if (!obj_shown(bar)) { printf("check %s: FAIL the bar hid while scrolled back\n", name); return 1; }
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber while scrolled back\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        /* ⇑ half a screen back, ⇓ forward; nothing sent */
        int half = pv_view_rows() / 2;
        lv_obj_send_event(sup, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (pv_view_off() != off + half) { printf("check %s: FAIL the scroll-up chip: %d, not %d\n", name, pv_view_off(), off + half); return 1; }
        lv_obj_send_event(sdown, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (pv_view_off() != off) { printf("check %s: FAIL the scroll-down chip: %d, not %d\n", name, pv_view_off(), off); return 1; }
        if (s_ssh_sent_n != sent) { printf("check %s: FAIL a scroll chip or the drag sent %d bytes\n", name, (int)(s_ssh_sent_n - sent)); return 1; }
        /* a lost link takes the state slot (whole), the scroll chips stay; the link back gives the indicator back */
        setenv("PREVIEW_WIFI", "0", 1); step(d, 20);
        if (!pv_state_fits(name, bar, "Link lost, reconnecting\xe2\x80\xa6")) return 1;
        if (!pv_scroll_chips(bar, true, false)) { printf("check %s: FAIL the indicator over a lost link\n", name); return 1; }
        r |= pv_run_checks(d, s, "terminal-scroll-touch-link-lost", false, 0);
        setenv("PREVIEW_WIFI", "1", 1); step(d, 20);
        if (!pv_scroll_chips(bar, true, true)) { printf("check %s: FAIL the indicator didn't come back with the link\n", name); return 1; }
        /* a tap on the indicator returns to the bottom: the ↑ ↓ key chips return, nothing is sent, the bar hides in 3 s */
        lv_obj_send_event(ind, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (pv_view_off() != 0 || !pv_scroll_chips(bar, false, false) || s_ssh_sent_n != sent) { printf("check %s: FAIL the indicator's tap: view %d\n", name, pv_view_off()); return 1; }
        if (lv_obj_get_style_max_width(nr, 0) != 400) { printf("check %s: FAIL the name didn't get its 400 px back\n", name); return 1; }
        step(d, 3100);
        if (obj_shown(bar)) { printf("check %s: FAIL the bar stayed up at the bottom\n", name); return 1; }
        /* the Esc chip while scrolled back: sent, and the view goes live */
        tap(d, in, 640, 20);
        lv_obj_send_event(sup, LV_EVENT_CLICKED, NULL); step(d, 20);
        assert(pv_view_off() > 0);
        lv_obj_t *esc = find_clickable_shown(bar, "Esc");
        assert(esc);
        lv_obj_send_event(esc, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (s_ssh_sent_n != sent + 1 || s_ssh_sent[sent] != 0x1b || pv_view_off() != 0 || !pv_scroll_chips(bar, false, false)) { printf("check %s: FAIL the Esc chip while scrolled back\n", name); return 1; }
        if (fast_timers(16) != 1 || fast_timers(100)) { printf("check %s: FAIL timers: 16 ms %d, 100 ms %d\n", name, fast_timers(16), fast_timers(100)); return 1; }
        ui_nav_back(); step(d, 20);
        lv_indev_delete(in);
        printf("%s: a drag scrolls back and brings up the bar; at the narrowest bar the scroll chips take the up and down keys' places (every chip a bar chip, nothing moves), the indicator chip fits beside the narrowed name and gives way to a lost link; the chips scroll half a screen and send nothing; a tap on the indicator, or any key chip, returns to the live view\n", name);
        return r;
    }
    printf("preview: unknown terminal scroll case %s\n", v);
    return 2;
}

static int term_voice_case(lv_display_t *d, const char *v) {
    char name[48], key[VR_KEY_MAX];
    snprintf(name, sizeof(name), "terminal-%s", v);
    int r = 0;
    pv_voice_reset();
    if (!strcmp(v, "voice-rec")) {
        lv_obj_t *s = pv_open_takeover(d), *bar = session_bar_of(s);
        pv_ssh_feed(k_takeover_sample); step(d, 3100);
        assert(bar && !obj_shown(bar));
        /* Ctrl+Space starts a recording keyed to this screen and sends nothing (no NUL) */
        if (!pv_term_record(d, 7000, key)) { printf("check %s: FAIL Ctrl+Space didn't start a terminal recording, or sent a byte\n", name); return 1; }
        if (lv_obj_get_child_count(lv_layer_top())) { printf("check %s: FAIL the start made a toast (internal RAM while recording)\n", name); return 1; }
        lv_obj_t *ind = find_label(bar, TV_REC_7, false);
        if (!obj_shown(bar) || !ind || !label_red(ind) || !bar_dot_red(bar)) { printf("check %s: FAIL no red dot and \"" TV_REC_7 "\" in the bar\n", name); return 1; }
        step(d, 3100);
        if (!obj_shown(bar)) { printf("check %s: FAIL the bar hid while recording\n", name); return 1; }
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber while recording\n", name); return 1; }
        if (fast_timers(16) != 1 || fast_timers(100)) { printf("check %s: FAIL timers: 16 ms %d, 100 ms %d\n", name, fast_timers(16), fast_timers(100)); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        /* every key but Ctrl+Space, Esc and the leave keys is eaten; so are the bar's key chips */
        size_t sent = s_ssh_sent_n;
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'a' });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ENTER });
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'c', .mods = MOD_CTRL });
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = '@', .mods = MOD_CTRL });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_BACKSPACE });
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_UP, .mods = MOD_SHIFT });
        step(d, 20);
        click_label(d, bar, "Tab"); click_label(d, bar, "^C"); click_label(d, bar, "\xe2\x86\x91");
        if (s_ssh_sent_n != sent || s_voice_cancel_calls || s_pv_voice.phase != VOICE_RECORDING) {
            printf("check %s: FAIL %d bytes reached the Host while recording (or the recording ended)\n", name, (int)(s_ssh_sent_n - sent)); return 1;
        }
        /* the timer moves on the 16 ms pump */
        s_pv_voice.elapsed_ms = 12000; step(d, 20);
        if (!find_label(bar, "Recording 0:12 / 0:30", false)) { printf("check %s: FAIL the timer didn't move\n", name); return 1; }
        /* Esc cancels and sends nothing; the bar gives the session its state back and hides after 3 s */
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 20);
        if (s_voice_cancel_calls != 1 || s_pv_voice.phase != VOICE_IDLE || s_ssh_sent_n != sent) { printf("check %s: FAIL Esc: %d cancels, %d bytes sent\n", name, s_voice_cancel_calls, (int)(s_ssh_sent_n - sent)); return 1; }
        if (find_label(bar, "Recording", true) || bar_dot_red(bar)) { printf("check %s: FAIL the indicator outlived the cancel\n", name); return 1; }
        step(d, 3100);
        if (obj_shown(bar)) { printf("check %s: FAIL the bar stayed up after the cancel\n", name); return 1; }
        /* while transcribing, keys pass and the bar says so; Ctrl+Space says "Still transcribing…"; Esc cancels this
           screen's transcription and isn't sent (review M1), and with nothing pending Esc goes to the Host again */
        if (!pv_term_record(d, 2000, key) || !pv_term_stop(d)) { printf("check %s: FAIL the second recording didn't start and stop\n", name); return 1; }
        lv_obj_t *tl = find_label(bar, TV_TRANSCRIBING, false);
        if (!tl || !obj_shown(bar) || bar_dot_red(bar) || label_red(tl)) { printf("check %s: FAIL no muted \"" TV_TRANSCRIBING "\"\n", name); return 1; }
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber while transcribing\n", name); return 1; }
        r |= pv_run_checks(d, s, "terminal-voice-transcribing", false, 0);
        sent = s_ssh_sent_n;
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'a' }); step(d, 20);
        if (s_ssh_sent_n != sent + 1 || s_ssh_sent[sent] != 'a') { printf("check %s: FAIL keys didn't pass while transcribing\n", name); return 1; }
        int starts = s_voice_start_calls;
        step(d, 800); pv_ctrl_space(d);
        if (s_voice_start_calls != starts || !find_label(lv_layer_top(), "Still transcribing\xe2\x80\xa6", false)) { printf("check %s: FAIL Ctrl+Space while transcribing\n", name); return 1; }
        sent = s_ssh_sent_n;
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 20);
        if (s_voice_cancel_calls != 2 || s_ssh_sent_n != sent || find_label(bar, TV_TRANSCRIBING, false)) { printf("check %s: FAIL Esc while transcribing: %d cancels, %d bytes sent\n", name, s_voice_cancel_calls, (int)(s_ssh_sent_n - sent)); return 1; }
        pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_ESC }); step(d, 20);
        if (s_voice_cancel_calls != 2 || s_ssh_sent_n != sent + 1 || s_ssh_sent[sent] != 0x1b) { printf("check %s: FAIL Esc with nothing pending didn't reach the Host\n", name); return 1; }
        pv_voice_reset(); step(d, 20);
        /* leaving while transcribing cancels it; a result that comes after types nothing */
        if (!pv_term_record(d, 2000, key) || !pv_term_stop(d)) { printf("check %s: FAIL the third recording\n", name); return 1; }
        int cancels0 = s_voice_cancel_calls;
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
        if (ui_terminal_is_top() || s_voice_cancel_calls != cancels0 + 1) { printf("check %s: FAIL leaving didn't cancel the transcription (%d cancels)\n", name, s_voice_cancel_calls); return 1; }
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "Late words.", false); step(d, 400);
        if (s_ssh_sent_n != sent) { printf("check %s: FAIL a late result was typed\n", name); return 1; }
        pv_voice_reset();
        /* every leave while recording cancels, then leaves the take-over: Ctrl+←, Ctrl+Q and the "← Leave" chip */
        for (int how = 0; how < 3; how++) {
            s = pv_open_takeover(d); bar = session_bar_of(s);
            pv_ssh_feed(k_takeover_sample); step(d, 20);
            if (!pv_term_record(d, 4000, key)) { printf("check %s: FAIL no recording to leave (%d)\n", name, how); return 1; }
            int cancels = s_voice_cancel_calls, closed = s_takeover_closed;
            sent = s_ssh_sent_n;
            if (how == 0) pv_key((key_event_t){ .type = KEY_EV_SPECIAL, .key = SK_LEFT, .mods = MOD_CTRL });
            else if (how == 1) pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = 'q', .mods = MOD_CTRL });
            else click_label(d, bar, "\xe2\x86\x90 Leave");
            step(d, 20);
            if (ui_terminal_is_top() || s_voice_cancel_calls != cancels + 1 || s_takeover_closed != closed + 1 || s_ssh_sent_n != sent || s_pv_voice.phase != VOICE_IDLE) {
                printf("check %s: FAIL leave %s while recording: top %d, %d cancels, %d bytes\n", name, how == 0 ? "Ctrl+\xe2\x86\x90" : how == 1 ? "Ctrl+Q" : "chip",
                       ui_terminal_is_top(), s_voice_cancel_calls - cancels, (int)(s_ssh_sent_n - sent));
                return 1;
            }
            pv_voice_reset();
        }
        /* an SSH session: the same key and indicator; Ctrl+] while recording cancels, then asks */
        s = pv_open_ssh(d); bar = session_bar_of(s);
        pv_ssh_feed("pi@box:~ $ "); step(d, 20);
        char k2[VR_KEY_MAX];
        if (!pv_term_record(d, 7000, k2) || !strcmp(k2, key)) { printf("check %s: FAIL no SSH recording with its own key\n", name); return 1; }
        if (!find_label(bar, TV_REC_7, false) || !bar_dot_red(bar)) { printf("check %s: FAIL no indicator in the SSH bar\n", name); return 1; }
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber on the SSH recording\n", name); return 1; }
        r |= pv_run_checks(d, s, "terminal-voice-rec-ssh", false, 0);
        sent = s_ssh_sent_n;
        int cancels = s_voice_cancel_calls;
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
        lv_obj_t *top = lv_layer_top();
        if (s_voice_cancel_calls != cancels + 1 || !find_label(top, "Disconnect from box?", false) || s_ssh_sent_n != sent) { printf("check %s: FAIL Ctrl+] while recording didn't cancel and ask\n", name); return 1; }
        click_label(d, top, "Disconnect");
        assert(!ui_terminal_is_top());
        s_preview_server_n--;
        printf("%s: Ctrl+Space starts (no NUL sent), red dot and timer in the bar, bar kept up, keys and chips eaten, Esc cancels; transcribing passes keys; leaving cancels; SSH too\n", name);
        return r;
    }
    if (!strcmp(v, "voice-done")) {
        /* a take-over at Claude Code's empty prompt; the vt never saw DECSET 2004, and the text is still bracketed */
        lv_obj_t *s = pv_open_takeover(d), *bar = session_bar_of(s), *top = lv_layer_top();
        pv_ssh_feed(k_takeover_sample); pv_ssh_feed(TV_PROMPT); step(d, 3100);
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no recording\n", name); return 1; }
        size_t sent = s_ssh_sent_n;
        const char *text = "Please run\nthe tests.\r";
        pv_voice_post(key, VR_OUT_TEXT, text, false); step(d, 20);
        char want[VR_TEXT_MAX + 14];
        size_t wn = cc_voice_type_build_ex(text, true, false, want, sizeof(want));
        if (!wn || s_ssh_sent_n - sent != wn || memcmp(s_ssh_sent + sent, want, wn)) {
            printf("check %s: FAIL the pty got %d bytes, not the builder's %d\n", name, (int)(s_ssh_sent_n - sent), (int)wn); return 1;
        }
        if (memcmp(s_ssh_sent + sent, "\x1b[200~Please run the tests.\x1b[201~", wn) || sent_has_line_end(sent)) { printf("check %s: FAIL not bracketed, or an Enter or newline was written\n", name); return 1; }
        if (s_paste_calls) { printf("check %s: FAIL cc_link_paste was called\n", name); return 1; }
        if (find_label(bar, TV_TRANSCRIBING, false)) { printf("check %s: FAIL the indicator outlived the result\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        step(d, 3100);
        if (obj_shown(bar)) { printf("check %s: FAIL the bar stayed up after the result\n", name); return 1; }
        /* a second dictation in a take-over doesn't join (it types only at an empty prompt: re-review N1); the 30 s
           limit toasts after the text */
        if (!pv_term_record(d, 30000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no second recording\n", name); return 1; }
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "More.", true); step(d, 20);
        if (s_ssh_sent_n - sent != 17 || memcmp(s_ssh_sent + sent, "\x1b[200~More.\x1b[201~", 17) || !find_label(top, "Stopped at the 30 s limit", false)) { printf("check %s: FAIL the limit result\n", name); return 1; }
        /* an error types nothing and toasts */
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no third recording\n", name); return 1; }
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_NO_SPEECH, "", false); step(d, 20);
        if (s_ssh_sent_n != sent || !find_label(top, "No speech heard. Try again closer to the Tab5.", false)) { printf("check %s: FAIL the error result\n", name); return 1; }
        /* review I1: a permission dialog came up while transcribing. Nothing is typed (a "2" would pick option 2); the text
           is held, and Ctrl+Space types it once the prompt is back, without starting a recording */
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no dialog recording\n", name); return 1; }
        pv_ssh_feed(TV_DIALOG); step(d, 20);
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "2 more tests, then stop", false); step(d, 20);
        if (s_ssh_sent_n != sent || !find_label(top, TV_NOT_AT_PROMPT, false)) { printf("check %s: FAIL typed into a permission dialog (%d bytes)\n", name, (int)(s_ssh_sent_n - sent)); return 1; }
        r |= pv_run_checks(d, s, "terminal-voice-held", false, 0);
        int starts = s_voice_start_calls;
        step(d, 800); pv_ctrl_space(d);
        if (s_ssh_sent_n != sent || s_voice_start_calls != starts) { printf("check %s: FAIL Ctrl+Space at the dialog typed or recorded\n", name); return 1; }
        pv_ssh_feed("\x1b[2J\x1b[H"); pv_ssh_feed(TV_PROMPT); step(d, 20);
        step(d, 800); pv_ctrl_space(d);
        if (s_voice_start_calls != starts || s_ssh_sent_n - sent != 35 || memcmp(s_ssh_sent + sent, "\x1b[200~2 more tests, then stop\x1b[201~", 35)) {
            printf("check %s: FAIL the held text at the prompt: %d bytes\n", name, (int)(s_ssh_sent_n - sent)); return 1;
        }
        /* the held text is typed once: the next press records */
        step(d, 800); pv_ctrl_space(d);
        if (s_voice_start_calls != starts + 1) { printf("check %s: FAIL the press after a held text didn't record\n", name); return 1; }
        voice_cancel(); pv_voice_reset(); step(d, 20);
        /* a draft on the input line is not an empty prompt either; a held text older than 60 s is wiped */
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no draft recording\n", name); return 1; }
        pv_ssh_feed("\x1b[2J\x1b[H" "\xe2\x9d\xaf my own draft\r\n"); step(d, 20);
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "Lost later.", false); step(d, 20);
        if (s_ssh_sent_n != sent) { printf("check %s: FAIL typed over a draft\n", name); return 1; }
        step(d, 61000);
        pv_ssh_feed("\x1b[2J\x1b[H"); pv_ssh_feed(TV_PROMPT); step(d, 20);
        starts = s_voice_start_calls;
        pv_ctrl_space(d);
        if (s_ssh_sent_n != sent || s_voice_start_calls != starts + 1) { printf("check %s: FAIL a held text came back after 60 s\n", name); return 1; }
        voice_cancel(); pv_voice_reset(); step(d, 20);
        /* another screen's result (a Sessions key) is never taken or typed */
        if (!pv_term_record(d, 3000, key)) { printf("check %s: FAIL no foreign-key recording\n", name); return 1; }
        sent = s_ssh_sent_n;
        pv_voice_post("1a2b3c4d", VR_OUT_TEXT, "Not mine.", false); step(d, 20);
        if (s_ssh_sent_n != sent || !s_pv_vres_ready) { printf("check %s: FAIL a result for another key was taken\n", name); return 1; }
        pv_voice_reset();
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
        /* an SSH shell that never turned on bracketed paste gets the bare, sanitized text */
        s = pv_open_ssh(d);
        pv_ssh_feed("pi@box:~ $ "); step(d, 20);
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no SSH recording\n", name); return 1; }
        sent = s_ssh_sent_n;
        const char *t2 = "ls \x1b[201~ -la\n";
        pv_voice_post(key, VR_OUT_TEXT, t2, false); step(d, 20);
        wn = cc_voice_type_build_ex(t2, false, false, want, sizeof(want));
        if (s_ssh_sent_n - sent != wn || memcmp(s_ssh_sent + sent, "ls [201~ -la", wn) || sent_has_line_end(sent)) { printf("check %s: FAIL the SSH bytes\n", name); return 1; }
        /* review M3: the Disconnect confirm is up when the result comes: nothing typed behind it; held, and typed (joined)
           by Ctrl+Space after Cancel */
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no confirm recording\n", name); return 1; }
        pv_key((key_event_t){ .type = KEY_EV_CHAR, .cp = ']', .mods = MOD_CTRL }); step(d, 20);
        if (!find_label(top, "Disconnect from box?", false)) { printf("check %s: FAIL no confirm\n", name); return 1; }
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "cd src", false); step(d, 20);
        if (s_ssh_sent_n != sent || !find_label(top, TV_NOT_READY, false)) { printf("check %s: FAIL typed behind the confirm\n", name); return 1; }
        click_label(d, top, "Cancel");
        assert(ui_terminal_is_top());
        step(d, 800); pv_ctrl_space(d);
        if (s_ssh_sent_n - sent != 7 || memcmp(s_ssh_sent + sent, " cd src", 7)) { printf("check %s: FAIL the held text after Cancel\n", name); return 1; }
        /* the link lost when the result comes: held with a toast, typed when it is back */
        if (!pv_term_record(d, 3000, key) || !pv_term_stop(d)) { printf("check %s: FAIL no link-lost recording\n", name); return 1; }
        setenv("PREVIEW_WIFI", "0", 1); step(d, 20);
        sent = s_ssh_sent_n;
        pv_voice_post(key, VR_OUT_TEXT, "pwd", false); step(d, 20);
        if (s_ssh_sent_n != sent || !find_label(top, TV_NOT_READY, false)) { printf("check %s: FAIL typed with the link down\n", name); return 1; }
        setenv("PREVIEW_WIFI", "1", 1); step(d, 20);
        step(d, 800); pv_ctrl_space(d);
        if (s_ssh_sent_n - sent != 4 || memcmp(s_ssh_sent + sent, " pwd", 4)) { printf("check %s: FAIL the held text after the link came back\n", name); return 1; }
        ui_nav_back(); step(d, 20);
        s_preview_server_n--;
        printf("%s: the pty got exactly cc_voice_type_build_ex's bytes (always bracketed in a take-over, bare in a plain shell), no Enter or newline, no paste; joined with a space; a dialog, a draft, a confirm or a lost link holds the text for Ctrl+Space (60 s); limit and error toasts; another key's result left alone\n", name);
        return r;
    }
    if (!strcmp(v, "voice-touch") || !strcmp(v, "voice-touch-nokbd")) {
        /* the bar's Mic chip is Ctrl+Space by touch: a tap starts, a tap stops, the text is typed as before. nokbd: no
           physical keyboard, so ⌨ is in the bar too, and with a name at the 400 px cap the state slot is at its
           narrowest; "Recording 0:30 / 0:30" must still fit it whole */
        bool nokbd = !strcmp(v, "voice-touch-nokbd");
        if (nokbd) setenv("PREVIEW_KBD", "0", 1);
        lv_indev_t *in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, pointer_read);
        s_ssh_autoclose = false;
        ui_nav_home(); step(d, 200);
        const char *title = "companion-test-a-session-name-long-enough-to-reach-the-cap";   /* the narrowest bar */
        ui_terminal_open_command(0, "~/.local/bin/claude attach 1a2b3c4d", title, takeover_closed_cb, NULL);
        lv_obj_t *s = lv_screen_active(), *bar = session_bar_of(s), *top = lv_layer_top();
        assert(ui_terminal_is_top() && bar);
        /* review M2: before the pump sees the session up the bar says "Connecting…", whole beside Mic */
        if (!pv_state_fits(name, bar, "Connecting\xe2\x80\xa6")) return 1;
        /* the banner's hint says how to leave: by touch with no keyboard (review M5) */
        const char *hint = nokbd ? "Tap the top of the screen for the bar: \xe2\x86\x90 Leave returns to Sessions"
                                 : "Ctrl+\xe2\x86\x90 returns to Sessions";
        if (!pv_screen_has(hint)) { printf("check %s: FAIL the terminal banner doesn't read \"%s\"\n", name, hint); return 1; }
        step(d, 20);
        pv_ssh_feed(k_takeover_sample); pv_ssh_feed(TV_PROMPT); step(d, 20);
        /* a lost link: red "Link lost, reconnecting…", whole */
        setenv("PREVIEW_WIFI", "0", 1); step(d, 20);
        if (!pv_state_fits(name, bar, "Link lost, reconnecting\xe2\x80\xa6")) return 1;
        setenv("PREVIEW_WIFI", "1", 1); step(d, 20);
        if (lv_obj_get_style_max_width(lv_obj_get_child(bar, 1), 0) != 400) { printf("check %s: FAIL the name didn't get its 400 px back with the link\n", name); return 1; }
        step(d, 3100);
        if (obj_shown(bar)) { printf("check %s: FAIL the bar is still up\n", name); return 1; }
        tap(d, in, 640, 20);   /* touch: the top of the screen brings the bar back */
        lv_obj_t *mic = find_clickable_shown(bar, "Mic");
        if (!obj_shown(bar) || !mic || !obj_shown(mic)) { printf("check %s: FAIL no Mic chip in the bar\n", name); return 1; }
        if ((find_clickable_shown(bar, LV_SYMBOL_KEYBOARD) != NULL) != nokbd) { printf("check %s: FAIL the keyboard chip\n", name); return 1; }
        if (lv_obj_get_height(mic) != UI_TK_BAR_CTRL || !lv_obj_has_flag(mic, UI_BAR_CONTROL)) { printf("check %s: FAIL the Mic chip isn't a bar chip\n", name); return 1; }
        lv_obj_t *mic_l = lv_obj_get_child(mic, 0);
        lv_area_t ma0; lv_obj_get_coords(mic, &ma0);
        /* a tap starts a recording keyed to this screen and sends nothing */
        step(d, 800);
        int starts = s_voice_start_calls;
        size_t sent = s_ssh_sent_n;
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (s_voice_start_calls != starts + 1 || s_pv_voice.phase != VOICE_STARTING || strncmp(s_pv_voice.key, "term:", 5) || s_ssh_sent_n != sent) {
            printf("check %s: FAIL a Mic tap didn't start a terminal recording, or sent a byte\n", name); return 1;
        }
        snprintf(key, sizeof(key), "%s", s_pv_voice.key);
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 7000; s_pv_voice.meter = 60; step(d, 20);
        lv_obj_t *ind = find_label(bar, TV_REC_7, false);
        if (!ind) {
            lv_obj_t *st = find_label(bar, "Recording", true);
            printf("check %s: FAIL the indicator reads \"%s\" in a %d px slot\n", name, st ? lv_label_get_text(st) : "(none)", st ? (int)lv_obj_get_width(st) : -1);
            return 1;
        }
        if (!label_red(ind) || !bar_dot_red(bar) || !label_red(mic_l)) { printf("check %s: FAIL no red indicator and red Mic while recording\n", name); return 1; }
        /* the state slot holds the indicator whole, at 0:07 and at the 0:30 limit */
        for (int lim = 0; lim < 2; lim++) {
            if (lim) { s_pv_voice.elapsed_ms = 30000; step(d, 20); ind = find_label(bar, "Recording 0:30 / 0:30", false); }
            lv_point_t sz;
            assert(ind);
            lv_text_get_size(&sz, lv_label_get_text(ind), lv_obj_get_style_text_font(ind, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            printf("%s: state slot %d px for \"%s\" (%d px)\n", name, (int)lv_obj_get_width(ind), lv_label_get_text(ind), (int)sz.x);
            if (sz.x > lv_obj_get_width(ind)) { printf("check %s: FAIL \"%s\" (%d px) is cut in a %d px slot\n", name, lv_label_get_text(ind), (int)sz.x, (int)lv_obj_get_width(ind)); return 1; }
        }
        /* the indicator's updates (the 16 ms pump's seconds) never move the chip, and leave the bar's 16 px gap to it */
        lv_area_t ia, ma; lv_obj_get_coords(ind, &ia); lv_obj_get_coords(mic, &ma);
        if (ma.x1 != ma0.x1 || ma.x2 != ma0.x2 || ia.x2 + 16 > ma.x1) { printf("check %s: FAIL the Mic chip moved (%d-%d, was %d-%d) or the indicator (x2 %d) reaches it\n", name, (int)ma.x1, (int)ma.x2, (int)ma0.x1, (int)ma0.x2, (int)ia.x2); return 1; }
        s_pv_voice.elapsed_ms = 7000; step(d, 20);
        lv_obj_t *nr = lv_obj_get_child(bar, 1);   /* the name: after ← Leave */
        if (!nr || lv_obj_get_width(nr) > 160) { printf("check %s: FAIL the name didn't give way while recording\n", name); return 1; }
        step(d, 3100);
        if (!obj_shown(bar)) { printf("check %s: FAIL the bar hid while recording\n", name); return 1; }
        if (fast_timers(16) != 1 || fast_timers(100)) { printf("check %s: FAIL timers: 16 ms %d, 100 ms %d\n", name, fast_timers(16), fast_timers(100)); return 1; }
        if (pv_amber_pixels(s)) { printf("check %s: FAIL amber while recording\n", name); return 1; }
        r |= pv_run_checks(d, s, name, false, 0);
        /* a tap on Mic stops it (past the toggle guard): transcribing, Mic muted; a tap now says "Still transcribing…" */
        step(d, 800);
        int stops = s_voice_stop_calls;
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (s_voice_stop_calls != stops + 1 || s_pv_voice.phase != VOICE_TRANSCRIBING || !find_label(bar, "Transcribing", true) || label_red(mic_l)) {
            printf("check %s: FAIL a Mic tap didn't stop the recording\n", name); return 1;
        }
        lv_obj_get_coords(mic, &ma);
        if (ma.x1 != ma0.x1) { printf("check %s: FAIL the Mic chip moved while transcribing\n", name); return 1; }
        {
            lv_obj_t *tl = find_label(bar, TV_TRANSCRIBING, false);
            if (!tl) { printf("check %s: FAIL \"" TV_TRANSCRIBING "\" is cut in the state slot\n", name); return 1; }
            printf("%s: state slot %d px for \"%s\"\n", name, (int)lv_obj_get_width(tl), lv_label_get_text(tl));
        }
        r |= pv_run_checks(d, s, nokbd ? "terminal-voice-touch-nokbd-transcribing" : "terminal-voice-touch-transcribing", false, 0);
        step(d, 800);
        starts = s_voice_start_calls;
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
        if (s_voice_start_calls != starts || !find_label(top, "Still transcribing\xe2\x80\xa6", false)) { printf("check %s: FAIL a Mic tap while transcribing\n", name); return 1; }
        /* the result is typed as for Ctrl+Space: the builder's bytes, bracketed, no Enter */
        sent = s_ssh_sent_n;
        const char *text = "Please run the tests.";
        pv_voice_post(key, VR_OUT_TEXT, text, false); step(d, 20);
        char want[VR_TEXT_MAX + 14];
        size_t wn = cc_voice_type_build_ex(text, true, false, want, sizeof(want));
        if (!wn || s_ssh_sent_n - sent != wn || memcmp(s_ssh_sent + sent, want, wn) || sent_has_line_end(sent)) {
            printf("check %s: FAIL the pty got %d bytes, not the builder's %d\n", name, (int)(s_ssh_sent_n - sent), (int)wn); return 1;
        }
        if (find_label(bar, "Transcribing", true) || find_label(bar, "Recording", true)) { printf("check %s: FAIL the indicator outlived the result\n", name); return 1; }
        lv_obj_t *nl = lv_obj_get_child(bar, 1);
        if (!nl || lv_obj_get_style_max_width(nl, 0) != 400) { printf("check %s: FAIL the name didn't get its 400 px back\n", name); return 1; }
        if (nokbd) {
            /* touch wording: a result the session isn't ready for says to tap Mic, and the Mic tap types it */
            step(d, 800);
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
            s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 2000; step(d, 800);
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
            pv_ssh_feed(TV_DIALOG); step(d, 20);
            sent = s_ssh_sent_n;
            pv_voice_post(key, VR_OUT_TEXT, "later", false); step(d, 20);
            const char *touch = "Not typed: the session isn't at an empty prompt. Tap Mic there to type it.";
            if (s_ssh_sent_n != sent || !find_label(top, touch, false)) { printf("check %s: FAIL no touch wording for a held text\n", name); return 1; }
            pv_ssh_feed("\x1b[2J\x1b[H"); pv_ssh_feed(TV_PROMPT); step(d, 800);
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
            if (s_ssh_sent_n - sent != 17 || memcmp(s_ssh_sent + sent, "\x1b[200~later\x1b[201~", 17)) { printf("check %s: FAIL a Mic tap didn't type the held text\n", name); return 1; }
        }
        /* the Esc chip still cancels a recording started by touch */
        step(d, 800);
        lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
        s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 1000; step(d, 20);
        int cancels = s_voice_cancel_calls;
        sent = s_ssh_sent_n;
        click_label(d, bar, "Esc");
        if (s_voice_cancel_calls != cancels + 1 || s_pv_voice.phase != VOICE_IDLE || s_ssh_sent_n != sent || label_red(mic_l)) { printf("check %s: FAIL the Esc chip didn't cancel\n", name); return 1; }
        /* leaving by touch: the Leave chip */
        click_label(d, bar, "\xe2\x86\x90 Leave");
        if (ui_terminal_is_top()) { printf("check %s: FAIL Leave by touch\n", name); return 1; }
        pv_voice_reset();
        if (nokbd) {
            /* an SSH session by touch: its banner, and a result held behind the Disconnect confirm says to tap Mic */
            s = pv_open_ssh(d); bar = session_bar_of(s);
            if (!pv_screen_has("Tap the top of the screen for the bar: \xe2\x86\x90 Leave disconnects.")) { printf("check %s: FAIL the SSH banner isn't touch wording\n", name); return 1; }
            pv_ssh_feed("pi@box:~ $ "); step(d, 800);
            mic = find_clickable_shown(bar, "Mic");
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
            snprintf(key, sizeof(key), "%s", s_pv_voice.key);
            s_pv_voice.phase = VOICE_RECORDING; s_pv_voice.elapsed_ms = 2000; step(d, 800);
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);   /* stop: transcribing */
            click_label(d, bar, "\xe2\x86\x90 Leave");
            if (!find_label(top, "Disconnect from box?", false)) { printf("check %s: FAIL no confirm\n", name); return 1; }
            sent = s_ssh_sent_n;
            pv_voice_post(key, VR_OUT_TEXT, "pwd", false); step(d, 20);
            const char *nr_touch = "Not typed: the session isn't ready. Tap Mic when it is to type it.";
            if (s_ssh_sent_n != sent || !find_label(top, nr_touch, false)) { printf("check %s: FAIL no touch wording behind the confirm\n", name); return 1; }
            click_label(d, top, "Cancel");
            step(d, 800);
            lv_obj_send_event(mic, LV_EVENT_CLICKED, NULL); step(d, 20);
            if (s_ssh_sent_n - sent != 3 || memcmp(s_ssh_sent + sent, "pwd", 3)) { printf("check %s: FAIL a Mic tap didn't type the held text after Cancel (%d bytes)\n", name, (int)(s_ssh_sent_n - sent)); return 1; }
            click_label(d, bar, "\xe2\x86\x90 Leave");
            click_label(d, top, "Disconnect");
            assert(!ui_terminal_is_top());
            s_preview_server_n--;
            pv_voice_reset();
        }
        printf("%s: Mic (a bar chip%s) starts and stops like Ctrl+Space, red while recording, muted while transcribing; the text is typed as before; the indicator fits the state slot; Esc chip cancels; Leave leaves\n", name, nokbd ? ", beside the soft-keyboard chip, long name" : "");
        return r;
    }
    printf("preview: unknown case terminal %s\n", v);
    return 2;
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
        assert(lv_font_get_glyph_dsc(&font_px24,&g,0x2026,0) && !g.is_placeholder);   /* "Starting…" */
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
    if (!strcmp(name,"term-spacing")) { terminal_spacing_check(); return 0; }   /* C12: was "terminal" */
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
    if (!strcmp(name,"fwupdate")) return fwupdate_case(d, argc>2 ? argv[2] : "download");
    if (!strcmp(name,"fw-notes")) return fw_notes_case(d);
    if (!strcmp(name,"keyboard-settings")) return keyboard_settings_case(d);
    if (!strcmp(name,"network-live")) return network_live_case(d);
    if (!strcmp(name,"network-focus")) return network_focus_case(d);
    if (!strcmp(name,"picker-fresh")) return picker_fresh_case(d);
    if (!strcmp(name,"net-sub-open")) return net_sub_open_case(d);
    if (!strcmp(name,"terminal")) return terminal_case(d, argc>2 ? argv[2] : "");
    if (!strcmp(name,"takeover")) return takeover_case(d, argc>2 ? argv[2] : "");
    if (!strcmp(name,"ssh")) return ssh_case(d, argc>2 ? argv[2] : "");
    if (!strcmp(name,"hostform")) return hostform_case(d, argc>2 ? argv[2] : "new");
    if (!strcmp(name,"settings-sub")) return settings_sub_case(d, argc>2 ? argv[2] : "networks", argc>3 ? argv[3] : "");
    fprintf(stderr, "preview: unknown case %s\n", argc>1 ? argv[1] : "(none)");
    return 2;
}
