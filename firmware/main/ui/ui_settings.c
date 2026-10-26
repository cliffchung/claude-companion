/* Settings (spec §5.6–§5.12). One screen: the header, a 320 px rail of six sections and one scrolling
 * panel. Only the visible section is built (B2 RAM): a rail click cleans and refills the panel, never the
 * screen. The only timer of its own is the 2 s Diagnostics refresh, which exists only while About ›
 * Diagnostics is expanded and goes with the panel. */
#include "ui_settings.h"
#include "sys/link_down.h"
#include "ui.h"
#include "ui_comp.h"
#include "ui_softkbd.h"
#include "ui_sessions.h"
#include "ui_host_form.h"
#include "ui_settings_fw.h"
#include "ota/fw_update.h"
#include "settings/settings.h"
#include "settings/kbd_colour.h"
#include "sys/sysinfo.h"
#include "sys/power.h"
#include "sys/vitals.h"
#include "input/keyboard.h"
#include "servers/server_store.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "companion/cc_link.h"
#include "ssh/device_key.h"
#include "bsp/m5stack_tab5.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_OPTS(a) ((int)(sizeof(a) / sizeof(a[0])))

static ui_set_section_t s_section;   /* the last used section: Host after a boot (RAM only) */
static lv_obj_t *s_panel;            /* the one panel of the current Settings screen */
static lv_obj_t *s_rail[UI_SET_COUNT];
static bool s_diag_open;             /* About › Diagnostics expanded */

static const str_id_t k_section_names[UI_SET_COUNT] = {
    STR_SET_HOST, STR_SET_NETWORK, STR_SET_POWER, STR_SET_KBD_LIGHT, STR_SET_DISPLAY_TIME, STR_ABOUT,
};

static void panel_fill(void);
static void refill(void) { ui_settings_refill(); }   /* every panel keeps its scroll on a change (T8 M5) */

/* ------------------------------------------------------------------ Host */
static char s_host_names[SERVER_MAX][48];
static const char *s_host_ptrs[SERVER_MAX + 2];
static int s_host_n;

static void host_pick_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0) return;
    if (i < s_host_n) cc_link_set_server(s_host_names[i]);
    else if (i == s_host_n) {   /* Add host…: the host form; saving it makes the new entry the Host (P4-6) */
        static hf_arg_t a;
        memset(&a, 0, sizeof(a));
        a.index = -1;
        a.origin = HF_FROM_HOST_PICKER;
        ui_nav_push(ui_host_form_create, &a);
        return;
    }
    else cc_link_set_server("");
    refill();
}

static void host_cb(lv_event_t *e)
{
    (void)e;
    int n = server_store_count(), cur = -1;
    if (n > SERVER_MAX) n = SERVER_MAX;
    char want[48];
    cc_settings_server_name(want, sizeof(want));
    for (int i = 0; i < n; i++) {
        const server_t *s = server_store_get(i);
        strlcpy(s_host_names[i], s ? s->name : "?", sizeof(s_host_names[i]));
        s_host_ptrs[i] = s_host_names[i];
        if (want[0] && cur < 0 && !strcmp(want, s_host_names[i])) cur = i;
    }
    s_host_n = n;
    s_host_ptrs[n] = tr(STR_SET_ADD_HOST);
    s_host_ptrs[n + 1] = tr(STR_NONE);
    if (!want[0]) cur = n + 1;
    ui_picker(tr(STR_SET_HOST), s_host_ptrs, n + 2, cur, host_pick_cb, NULL);
}

/* Address: the host form on the Host's entry (§5.7); its Save tells the link */
static void address_cb(lv_event_t *e)
{
    (void)e;
    static hf_arg_t a;
    memset(&a, 0, sizeof(a));
    a.index = cc_settings_server();
    a.origin = HF_FROM_SETTINGS_HOST;
    if (a.index < 0) return;
    ui_nav_push(ui_host_form_create, &a);
}

static void fallback_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_lan_fallback(v)) { ui_toast(tr(STR_SET_FALLBACK_BAD)); return; }
    refill();
}
static void fallback_cb(lv_event_t *e)
{
    (void)e;
    char cur[16];
    ui_prompt(tr(STR_SET_FALLBACK), tr(STR_SET_FALLBACK_PH), cc_link_lan_fallback(cur, sizeof(cur)), false, fallback_prompt_cb, NULL);
}

static void path_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_claude_path(v)) { ui_toast(tr(STR_SET_CLAUDE_PATH_BAD)); return; }
    refill();
}
static void path_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    ui_prompt(tr(STR_SET_CLAUDE_PATH), tr(STR_SET_CLAUDE_PATH_PH), cc_settings_claude_path(cur, sizeof(cur)), false,
              path_prompt_cb, NULL);
}

/* Whisper path and model on the Host (Plan 5, spec 6.5): the same rule and copy as the Claude path */
static void whisper_path_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_whisper_path(v)) { ui_toast(tr(STR_SET_CLAUDE_PATH_BAD)); return; }
    refill();
}
static void whisper_path_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    ui_prompt(tr(STR_SET_WHISPER_PATH), tr(STR_SET_WHISPER_PATH_PH), cc_settings_whisper_path(cur, sizeof(cur)), false,
              whisper_path_prompt_cb, NULL);
}
static void whisper_model_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_whisper_model(v)) { ui_toast(tr(STR_SET_CLAUDE_PATH_BAD)); return; }
    refill();
}
static void whisper_model_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    ui_prompt(tr(STR_SET_WHISPER_MODEL), tr(STR_SET_WHISPER_MODEL_PH), cc_settings_whisper_model(cur, sizeof(cur)), false,
              whisper_model_prompt_cb, NULL);
}
/* Voice input · Check (P5-5): the check runs on the Sessions worker, which toasts its result */
static void voice_check_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_sessions_voice_check()) ui_toast(tr(STR_SET_BUSY));
}

static void forget_yes(bool yes, void *ud)
{
    (void)ud;
    if (yes && !ui_sessions_forget_host_key()) ui_toast(tr(STR_SET_BUSY));
}
static void forget_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm_ex(tr(STR_SET_FORGET_Q), tr(STR_SET_FORGET_BODY), tr(STR_SET_FORGET), true, forget_yes, NULL);
}

/* Trust this key (polish 1): the key the Host presented on a first connect, which nothing pins or carries over. The
   confirm shows it again with the command that prints the Host's own; only that key is pinned (cc_link_trust_host_key).
   s_trust_fp is the panel's (every refill rewrites it); the confirm trusts its own copy, s_confirm_fp, which only
   trust_cb writes, so a refill while it is open can't change what gets trusted (fix round 1, C1). */
static char s_trust_fp[80];
static char s_confirm_fp[80];
static char s_trust_body[320];
static void trust_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    /* the key the Host presents now must still be the one the confirm showed: else nothing is trusted */
    char now[80];
    if (!cc_link_unknown_host_key(now, sizeof(now)) || strcmp(now, s_confirm_fp) != 0) {
        ui_toast(tr(STR_SET_TRUST_STALE));
        refill();
        return;
    }
    if (!ui_sessions_trust_host_key(s_confirm_fp)) ui_toast(tr(STR_SET_BUSY));
}
static void trust_cb(lv_event_t *e)
{
    (void)e;
    strlcpy(s_confirm_fp, s_trust_fp, sizeof(s_confirm_fp));
    snprintf(s_trust_body, sizeof(s_trust_body), tr(STR_SET_TRUST_BODY), s_confirm_fp);
    ui_confirm_ex(tr(STR_SET_TRUST_Q), s_trust_body, tr(STR_SET_TRUST_ACTION), false, trust_yes, NULL);
}

static void pubkey_cb(lv_event_t *e)
{
    (void)e;
    char line[160];
    if (device_key_pubkey_line(line, sizeof(line)) != ESP_OK) { ui_toast(tr(STR_SET_KEY_UNREADABLE)); return; }
    ui_sheet_text(tr(STR_SET_DEVICE_KEY), line, tr(STR_SET_DEVICE_KEY_NOTE));
}

/* "ssh-ed25519 … claude-companion · Show": the key type, then the comment up to its '@'. */
static void pubkey_short(char *out, size_t cap)
{
    char line[160];
    if (device_key_pubkey_line(line, sizeof(line)) != ESP_OK) { strlcpy(out, tr(STR_SET_KEY_UNREADABLE), cap); return; }
    char *type_end = strchr(line, ' ');
    char *comment = type_end ? strchr(type_end + 1, ' ') : NULL;
    if (type_end) *type_end = 0;
    if (comment) {
        comment++;
        char *at = strchr(comment, '@');
        if (at) *at = 0;
    }
    snprintf(out, cap, "%s \xe2\x80\xa6 %s \xc2\xb7 %s", line, comment ? comment : "", tr(STR_SET_SHOW));
}

/* Link: "● connected over home Wi-Fi" in green or the reason in red, then the path and the watch state. */
static void link_row(lv_obj_t *p)
{
    lv_obj_t *r = ui_srow(p, UI_ROW_INFO, tr(STR_SET_LINK), NULL, NULL, NULL);
    lv_obj_delete(ui_srow_value(r));
    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, LV_PCT(65), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    char path[40], b[CC_STATUS_MAX + 8], w[96];
    bool up = cc_link_connected();
    bool tailnet = cc_link_mac_path(path, sizeof(path)) && !strncmp(path, "tailnet", 7);
    if (up) snprintf(b, sizeof(b), "\xe2\x97\x8f %s", tr(tailnet ? STR_SET_LINK_UP_TS : STR_SET_LINK_UP_LAN));
    else ui_sessions_link_down_text(b, sizeof(b));
    lv_obj_t *l1 = ui_text(col, b, up ? UI_TONE_GREEN : UI_TONE_RED, true);
    lv_obj_t *l2 = ui_text(col, "", UI_TONE_MUTED, true);
    snprintf(b, sizeof(b), "%s \xc2\xb7 %s", path, cc_watch_status_text(w, sizeof(w)));
    lv_label_set_text(l2, b);
    lv_obj_t *ls[2] = { l1, l2 };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(ls[i], LV_PCT(100));
        lv_obj_set_style_text_align(ls[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(ls[i], LV_LABEL_LONG_MODE_WRAP);
    }
}

static void fill_host(lv_obj_t *p)
{
    char name[48], v[160];
    cc_settings_server_name(name, sizeof(name));
    int idx = cc_settings_server();
    const server_t *s = idx >= 0 ? server_store_get(idx) : NULL;
    ui_srow(p, UI_ROW_NAV, tr(STR_SET_HOST), s ? s->host : name[0] ? name : tr(STR_SET_NOT_SET), host_cb, NULL);
    if (s) {
        if (s->port && s->port != 22) snprintf(v, sizeof(v), "%s@%s:%d", s->user, s->host, s->port);
        else snprintf(v, sizeof(v), "%s@%s", s->user, s->host);
        ui_srow(p, UI_ROW_NAV, tr(STR_SET_ADDRESS), v, address_cb, NULL);
    }
    link_row(p);
    char fb[16];
    cc_link_lan_fallback(fb, sizeof(fb));
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_FALLBACK), fb[0] ? fb : tr(STR_NONE), fallback_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_CLAUDE_PATH), cc_settings_claude_path(v, sizeof(v)), path_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_WHISPER_PATH), cc_settings_whisper_path(v, sizeof(v)), whisper_path_cb, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_WHISPER_MODEL), cc_settings_whisper_model(v, sizeof(v)), whisper_model_cb, NULL);
    lv_obj_t *vr = ui_srow(p, UI_ROW_ACTION, tr(STR_SET_VOICE), NULL, voice_check_cb, NULL);
    ui_srow_chip(vr, tr(STR_SET_VOICE_CHECK), false);
    ui_settings_fw_host_row(p);   /* Plan 6: Firmware file on Host */

    ui_section_header(p, tr(STR_SET_HOST_KEY));
    char fp[80];
    if (cc_link_pinned_fingerprint(fp, sizeof(fp))) {
        /* the whole fingerprint, wrapped, to compare with ssh-keygen -lf on the host */
        ui_srow_fingerprint(p, fp, tr(STR_SET_FORGET), true, forget_cb, NULL);
    } else if (cc_link_unknown_host_key(s_trust_fp, sizeof(s_trust_fp))) {
        /* first run: the link refused a key nothing pins; never trusted without the user's confirm */
        ui_srow_fingerprint(p, s_trust_fp, tr(STR_SET_TRUST), false, trust_cb, NULL);
        ui_footnote(p, tr(STR_SET_NEW_KEY_SEEN));
    } else {
        ui_srow(p, UI_ROW_INFO, tr(STR_SET_NO_PIN), NULL, NULL, NULL);
    }

    ui_section_header(p, tr(STR_SET_DEVICE_KEY));
    pubkey_short(v, sizeof(v));
    ui_srow(p, UI_ROW_ACTION, tr(STR_SET_PUBLIC_KEY), v, pubkey_cb, NULL);
    ui_footnote(p, tr(STR_SET_HOST_NOTE));
}

/* Network: ui_settings_net.c (Task 8). */

/* ----------------------------------------------------------------- Power */
/* Menus of presets; the values go through power_setting_set, so they get the console's clamps. */
typedef struct { str_id_t title; const char *name; char unit; const int *opts; int n; str_id_t zero; } pw_menu_t;
static const int k_dim_opts[] = { 10, 15, 30, 60, 120, 300, 600 };
static const int k_off_opts[] = { 20, 30, 60, 120, 300, 600, 1800, 3600 };
static const int k_deep_opts[] = { 0, 15, 30, 60, 120, 240, 480, 720 };
static const int k_hib_opts[] = { 0, 1, 2, 4, 8, 12, 24, 48 };
static const int k_radio_after_opts[] = { 5, 10, 20, 30, 60, 120, 240 };
static const int k_checkin_opts[] = { 1, 2, 5, 10, 15, 30, 60 };
static const int k_hib_checkin_opts[] = { 0, 15, 30, 60, 120, 240, 480, 720 };
static const pw_menu_t k_pw_dim = { STR_SET_PW_DIM, "dim", 's', k_dim_opts, N_OPTS(k_dim_opts), STR_SET_NEVER };
static const pw_menu_t k_pw_off = { STR_SET_PW_OFF, "off", 's', k_off_opts, N_OPTS(k_off_opts), STR_SET_NEVER };
static const pw_menu_t k_pw_deep = { STR_SET_PW_DEEP, "deep-idle", 'm', k_deep_opts, N_OPTS(k_deep_opts), STR_SET_NEVER };
static const pw_menu_t k_pw_hib = { STR_SET_PW_HIB, "hibernate", 'h', k_hib_opts, N_OPTS(k_hib_opts), STR_SET_NEVER };
static const pw_menu_t k_pw_radio_after = { STR_SET_PW_RADIO_AFTER, "radio-after", 'm', k_radio_after_opts,
                                            N_OPTS(k_radio_after_opts), STR_SET_NEVER };
static const pw_menu_t k_pw_checkin = { STR_SET_PW_CHECKIN, "checkin", 'm', k_checkin_opts, N_OPTS(k_checkin_opts),
                                        STR_SET_NEVER };
static const pw_menu_t k_pw_hib_checkin = { STR_SET_PW_HIB_CHECKIN, "hib-checkin", 'm', k_hib_checkin_opts,
                                            N_OPTS(k_hib_checkin_opts), STR_OFF };

/* "30 s", "2 min", "1 h", or the menu's word for 0 ("Never", "Off") */
static void pw_value(const pw_menu_t *m, int v, char *buf, size_t cap)
{
    int s = m->unit == 's' ? v : m->unit == 'm' ? v * 60 : v * 3600;
    if (v == 0) snprintf(buf, cap, "%s", tr(m->zero));
    else if (s < 60 || s % 60) snprintf(buf, cap, "%d %s", s, tr(STR_UNIT_S));
    else if (s % 3600 == 0) snprintf(buf, cap, "%d %s", s / 3600, tr(STR_UNIT_H));
    else snprintf(buf, cap, "%d %s", s / 60, tr(STR_UNIT_MIN));
}
static int pw_current(const pw_menu_t *m)
{
    return m == &k_pw_dim ? g_settings.pw_dim_s : m == &k_pw_off ? g_settings.pw_off_s
         : m == &k_pw_deep ? g_settings.pw_deep_idle_min : m == &k_pw_hib ? g_settings.pw_hibernate_h
         : m == &k_pw_radio_after ? power_cfg(PCFG_RADIO_AFTER) : m == &k_pw_checkin ? power_cfg(PCFG_CHECKIN)
         : power_cfg(PCFG_HIB_CHECKIN);
}
static void pw_menu_cb(int i, void *ud)
{
    const pw_menu_t *m = ud;
    if (i < 0 || i >= m->n) return;
    power_setting_set(m->name, m->opts[i]);
    refill();
}
static void pw_row_cb(lv_event_t *e)
{
    const pw_menu_t *m = lv_event_get_user_data(e);
    char items[16][24];   /* the picker's rows copy the labels */
    const char *ptrs[16];
    int n = m->n < 16 ? m->n : 16, cur = -1, v = pw_current(m);
    for (int i = 0; i < n; i++) {
        pw_value(m, m->opts[i], items[i], sizeof(items[i]));
        ptrs[i] = items[i];
        if (m->opts[i] == v) cur = i;
    }
    ui_picker(tr(m->title), ptrs, n, cur, pw_menu_cb, (void *)m);
}
static void pw_row(lv_obj_t *p, const pw_menu_t *m, const char *override)
{
    char buf[24];
    pw_value(m, pw_current(m), buf, sizeof(buf));
    ui_srow(p, UI_ROW_VALUE, tr(m->title), override ? override : buf, pw_row_cb, (void *)m);
}
static void pw_switch_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    power_setting_set(name, lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
static void ssh_awake_cb(lv_event_t *e)
{
    g_settings.ss_disable_in_ssh = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_save();
}
#define ON(x) ((x) ? "1" : NULL)   /* a switch row starts on with a non-empty value */

static void fill_power(lv_obj_t *p)
{
    ui_section_header(p, tr(STR_SET_PW_SCREEN));
    pw_row(p, &k_pw_dim, NULL);
    pw_row(p, &k_pw_off, NULL);
    ui_srow(p, UI_ROW_SWITCH, tr(STR_SET_PW_SLEEP_USB), ON(g_settings.pw_sleep_on_usb), pw_switch_cb, (void *)"sleep-usb");
    ui_srow(p, UI_ROW_SWITCH, tr(STR_SET_PW_SSH_AWAKE), ON(g_settings.ss_disable_in_ssh), ssh_awake_cb, NULL);
    ui_footnote(p, tr(STR_SET_PW_SCREEN_NOTE));
    ui_section_header(p, tr(STR_SET_PW_WAKE));
    ui_srow(p, UI_ROW_SWITCH, tr(STR_SET_PW_WAKE_NEEDS), ON(g_settings.pw_wake_needs_you), pw_switch_cb, (void *)"needs-wake");
    ui_footnote(p, tr(STR_SET_PW_WAKE_NOTE));
    ui_section_header(p, tr(STR_SET_PW_SAVER));
    ui_srow(p, UI_ROW_SWITCH, tr(STR_SET_PW_RADIO_OFF), ON(power_cfg(PCFG_RADIO_OFF)), pw_switch_cb, (void *)"radio-off");
    pw_row(p, &k_pw_radio_after, NULL);
    pw_row(p, &k_pw_checkin, NULL);
    pw_row(p, &k_pw_deep, NULL);
    pw_row(p, &k_pw_hib, hibernate_supported() ? NULL : tr(STR_SET_NOT_AVAILABLE));
    pw_row(p, &k_pw_hib_checkin, NULL);
    ui_footnote(p, tr(STR_SET_PW_SAVER_NOTE));
}

/* -------------------------------------------------------- Keyboard light */
static const char *colour_label(void)
{
    int i = kbd_colour_index(g_settings.kbd_led_mode, g_settings.kbd_led_rgb);
    return i == 0 ? tr(STR_SET_KBD_OWN) : i > 0 ? kbd_swatches[i - 1].label : tr(STR_SET_CUSTOM);
}
static void colour_pick_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0) return;
    kbd_colour_apply(i, &g_settings.kbd_led_mode, g_settings.kbd_led_rgb);
    settings_save();
    power_kbd_light_refresh();
    refill();
}
static void colour_cb(lv_event_t *e)
{
    (void)e;
    const char *items[1 + KBD_SWATCH_COUNT];
    items[0] = tr(STR_SET_KBD_OWN);
    for (int i = 0; i < KBD_SWATCH_COUNT; i++) items[1 + i] = kbd_swatches[i].label;
    lv_obj_t *sheet = ui_picker(tr(STR_SET_COLOUR), items, 1 + KBD_SWATCH_COUNT,
                                kbd_colour_index(g_settings.kbd_led_mode, g_settings.kbd_led_rgb), colour_pick_cb, NULL);
    /* the swatches are drawn, never shown as hex: the rows follow the title in item order */
    for (int i = 0; i < KBD_SWATCH_COUNT; i++) {
        lv_obj_t *row = lv_obj_get_child(sheet, 2 + i);
        if (row) lv_obj_move_to_index(ui_swatch(row, kbd_swatches[i].rgb), 1);
    }
}
static void led_bright_live_cb(lv_event_t *e)
{
    g_settings.kbd_led_bright = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    power_kbd_light_refresh();
}
static void led_bright_cb(lv_event_t *e)
{
    led_bright_live_cb(e);
    settings_save();
}
static void needs_light_cb(lv_event_t *e)
{
    g_settings.kbd_needs_light = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_save();
    power_kbd_light_refresh();
}
/* PT24: the needs-you amber's brightness with the screen dark */
static const struct { str_id_t name; int pct; } k_dark_light[] = {
    { STR_SET_DARK_LOWEST, PR_DARK_LIGHT_LOWEST }, { STR_SET_DARK_LOW, PR_DARK_LIGHT_LOW },
    { STR_SET_DARK_MEDIUM, PR_DARK_LIGHT_MEDIUM },
};
static int dark_light_index(void)
{
    for (int i = 0; i < N_OPTS(k_dark_light); i++)
        if (g_settings.kbd_dark_light == k_dark_light[i].pct) return i;
    return -1;
}
static void dark_light_pick_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i >= N_OPTS(k_dark_light)) return;
    power_setting_set("dark-light", k_dark_light[i].pct);
    power_kbd_light_refresh();
    refill();
}
static void dark_light_cb(lv_event_t *e)
{
    (void)e;
    const char *items[N_OPTS(k_dark_light)];
    for (int i = 0; i < N_OPTS(k_dark_light); i++) items[i] = tr(k_dark_light[i].name);
    ui_picker(tr(STR_SET_DARK_LIGHT), items, N_OPTS(k_dark_light), dark_light_index(), dark_light_pick_cb, NULL);
}

static str_id_t kbd_state(void)
{
    return keyboard_present() ? STR_SET_KBD_CONNECTED : keyboard_usb_present() ? STR_SET_KBD_USB : STR_SET_KBD_NONE;
}

static void fill_kbd(lv_obj_t *p)
{
    ui_srow(p, UI_ROW_INFO, tr(STR_KEYBOARD), tr(kbd_state()), NULL, NULL);
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_COLOUR), colour_label(), colour_cb, NULL);
    lv_obj_t *r = ui_srow_slider(p, tr(STR_SET_BRIGHTNESS), 0, 100, g_settings.kbd_led_bright, led_bright_cb, NULL);
    lv_obj_add_event_cb(ui_srow_value(r), led_bright_live_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui_section_header(p, tr(STR_SET_NEEDS_YOU));
    ui_srow(p, UI_ROW_SWITCH, tr(STR_SET_KBD_NEEDS_LIGHT), ON(g_settings.kbd_needs_light), needs_light_cb, NULL);
    char pct[16];
    int di = dark_light_index();
    if (di < 0) snprintf(pct, sizeof(pct), "%u %%", g_settings.kbd_dark_light);   /* set from the console */
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_DARK_LIGHT), di >= 0 ? tr(k_dark_light[di].name) : pct, dark_light_cb, NULL);
    ui_footnote(p, tr(STR_SET_KBD_NOTE));
}

/* --------------------------------------------------------- Display & time */
static void bright_live_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (v < 5) v = 5;
    g_settings.brightness = (uint8_t)v;
    bsp_display_brightness_set(v);
}
static void bright_cb(lv_event_t *e)
{
    bright_live_cb(e);
    settings_save();   /* saved on release */
}
static void tz_pick_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0 || !vitals_tz_presets[i].rule) return;
    strlcpy(g_settings.tz_rule, vitals_tz_presets[i].rule, sizeof(g_settings.tz_rule));
    settings_save();
    sysinfo_apply_timezone();
    refill();
}
static int tz_index(void)
{
    for (int i = 0; vitals_tz_presets[i].label; i++)
        if (!strcmp(vitals_tz_presets[i].rule, g_settings.tz_rule)) return i;
    return -1;
}
static void tz_cb(lv_event_t *e)
{
    (void)e;
    const char *ptrs[16];
    int n = 0;
    while (n < 16 && vitals_tz_presets[n].label) { ptrs[n] = vitals_tz_presets[n].label; n++; }
    ui_picker(tr(STR_SET_TZ), ptrs, n, tz_index(), tz_pick_cb, NULL);
}
static void sntp_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_mgr_is_connected()) { ui_toast(tr(STR_SET_NO_WIFI)); return; }
    sysinfo_start_sntp();
    ui_toast(tr(STR_SET_SYNCING));
}

static void fill_display(lv_obj_t *p)
{
    lv_obj_t *r = ui_srow_slider(p, tr(STR_SET_BRIGHTNESS), 5, 100, g_settings.brightness, bright_cb, NULL);
    lv_obj_add_event_cb(ui_srow_value(r), bright_live_cb, LV_EVENT_VALUE_CHANGED, NULL);
    int tz = tz_index();
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_TZ), tz >= 0 ? vitals_tz_presets[tz].label : tr(STR_SET_TZ_CUSTOM), tz_cb, NULL);
    char buf[24];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    r = ui_srow(p, UI_ROW_ACTION, tr(STR_SET_CLOCK), buf, sntp_cb, NULL);
    ui_srow_chip(r, tr(STR_SET_SYNC_NOW), false);
    ui_footnote(p, tr(STR_SET_CLOCK_NOTE));
}

/* ----------------------------------------------------------------- About */
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return tr(STR_RST_POWERON);
    case ESP_RST_EXT: return tr(STR_RST_EXT);
    case ESP_RST_SW: return tr(STR_RST_SW);
    case ESP_RST_PANIC: return tr(STR_RST_PANIC);
    case ESP_RST_INT_WDT: return tr(STR_RST_INT_WDT);
    case ESP_RST_TASK_WDT: return tr(STR_RST_TASK_WDT);
    case ESP_RST_WDT: return tr(STR_RST_WDT);
    case ESP_RST_DEEPSLEEP: return tr(STR_RST_DEEPSLEEP);
    case ESP_RST_BROWNOUT: return tr(STR_RST_BROWNOUT);
    case ESP_RST_USB: return tr(STR_RST_USB);
    default: return tr(STR_RST_OTHER);
    }
}

static int s_diag_channel;   /* read once when Diagnostics opens (an SDIO RPC), not every 2 s */

/* Appends one formatted line (the formats are STR_DIAG_*); false once the buffer is full. */
static bool diag_line(char *b, size_t cap, size_t *n, const char *fmt, ...)
{
    if (*n >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k < 0) return false;
    *n += (size_t)k;
    return *n < cap;
}

static void diag_text(char *b, size_t cap)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    power_info_t pw = sysinfo_power();
    char ip[20] = "", up[16];
    if (wifi_mgr_is_connected()) wifi_mgr_get_ip4(ip, sizeof(ip));
    ui_format_duration(up, sizeof(up), sysinfo_uptime_s());
    size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), n = 0;
    b[0] = 0;
    if (!diag_line(b, cap, &n, tr(STR_DIAG_CHIP), chip.revision / 100, chip.revision % 100, esp_get_idf_version())) return;
    if (!diag_line(b, cap, &n, tr(STR_DIAG_INTERNAL), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024))) return;
    if (!diag_line(b, cap, &n, tr(STR_DIAG_PSRAM), (unsigned)(ps >> 20), (unsigned)((ps & 0xFFFFF) * 10 >> 20))) return;
    bool ok = pw.valid
        ? diag_line(b, cap, &n, tr(STR_DIAG_BATTERY), (double)pw.bus_v, (int)(pw.current_a * 1000.0f),
                    (double)(pw.bus_v * (pw.current_a < 0 ? -pw.current_a : pw.current_a)),
                    tr(sysinfo_charging() ? STR_DIAG_CHARGING : STR_DIAG_NOT_CHARGING))
        : diag_line(b, cap, &n, "%s", tr(STR_DIAG_BATTERY_NONE));
    if (!ok) return;
    ok = ip[0] ? diag_line(b, cap, &n, tr(STR_DIAG_WIFI), sysinfo_rssi_cached(), s_diag_channel, ip)
               : diag_line(b, cap, &n, "%s", tr(STR_DIAG_WIFI_NONE));
    if (!ok) return;
    diag_line(b, cap, &n, tr(STR_DIAG_REST), tailscale_state_text(), tr(kbd_state()), up, reset_name(esp_reset_reason()));
}

static void diag_tick(lv_timer_t *t)
{
    lv_obj_t *l = lv_timer_get_user_data(t);
    char b[512];
    diag_text(b, sizeof(b));
    if (strcmp(lv_label_get_text(l), b)) lv_label_set_text(l, b);   /* a write redraws: only on a change */
}

static void diag_cb(lv_event_t *e)
{
    (void)e;
    s_diag_open = !s_diag_open;
    if (s_diag_open) s_diag_channel = wifi_mgr_is_connected() ? wifi_mgr_channel() : 0;
    refill();
}

static void reboot_yes(bool yes, void *ud) { (void)ud; if (yes) link_down_restart("Settings reboot"); }
static void reboot_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm_ex(tr(STR_REBOOT_Q), NULL, tr(STR_REBOOT), false, reboot_yes, NULL);
}
static void reset_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    settings_reset_user();
    ui_toast(tr(STR_SET_RESET_DONE));
    refill();
}
static void reset_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm_ex(tr(STR_SET_RESET_Q), tr(STR_SET_RESET_BODY), tr(STR_SET_RESET), true, reset_yes, NULL);
}

static void fill_about(lv_obj_t *p)
{
    char v[96];
    snprintf(v, sizeof(v), "%s \xc2\xb7 %s", sysinfo_app_version(), esp_app_get_description()->date);
    if (!fwu_rollback_ok()) snprintf(v + strlen(v), sizeof(v) - strlen(v), " \xc2\xb7 %s", tr(STR_SET_FW_ROLLBACK_OFF));   /* fix C1 */
    ui_srow(p, UI_ROW_INFO, tr(STR_SET_VERSION), v, NULL, NULL);
    ui_settings_fw_about(p);   /* Plan 6: Update firmware, Previous version */
    ui_section_header(p, tr(STR_SET_CREDITS));
    ui_footnote(p, tr(STR_SET_CREDIT_CLIENTRE));
    ui_footnote(p, tr(STR_SET_CREDIT_LIBSSH));
    ui_section_header(p, tr(STR_SET_SYSTEM));
    ui_srow(p, UI_ROW_NAV, tr(STR_SET_DIAGNOSTICS), tr(s_diag_open ? STR_SET_HIDE : STR_SET_SHOW), diag_cb, NULL);
    if (s_diag_open) {
        char b[512];
        diag_text(b, sizeof(b));
        lv_obj_t *l = ui_text(p, b, UI_TONE_FG, true);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_hor(l, 20, 0);
        /* refreshed every 2 s only while expanded: the timer goes with the label (a collapse or a section change
           cleans the panel), and LVGL is paused while the screen is off */
        ui_timer_bind(lv_timer_create(diag_tick, 2000, l), l);
    }
    ui_srow(p, UI_ROW_ACTION, tr(STR_REBOOT), NULL, reboot_cb, NULL);
    lv_obj_t *r = ui_srow(p, UI_ROW_ACTION, tr(STR_SET_RESET_SETTINGS), NULL, reset_cb, NULL);
    ui_text_tone(lv_obj_get_child(r, 0), UI_TONE_RED);
    ui_footnote(p, tr(STR_SET_RESET_NOTE));
}

/* ----------------------------------------------------------------- shell */
static void panel_fill(void)
{
    lv_obj_clean(s_panel);   /* the Diagnostics timer, if any, goes with its label */
    lv_obj_scroll_to_y(s_panel, 0, LV_ANIM_OFF);
    ui_title(s_panel, tr(k_section_names[s_section]));
    switch (s_section) {
    case UI_SET_HOST: fill_host(s_panel); break;
    case UI_SET_NETWORK: ui_settings_fill_network(s_panel); break;
    case UI_SET_POWER: fill_power(s_panel); break;
    case UI_SET_KBD: fill_kbd(s_panel); break;
    case UI_SET_DISPLAY: fill_display(s_panel); break;
    case UI_SET_ABOUT: fill_about(s_panel); break;
    default: break;
    }
}

void ui_settings_key_pinned(void)
{
    if (!s_panel) return;
    lv_obj_t *f = ui_keyboard_focused();
    while (f && f != s_panel) f = lv_obj_get_parent(f);
    ui_settings_refill();
    /* M7: the pinned row has the same fingerprint label as the Trust row, with Forget on it: the focus restored
       there would sit on a destructive control, so it goes to the panel's first control instead */
    if (f && s_section == UI_SET_HOST) ui_keyboard_focus_in(s_panel, 0);
}

void ui_settings_refill(void)
{
    if (!s_panel) return;
    int32_t y = lv_obj_get_scroll_y(s_panel);
    ui_focus_key_t k;
    ui_focus_save(s_panel, &k);   /* T13-3: the keyboard focus stays on its row (the Tailscale switch's refill) */
    panel_fill();
    lv_obj_update_layout(s_panel);
    lv_obj_scroll_to_y(s_panel, y, LV_ANIM_OFF);
    ui_focus_restore(s_panel, &k);
}

static void select_section(ui_set_section_t next)
{
    if (next >= UI_SET_COUNT || next == s_section || !s_panel) return;
    ui_srow_set_selected(s_rail[s_section], false);
    ui_srow_set_selected(s_rail[next], true);
    s_section = next;
    s_diag_open = false;
    panel_fill();
}

static void rail_cb(lv_event_t *e) { select_section((ui_set_section_t)(uintptr_t)lv_event_get_user_data(e)); }

static void screen_deleted(lv_event_t *e)
{
    (void)e;
    s_panel = NULL;
    memset(s_rail, 0, sizeof(s_rail));
}

lv_obj_t *ui_settings_create(void *arg)
{
    (void)arg;
    s_diag_open = false;   /* Diagnostics starts collapsed */
    lv_obj_t *scr = ui_screen_base();
    lv_obj_add_event_cb(scr, screen_deleted, LV_EVENT_DELETE, NULL);
    ui_header(scr, tr(STR_SETTINGS), tr(STR_SET_BACK_SESSIONS));
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(body, 0, UI_TK_HEADER_H);
    lv_obj_set_size(body, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - UI_TK_HEADER_H);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    /* the rail: 320 px, six 72 px rows; the selection is panel-2 with a 50 % fg outline, no hue */
    lv_obj_t *rail = lv_obj_create(body);
    lv_obj_remove_style_all(rail);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rail, UI_TK_RAIL_W, LV_PCT(100));
    lv_obj_set_style_pad_top(rail, UI_TK_GAP_M, 0);
    lv_obj_set_style_pad_bottom(rail, UI_TK_GAP_M, 0);
    lv_obj_set_style_pad_left(rail, UI_TK_MARGIN, 0);
    lv_obj_set_style_pad_right(rail, UI_TK_GAP_S, 0);
    lv_obj_set_style_pad_row(rail, UI_TK_GAP_S, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, lv_color_hex(UI_TK_LINE), 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < UI_SET_COUNT; i++) {
        s_rail[i] = ui_rail_row(rail, tr(k_section_names[i]), rail_cb, (void *)(uintptr_t)i);
        ui_srow_set_selected(s_rail[i], i == (int)s_section);
    }
    lv_obj_t *spacer = lv_obj_create(rail);
    lv_obj_remove_style_all(spacer);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    /* "↑↓ move · Enter choose · Esc back" while the keyboard is in use (spec §5.13) */
    lv_obj_t *hint = ui_text(rail, tr(STR_SET_KEY_HINT), UI_TONE_MUTED, true);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    ui_keyboard_hint(hint);

    /* the panel: a title heading and 64 px rows; it scrolls when the section doesn't fit */
    s_panel = lv_obj_create(body);
    lv_obj_remove_style_all(s_panel);
    ui_settings_column(s_panel);
    lv_obj_set_height(s_panel, LV_PCT(100));
    lv_obj_set_flex_grow(s_panel, 1);
    panel_fill();
    return scr;
}

void ui_settings_open(ui_set_section_t s)
{
    if (s < UI_SET_COUNT) s_section = s;
    ui_nav_push(ui_settings_create, NULL);
}

void ui_settings_open_net_sub(ui_net_sub_t sub)
{
    /* Settings on top: switch its section in place, so one Back returns to it (T8 M7) */
    if (ui_nav_is_top(ui_settings_create) && s_panel) select_section(UI_SET_NETWORK);
    else ui_settings_open(UI_SET_NETWORK);
    ui_nav_push(ui_settings_sub_create, (void *)(intptr_t)sub);
}
