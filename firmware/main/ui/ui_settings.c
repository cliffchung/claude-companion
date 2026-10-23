#include "ui.h"
#include "ui_motion.h"
#include "ui_softkbd.h"
#include "settings/settings.h"
#include "sys/sysinfo.h"
#include "input/keyboard.h"
#include "sys/power.h"
#include "sys/vitals.h"
#include "servers/server_store.h"
#include "net/wifi_mgr.h"
#include "storage/sd.h"
#include "companion/cc_link.h"
#include "bsp/m5stack_tab5.h"
#include "esp_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (v < 5) v = 5;
    g_settings.brightness = v;
    bsp_display_brightness_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void rot_menu_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0) return;
    g_settings.rotation = i; settings_save();
    lv_display_set_rotation(NULL, (lv_display_rotation_t)i);
    ui_nav_refresh();
}
static void rot_cb(lv_event_t *e)
{
    (void)e;
    const char *items[] = { "0°", "90°", "180°", "270°" };
    ui_menu(tr(STR_SET_ROTATION), items, 4, rot_menu_cb, NULL);
}

static void ntp_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_mgr_is_connected()) { ui_toast(tr(STR_WIFI_NOT_CONNECTED)); return; }
    sysinfo_start_sntp();
    ui_toast(tr(STR_PLEASE_WAIT));
}

static void kbd_layout_menu_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i >= KBD_LAYOUT_COUNT || i == g_settings.kbd_layout) return;
    g_settings.kbd_layout = i;
    settings_save();
    keyboard_set_layout(i);
    ui_nav_refresh();
}
static void kbd_layout_cb(lv_event_t *e)
{
    (void)e;
    const char *items[KBD_LAYOUT_COUNT] = { tr(STR_KBD_LAYOUT_STOCK), tr(STR_KBD_LAYOUT_JIS) };
    ui_menu(tr(STR_SET_KBD_LAYOUT), items, KBD_LAYOUT_COUNT, kbd_layout_menu_cb, NULL);
}

static void led_bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    g_settings.kbd_led_bright = v;
    power_kbd_light_refresh();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void led_mode_cb(lv_event_t *e)
{
    g_settings.kbd_led_mode = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_save();
    power_kbd_light_refresh();
}
static void led_needs_cb(lv_event_t *e)
{
    g_settings.kbd_needs_light = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_save();
    power_kbd_light_refresh();
}
/* No gold or orange: amber on the keyboard means needs-you (pr_rgb_amber_like) */
static const uint32_t s_led_presets[] = { 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF };
static void led_color_menu_cb(int i, void *ud)
{
    (void)ud; if (i < 0) return;
    uint32_t c = s_led_presets[i];
    g_settings.kbd_led_rgb[0] = c >> 16; g_settings.kbd_led_rgb[1] = (c >> 8) & 255; g_settings.kbd_led_rgb[2] = c & 255;
    g_settings.kbd_led_mode = 1;
    power_kbd_light_refresh();
    settings_save(); ui_nav_refresh();
}
static void led_color_cb(lv_event_t *e)
{
    (void)e;
    const char *items[] = { "Blue", "White", "Red", "Green", "Magenta", "Cyan" };
    ui_menu(tr(STR_SET_KBD_LED), items, 6, led_color_menu_cb, NULL);
}
static void appearance_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_appearance_create, NULL); }
static void backup_cb(lv_event_t *e) { (void)e; ui_nav_push(ui_backup_create, NULL); }

static void creds_sd_cb(lv_event_t *e)
{
    bool want = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (!server_store_set_on_sd(want)) ui_toast(tr(STR_CREDS_NO_SD));
    ui_nav_refresh();
}
static void creds_reload_cb(lv_event_t *e)
{
    (void)e;
    server_store_reload();
    ui_toastf("%s: %d", tr(STR_SERVERS), server_store_count());
}
static void reboot_yes(bool yes, void *ud) { (void)ud; if (yes) esp_restart(); }
static void reboot_cb(lv_event_t *e) { (void)e; ui_confirm(tr(STR_REBOOT), tr(STR_REBOOT_Q), reboot_yes, NULL); }
static void about_cb(lv_event_t *e) { (void)e; char b[400]; snprintf(b, sizeof(b), "v%s\n%s", sysinfo_app_version(), tr(STR_ABOUT_TEXT)); ui_alert(tr(STR_ABOUT), b); }
static void ota_url_cb(const char *v, void *ud) { (void)ud; if (v) { strlcpy(g_settings.ota_url, v, sizeof(g_settings.ota_url)); settings_save(); ui_nav_refresh(); } }
static void ota_url_row_cb(lv_event_t *e) { (void)e; ui_prompt(tr(STR_OTA_URL), "https://", g_settings.ota_url, false, ota_url_cb, NULL); }

/* Power: the ladder's timings, wakes and the time zone (Plan 2). Menus of presets; the values go
   through power_setting_set, so they get the console's clamps. */
typedef struct { const char *title, *name, *unit; const int *opts; int n; } pw_menu_t;
static const int k_dim_opts[] = { 10, 15, 30, 60, 120, 300, 600 };
static const int k_off_opts[] = { 20, 30, 60, 120, 300, 600, 1800, 3600 };
static const int k_deep_opts[] = { 0, 15, 30, 60, 120, 240, 480, 720 };
static const int k_hib_opts[] = { 0, 1, 2, 4, 8, 12, 24, 48 };
#define N_OPTS(a) ((int)(sizeof(a) / sizeof(a[0])))
static const pw_menu_t k_pw_dim = { "Dim after (s)", "dim", "s", k_dim_opts, N_OPTS(k_dim_opts) };
static const pw_menu_t k_pw_off = { "Screen off after (s)", "off", "s", k_off_opts, N_OPTS(k_off_opts) };
static const pw_menu_t k_pw_deep = { "Deep idle after (min)", "deep-idle", "min", k_deep_opts, N_OPTS(k_deep_opts) };
static const pw_menu_t k_pw_hib = { "Hibernate after (h)", "hibernate", "h", k_hib_opts, N_OPTS(k_hib_opts) };
static char s_pw_items[16][32];
static const char *s_pw_ptrs[16];

static void pw_value(const pw_menu_t *m, int v, char *buf, size_t cap)
{
    if (v == 0 && m->opts[0] == 0) snprintf(buf, cap, "Off");
    else snprintf(buf, cap, "%d %s", v, m->unit);
}
static void pw_menu_cb(int i, void *ud)
{
    const pw_menu_t *m = ud;
    if (i < 0 || i >= m->n) return;
    power_setting_set(m->name, m->opts[i]);
    ui_nav_refresh();
}
static void pw_row_cb(lv_event_t *e)
{
    const pw_menu_t *m = lv_event_get_user_data(e);
    for (int i = 0; i < m->n; i++) {
        pw_value(m, m->opts[i], s_pw_items[i], sizeof(s_pw_items[i]));
        s_pw_ptrs[i] = s_pw_items[i];
    }
    ui_menu(m->title, s_pw_ptrs, m->n, pw_menu_cb, (void *)m);
}
static void pw_row(lv_obj_t *parent, const pw_menu_t *m, int v, const char *override)
{
    char buf[40];
    pw_value(m, v, buf, sizeof(buf));
    ui_setting_row(parent, m->title, override ? override : buf, pw_row_cb, (void *)m);
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
static void tz_menu_cb(int i, void *ud)
{
    (void)ud;
    if (i < 0 || !vitals_tz_presets[i].rule) return;
    strlcpy(g_settings.tz_rule, vitals_tz_presets[i].rule, sizeof(g_settings.tz_rule));
    settings_save();
    sysinfo_apply_timezone();
    ui_nav_refresh();
}
static void tz_cb(lv_event_t *e)
{
    (void)e;
    int n = 0;
    while (vitals_tz_presets[n].label && n < 16) { s_pw_ptrs[n] = vitals_tz_presets[n].label; n++; }
    ui_menu("Time zone", s_pw_ptrs, n, tz_menu_cb, NULL);
}
static const char *tz_label(void)
{
    for (int i = 0; vitals_tz_presets[i].label; i++)
        if (strcmp(vitals_tz_presets[i].rule, g_settings.tz_rule) == 0) return vitals_tz_presets[i].label;
    return g_settings.tz_rule;
}

/* Companion: which of the SSH servers is the Mac, and where `claude` lives on it. */
static char s_server_names[SERVER_MAX + 1][48];
static const char *s_server_ptrs[SERVER_MAX + 1];
static void companion_mac_menu_cb(int i, void *ud)
{
    (void)ud;
    int n = server_store_count();
    if (i < 0 || i > n) return;
    cc_link_set_server(i < n ? s_server_names[i] : "");
    ui_nav_refresh();
}
static void companion_mac_cb(lv_event_t *e)
{
    (void)e;
    int n = server_store_count();
    if (n > SERVER_MAX) n = SERVER_MAX;
    for (int i = 0; i < n; i++) {
        const server_t *s = server_store_get(i);
        strlcpy(s_server_names[i], s ? s->name : "?", sizeof(s_server_names[i]));
        s_server_ptrs[i] = s_server_names[i];
    }
    strlcpy(s_server_names[n], "None", sizeof(s_server_names[n]));
    s_server_ptrs[n] = s_server_names[n];
    ui_menu("Mac", s_server_ptrs, n + 1, companion_mac_menu_cb, NULL);
}
static void companion_path_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_claude_path(v)) { ui_toast("Use only letters, digits and _ . / ~ - (1 to 63 characters)"); return; }
    ui_nav_refresh();
}
static void companion_path_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    ui_prompt("Claude path", "~/.local/bin/claude", cc_settings_claude_path(cur, sizeof(cur)), false, companion_path_prompt_cb, NULL);
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *label, int min, int max, int val, lv_event_cb_t cb)
{
    lv_obj_t *card = ui_card(parent);
    ui_label(card, label, FONT_BODY, UI_TEXT);
    lv_obj_t *s = lv_slider_create(card);
    lv_obj_set_width(s, LV_PCT(96));
    lv_obj_set_style_margin_all(s, 8, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static void sb_bit_cb(lv_event_t *e)
{
    unsigned bit = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED)) g_settings.status_items |= bit;
    else g_settings.status_items &= ~bit;
    settings_save();
}

/* Keep the selected category when returning from a nested setting. Each panel
 * has its own scroll area beside the category menu. */
static unsigned s_category;
static const str_id_t s_categories[] = {
    STR_SET_SYSTEM, STR_SET_DISPLAY, STR_SET_POWER, STR_SET_STATUSBAR, STR_SET_INPUT,
    STR_SET_SECURITY, STR_SET_BACKUP, STR_ABOUT
};
/* Only the visible category is built: LVGL's small objects land in internal RAM, which TLS and
   the Wi-Fi link need, and the dashboard is built before Settings is freed when going home. */
static void panel_fill(lv_obj_t *c, unsigned category)
{
    char buf[128];
    switch (category) {
    case 0: {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ui_setting_row(c, tr(STR_SET_NTP), buf, ntp_cb, NULL);
        ui_setting_row(c, tr(STR_OTA_URL), g_settings.ota_url, ota_url_row_cb, NULL);
        ui_section_title(c, "Companion");
        char mac[48], cpath[64];
        cc_settings_server_name(mac, sizeof(mac));
        ui_setting_row(c, "Mac", mac[0] ? mac : "Not set", companion_mac_cb, NULL);
        ui_setting_row(c, "Claude path", cc_settings_claude_path(cpath, sizeof(cpath)), companion_path_cb, NULL);
        ui_label(c, "The Mac is one of your SSH servers. The Tab5 lists its Claude Code sessions and runs claude from this path.", FONT_SMALL, UI_MUTED);
        break;
    }
    case 1: {
        slider_row(c, tr(STR_SET_BRIGHTNESS), 5, 100, g_settings.brightness, bright_cb);
        snprintf(buf, sizeof(buf), "%d°", g_settings.rotation * 90);
        ui_setting_row(c, tr(STR_SET_ROTATION), buf, rot_cb, NULL);
        ui_setting_row(c, tr(STR_SET_THEME), "", appearance_cb, NULL);
        ui_label(c, tr(STR_SET_SCREENSHOT_NOTE), FONT_SMALL, UI_MUTED);
        break;
    }
    case 2: {
        pw_row(c, &k_pw_dim, g_settings.pw_dim_s, NULL);
        pw_row(c, &k_pw_off, g_settings.pw_off_s, NULL);
        ui_switch_row(c, "Sleep on USB power", g_settings.pw_sleep_on_usb, pw_switch_cb, (void *)"sleep-usb");
        ui_switch_row(c, "Wake when a session needs you", g_settings.pw_wake_needs_you, pw_switch_cb, (void *)"needs-wake");
        ui_switch_row(c, tr(STR_SET_KBD_NEEDS_LIGHT), g_settings.kbd_needs_light, led_needs_cb, NULL);
        ui_switch_row(c, "Stay awake in SSH Terminal", g_settings.ss_disable_in_ssh, ssh_awake_cb, NULL);
        ui_setting_row(c, "Time zone", tz_label(), tz_cb, NULL);
        pw_row(c, &k_pw_deep, g_settings.pw_deep_idle_min, NULL);
        pw_row(c, &k_pw_hib, g_settings.pw_hibernate_h, hibernate_supported() ? NULL : "(not available yet)");
        ui_label(c, "Deep idle and Hibernate apply on battery only. Any key or touch wakes the screen; "
                    "the key or touch that wakes it is not passed on.", FONT_SMALL, UI_MUTED);
        break;
    }
    case 3: {
        static const struct { str_id_t id; unsigned bit; } sb_items[] = {
            { STR_SB_WIFI, STATUS_WIFI }, { STR_SB_TAILSCALE, STATUS_TAILSCALE },
            { STR_SB_IP, STATUS_IP },     { STR_SB_BATTERY, STATUS_BATTERY },
            { STR_SB_KEYBOARD, STATUS_KEYBOARD }, { STR_SB_CLOCK, STATUS_CLOCK },
            { STR_SB_DATE, STATUS_DATE }, { STR_SB_YEAR, STATUS_YEAR },
        };
        for (size_t i = 0; i < sizeof(sb_items) / sizeof(sb_items[0]); i++)
            ui_switch_row(c, tr(sb_items[i].id), (g_settings.status_items & sb_items[i].bit) != 0,
                          sb_bit_cb, (void *)(uintptr_t)sb_items[i].bit);
        ui_label(c, tr(STR_SB_NOTE), FONT_SMALL, UI_MUTED);
        break;
    }
    case 4: {
        ui_label(c, tr(STR_GESTURE_NOTE), FONT_SMALL, UI_MUTED);
        snprintf(buf, sizeof(buf), tr(STR_SET_KBD_STATUS), keyboard_present() ? tr(STR_PRESENT) : (keyboard_usb_present() ? "USB" : tr(STR_ABSENT)));
        ui_label(c, buf, FONT_SMALL, UI_MUTED);
        ui_setting_row(c, tr(STR_SET_KBD_LAYOUT),
                       tr(g_settings.kbd_layout == KBD_LAYOUT_JIS ? STR_KBD_LAYOUT_JIS : STR_KBD_LAYOUT_STOCK),
                       kbd_layout_cb, NULL);
        if (g_settings.kbd_layout != KBD_LAYOUT_STOCK) ui_label(c, tr(STR_SET_KBD_LAYOUT_NOTE), FONT_SMALL, UI_MUTED);
        ui_switch_row(c, tr(STR_SET_KBD_LED), g_settings.kbd_led_mode, led_mode_cb, NULL);
        slider_row(c, tr(STR_SET_KBD_LED_BRIGHT), 0, 100, g_settings.kbd_led_bright, led_bright_cb);
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", g_settings.kbd_led_rgb[0], g_settings.kbd_led_rgb[1], g_settings.kbd_led_rgb[2]);
        lv_obj_t *lc = ui_setting_row(c, tr(STR_SET_KBD_LED), buf, led_color_cb, NULL);
        lv_obj_set_style_text_color(lv_obj_get_user_data(lc), UI_TEXT, 0);
        break;
    }
    case 5: {
        ui_switch_row(c, tr(STR_CREDS_ON_SD), g_settings.creds_on_sd, creds_sd_cb, NULL);
        ui_label(c, tr(STR_CREDS_NOTE), FONT_SMALL, UI_MUTED);
        if (g_settings.creds_on_sd) ui_setting_row(c, tr(STR_CREDS_RELOAD), "", creds_reload_cb, NULL);
        break;
    }
    case 6: {
        ui_setting_row(c, tr(STR_SET_BACKUP), "", backup_cb, NULL);
        break;
    }
    case 7: {
        snprintf(buf, sizeof(buf), "v%s", sysinfo_app_version());
        ui_setting_row(c, tr(STR_ABOUT), buf, about_cb, NULL);
        ui_button_colored(c, tr(STR_REBOOT), UI_DANGER, reboot_cb, NULL);
        break;
    }
    default: break;
    }
}

static void panel_clear(lv_obj_t *panel, unsigned category)
{
    lv_obj_clean(panel);
    ui_section_title(panel, tr(s_categories[category]));
}

static void category_cb(lv_event_t *e)
{
    unsigned next = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (next >= sizeof(s_categories) / sizeof(s_categories[0]) || next == s_category) return;
    lv_obj_t *nav = lv_obj_get_parent(lv_event_get_target_obj(e));
    lv_obj_t *deck = lv_obj_get_child(lv_obj_get_parent(nav), 1);
    unsigned previous = s_category;
    lv_obj_add_flag(lv_obj_get_child(deck, previous), LV_OBJ_FLAG_HIDDEN);
    panel_clear(lv_obj_get_child(deck, previous), previous);
    panel_fill(lv_obj_get_child(deck, next), next);
    lv_obj_remove_flag(lv_obj_get_child(deck, next), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *old_button = lv_obj_get_child(nav, previous);
    lv_obj_t *new_button = lv_obj_get_child(nav, next);
    lv_obj_set_style_bg_color(old_button, UI_CARD_HI, 0);
    lv_obj_set_style_text_color(old_button, UI_TEXT, 0);
    lv_obj_set_style_bg_color(new_button, UI_ACCENT, 0);
    lv_obj_set_style_text_color(new_button, ui_theme_pixel() ? UI_BG : UI_TEXT, 0);
    s_category = next;
    ui_motion_start(lv_obj_get_screen(deck), deck, next < previous);
}
static lv_obj_t *settings_panel(lv_obj_t *deck, unsigned category)
{
    lv_obj_t *panel = ui_card(deck);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    if (category != s_category) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    ui_section_title(panel, tr(s_categories[category]));
    return panel;
}

lv_obj_t *ui_settings_create(void *arg)
{
    (void)arg;
    lv_obj_t *scr = ui_screen_base();
    ui_topbar(scr, tr(STR_SETTINGS), true);
    lv_obj_t *body = ui_content(scr);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, ui_theme_pad(14), 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nav = ui_card(body);
    lv_obj_set_width(nav, LV_PCT(24));
    lv_obj_set_style_min_width(nav, 212, 0);
    lv_obj_set_height(nav, LV_PCT(100));
    lv_obj_add_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nav, LV_DIR_VER);
    for (unsigned i = 0; i < sizeof(s_categories) / sizeof(s_categories[0]); i++) {
        lv_obj_t *b = ui_button_colored(nav, tr(s_categories[i]),
                                       i == s_category ? UI_ACCENT : UI_CARD_HI,
                                       category_cb, (void *)(uintptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, 64);
        lv_obj_t *label = lv_obj_get_child(b, 0);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_font(label, FONT_SMALL, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *deck = lv_obj_create(body);
    lv_obj_remove_style_all(deck);
    lv_obj_set_height(deck, LV_PCT(100));
    lv_obj_set_flex_grow(deck, 1);
    lv_obj_remove_flag(deck, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < sizeof(s_categories) / sizeof(s_categories[0]); i++) {
        lv_obj_t *c = settings_panel(deck, i);
        if (i == s_category) panel_fill(c, i);
    }
    return scr;
}
