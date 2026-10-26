#include "settings.h"
#include "settings_migrate.h"
#include "ui/ui_tokens.h"
#include "input/keyboard.h"
#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "companion/cc_cmd.h"
#include "sys/vitals.h"
#include "sys/power_rules.h"
#include "sys/power.h"
#include "bsp/m5stack_tab5.h"
#include "storage/sd.h"
#include "servers/server_store.h"

static const char *TAG = "settings";
#define NS "app"
#define KEY "cfg"
/* CFG_VERSION, CFG_MAGIC and the blob layout are in settings_migrate.h. */

app_settings_t g_settings;

static void power_defaults(app_settings_t *s)
{
    s->pw_dim_s = 30;
    s->pw_off_s = 120;
    s->pw_sleep_on_usb = 1;
    s->pw_wake_needs_you = 1;
    s->kbd_needs_light = 1;
    s->kbd_dark_light = PR_DARK_LIGHT_DEFAULT;   /* PT24 */
    s->pw_radio_off = 0;
    s->pw_radio_off_min = 20;
    s->pw_checkin_min = 5;
    s->pw_light_sleep = 0;
    s->pw_deep_idle_min = 60;
    s->pw_hibernate_h = 2;
    s->pw_hibernate_checkin_h = 0;      /* reserved */
    s->pw_hibernate_checkin_min = 15;   /* PT16, PT18 */
}

void settings_power_defaults(void) { power_defaults(&g_settings); }

static void plan2_defaults(app_settings_t *s)
{
    strcpy(s->tz_rule, TZ_DEFAULT);
    power_defaults(s);
}

void settings_defaults(app_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rotation = 1;            /* reserved (Plan 4): the display is always ROTATION_90 */
    out->brightness = 80;
    out->ss_enable = 1;
    out->ss_idle_min = 5;
    out->ss_switch_min = 1;
    out->ss_disable_in_ssh = 1;
    out->ss_elem_x[0] = 40;  out->ss_elem_y[0] = 40;   /* clock */
    out->ss_elem_x[1] = 40;  out->ss_elem_y[1] = 160;  /* date */
    out->ss_elem_x[2] = 40;  out->ss_elem_y[2] = 220;  /* battery */
    out->ss_elem_x[3] = 40;  out->ss_elem_y[3] = 270;  /* network */
    for (int i = 0; i < 4; i++) out->ss_elem_color[i] = 0xFFFFFF;
    out->utc_offset_min = 9 * 60;
    out->kbd_led_mode = 0;
    out->kbd_led_bright = 20;
    out->kbd_led_rgb[0] = 0; out->kbd_led_rgb[1] = 128; out->kbd_led_rgb[2] = 255;
    out->creds_on_sd = 0;
    out->term_cursor_style = 0;
    out->term_cursor_blink = 0;   /* B1: a steady cursor, no idle redraw */
    out->term_line_height = UI_TK_TERM_LINE_H;
    /* Plan 4: one look, no themes. The terminal palette comes from the tokens. */
    out->term_fg = UI_TK_TERM_FG;
    out->term_bg = UI_TK_TERM_BG;
    out->term_cursor = UI_TK_TERM_CURSOR;
    memcpy(out->term_ansi, ui_tk_term_ansi, sizeof(out->term_ansi));
    out->term_font = 0;
    out->volume = 60;
    memset(out->ota_url, 0, sizeof(out->ota_url));   /* reserved (Plan 6) */
    out->ssh_bell = 1;
    out->ts_enable = 0;
    out->ts_direct = 0;
    strcpy(out->ts_device_name, "claude-companion");
    out->companion_server[0] = 0;
    strcpy(out->claude_path, "~/.local/bin/claude");
    strcpy(out->whisper_path, CC_WHISPER_DEFAULT);         /* v24, Plan 5 */
    strcpy(out->whisper_model, CC_WHISPER_MODEL_DEFAULT);
    strcpy(out->fw_path, CC_FW_PATH_DEFAULT);              /* v25, Plan 6 */
    plan2_defaults(out);
}

void settings_reset_defaults(void) { settings_defaults(&g_settings); }

void settings_reset_user(void)
{
    app_settings_t *d = malloc(sizeof(*d));   /* about 1.5 KB: not on the LVGL task's stack */
    if (!d) return;
    settings_defaults(d);
    settings_reset_scope(&g_settings, d);
    free(d);
    /* one NVS write: power_settings_defaults saves the whole blob (the scope above included), ends any console
       test value and refreshes the keyboard light, which also wakes the power task for the new timings */
    power_settings_defaults();
    bsp_display_brightness_set(g_settings.brightness);
}

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase + reinit");
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_reset_defaults();
    void *blob = NULL; size_t len = 0;
    if (nvs_get_blob_alloc(NS, KEY, &blob, &len)) {
        uint32_t old_version = settings_blob_load(&g_settings, blob, len);
        if (old_version && old_version != CFG_VERSION) {
            /* other layout: settings_blob_load copied what fits and ran the pure steps (v21 Tailscale off once;
               v23 the token terminal values, rotation 1, no amber keyboard light; v24 the default voice paths). The old line-height fixes
               for v11 and v13 are gone: below 23 the height is 32. */
            ESP_LOGW(TAG, "settings v%u -> v%d", (unsigned)old_version, CFG_VERSION);
            if (old_version <= 14) g_settings.ss_classic = 0;
            if (old_version <= 15) g_settings.kbd_layout = KBD_LAYOUT_STOCK;
            /* v17 added companion_server and claude_path at the end: defaults stand. */
            /* v18 added the Plan 2 fields at the end. The old blob's tail padding may have
               landed on them, so set them all, whatever the old UTC offset was. */
            if (old_version <= 17) plan2_defaults(&g_settings);
            /* v19 added pw_hibernate_checkin_min (PT16): an existing device gets the default */
            if (old_version <= 18) g_settings.pw_hibernate_checkin_min = 15;
            /* v20 (PT24) reuses kbd_off_cap's byte (a battery Off cap, default 100 = none) as kbd_dark_light:
               the old value means something else, so an existing device gets the default */
            g_settings.kbd_dark_light = (uint8_t)pr_dark_light_load(old_version, g_settings.kbd_dark_light);
            if (old_version <= 20) ESP_LOGW(TAG, "settings v%u -> v%d: Tailscale switched off (once)", (unsigned)old_version, CFG_VERSION);
            settings_save();
        }
        free(blob);
    }
    /* Amber on the keyboard means needs-you only: a saved gold or orange goes back to the default blue. */
    if (pr_rgb_amber_like(((uint32_t)g_settings.kbd_led_rgb[0] << 16) | ((uint32_t)g_settings.kbd_led_rgb[1] << 8) |
                          g_settings.kbd_led_rgb[2])) {
        g_settings.kbd_led_rgb[0] = 0x00; g_settings.kbd_led_rgb[1] = 0x80; g_settings.kbd_led_rgb[2] = 0xFF;
        settings_save();
    }
    if (g_settings.brightness < 5) g_settings.brightness = 5;
    if (g_settings.ss_idle_min == 0) g_settings.ss_idle_min = 5;
    g_settings.companion_server[sizeof(g_settings.companion_server) - 1] = 0;
    g_settings.claude_path[sizeof(g_settings.claude_path) - 1] = 0;
    g_settings.tz_rule[sizeof(g_settings.tz_rule) - 1] = 0;
    if (!vitals_tz_valid(g_settings.tz_rule)) strcpy(g_settings.tz_rule, TZ_DEFAULT);
    vitals_clamp_dim_off(&g_settings.pw_dim_s, &g_settings.pw_off_s);
    g_settings.pw_deep_idle_min = vitals_clamp_or_zero(g_settings.pw_deep_idle_min, 15, 720);
    g_settings.pw_hibernate_h = vitals_clamp_or_zero(g_settings.pw_hibernate_h, 1, 48);
    g_settings.pw_hibernate_checkin_min = (uint16_t)pr_hib_checkin_clamp_min(g_settings.pw_hibernate_checkin_min);
    g_settings.kbd_dark_light = (uint8_t)pr_dark_light_clamp(g_settings.kbd_dark_light);
    if (g_settings.pw_radio_off_min < 5) g_settings.pw_radio_off_min = 5;
    if (g_settings.pw_radio_off_min > 240) g_settings.pw_radio_off_min = 240;
    if (g_settings.pw_checkin_min < 1) g_settings.pw_checkin_min = 1;
    if (g_settings.pw_checkin_min > 60) g_settings.pw_checkin_min = 60;
    if (!cc_path_valid(g_settings.claude_path)) strcpy(g_settings.claude_path, "~/.local/bin/claude");
    settings_voice_paths_check(&g_settings);   /* NUL-terminated, else the default unless cc_path_valid */
    settings_fw_path_check(&g_settings);       /* the same for the firmware path (v25) */
}

void settings_post_load_migrate(bool checkin_boot)
{
    /* CFG 23 step 3 (spec §10, T11-13): the hosts an older build kept on the TF card come into NVS. Keyed on the
       flag alone (settings_sd_import_step), so a reset before it ran, no card, or a read error tries again on the
       next full boot; the card is mounted only for the import and its file is left as it is. */
    if (!settings_sd_import_step(g_settings.creds_on_sd, checkin_boot, -1).attempt) return;
    int n = -1;
    if (!sd_mount()) {
        ESP_LOGW(TAG, "hosts on SD not imported: no card (tries again next boot)");
    } else {
        n = server_store_import_sd_once();
        sd_unmount();
        if (n >= 0) ESP_LOGI(TAG, "hosts on SD imported: %d", n);
        else ESP_LOGW(TAG, "hosts on SD not imported: the file can't be read (tries again next boot)");
    }
    if (settings_sd_import_step(g_settings.creds_on_sd, checkin_boot, n).clear) {
        g_settings.creds_on_sd = 0;   /* reserved from here on (Plan 4) */
        settings_save();
    }
}

void settings_save(void)
{
    cfg_blob_t b = settings_blob_make(&g_settings);
    nvs_set_blob_ns(NS, KEY, &b, sizeof(b));
}

bool nvs_get_blob_alloc(const char *ns, const char *key, void **out, size_t *len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;
    bool ok = false;
    if (nvs_get_blob(h, key, NULL, &n) == ESP_OK && n > 0) {
        void *p = malloc(n);
        if (p && nvs_get_blob(h, key, p, &n) == ESP_OK) { *out = p; *len = n; ok = true; }
        else free(p);
    }
    nvs_close(h);
    return ok;
}

bool nvs_set_blob_ns(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_set_blob %s/%s: %s", ns, key, esp_err_to_name(e));
    return e == ESP_OK;
}

bool nvs_erase_key_ns(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool nvs_get_str_alloc(const char *ns, const char *key, char **out)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0; bool ok = false;
    if (nvs_get_str(h, key, NULL, &n) == ESP_OK && n > 0) {
        char *p = malloc(n);
        if (p && nvs_get_str(h, key, p, &n) == ESP_OK) { *out = p; ok = true; }
        else free(p);
    }
    nvs_close(h);
    return ok;
}

bool nvs_set_str_ns(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
