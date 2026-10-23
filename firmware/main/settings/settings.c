#include "settings.h"
#include "ui/ui_theme.h"
#include "input/keyboard.h"
#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "companion/cc_cmd.h"
#include "sys/vitals.h"

static const char *TAG = "settings";
#define NS "app"
#define KEY "cfg"
#define CFG_VERSION 18

app_settings_t g_settings;

typedef struct {
    uint32_t magic;
    uint32_t version;
    app_settings_t s;
} cfg_blob_t;

static void plan2_defaults(void)
{
    strcpy(g_settings.tz_rule, TZ_DEFAULT);
    g_settings.pw_dim_s = 30;
    g_settings.pw_off_s = 120;
    g_settings.pw_sleep_on_usb = 1;
    g_settings.pw_wake_lift = 1;
    g_settings.pw_wake_needs_you = 1;
    g_settings.kbd_needs_light = 1;
    g_settings.kbd_off_cap = 100;
    g_settings.pw_radio_off = 0;
    g_settings.pw_radio_off_min = 20;
    g_settings.pw_checkin_min = 5;
    g_settings.pw_light_sleep = 0;
    g_settings.pw_deep_idle_min = 60;
    g_settings.pw_hibernate_h = 2;
    g_settings.pw_hibernate_checkin_h = 0;
}

void settings_reset_defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.rotation = 1;            /* landscape, USB-C on the left */
    g_settings.brightness = 80;
    g_settings.ss_enable = 1;
    g_settings.ss_idle_min = 5;
    g_settings.ss_switch_min = 1;
    g_settings.ss_disable_in_ssh = 1;
    strcpy(g_settings.ss_wallpaper_dir, "");
    g_settings.ss_elem_x[0] = 40;  g_settings.ss_elem_y[0] = 40;   /* clock */
    g_settings.ss_elem_x[1] = 40;  g_settings.ss_elem_y[1] = 160;  /* date */
    g_settings.ss_elem_x[2] = 40;  g_settings.ss_elem_y[2] = 220;  /* battery */
    g_settings.ss_elem_x[3] = 40;  g_settings.ss_elem_y[3] = 270;  /* network */
    for (int i = 0; i < 4; i++) g_settings.ss_elem_color[i] = 0xFFFFFF;
    g_settings.utc_offset_min = 9 * 60;
    g_settings.kbd_led_mode = 0;
    g_settings.kbd_led_bright = 20;
    g_settings.kbd_led_rgb[0] = 0; g_settings.kbd_led_rgb[1] = 128; g_settings.kbd_led_rgb[2] = 255;
    g_settings.status_items = STATUS_DEFAULT;
    g_settings.creds_on_sd = 0;
    g_settings.term_cursor_style = 0;
    g_settings.term_cursor_blink = 1;
    g_settings.term_line_height = 32;   /* 24 px glyphs with readable row spacing */
    /* The look, including the terminal palette, comes from preset 0. */
    ui_theme_set_preset(0);
    g_settings.term_font = 0;
    g_settings.volume = 60;
    g_settings.ota_url[0] = 0;           /* no update manifest yet; the OTA tile is hidden */
    g_settings.ssh_bell = 1;
    g_settings.ts_enable = 0;
    g_settings.ts_direct = 0;
    strcpy(g_settings.ts_device_name, "claude-companion");
    g_settings.companion_server[0] = 0;
    strcpy(g_settings.claude_path, "~/.local/bin/claude");
    plan2_defaults();
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
        cfg_blob_t *b = blob;
        if (len >= sizeof(cfg_blob_t) && b->magic == 0x54414235 && b->version == CFG_VERSION) {
            g_settings = b->s;
        } else if (len >= sizeof(uint32_t) * 2 && b->magic == 0x54414235) {
            /* older layout: copy what fits, keep defaults for the rest */
            uint32_t old_version = b->version;
            size_t n = len - 2 * sizeof(uint32_t);
            if (n > sizeof(app_settings_t)) n = sizeof(app_settings_t);
            memcpy(&g_settings, &b->s, n);
            /* Previous releases left existing compact values unchanged unless
             * they were exactly 26. Give all old compact settings breathing room;
             * preserve larger spacing and allow subsequent manual adjustments. */
            if (old_version <= 11 && g_settings.term_line_height <= 30)
                g_settings.term_line_height = 32;
            if (old_version <= 12) ui_theme_set_terminal_amber();
            if (old_version <= 13 && (g_settings.term_line_height == 34 || g_settings.term_line_height < 28))
                g_settings.term_line_height = 32;
            if (old_version <= 14) g_settings.ss_classic = 0;
            if (old_version <= 15) g_settings.kbd_layout = KBD_LAYOUT_STOCK;
            /* v17 added companion_server and claude_path at the end: defaults stand. */
            /* v18 added the Plan 2 fields at the end. The old blob's tail padding may have
               landed on them, so set them all, whatever the old UTC offset was. */
            if (old_version <= 17) plan2_defaults();
            settings_save();
        }
        free(blob);
    }
    /* Builds from the upstream fork stored its manifest URL as the default; drop it. */
    if (strstr(g_settings.ota_url, "github") && strstr(g_settings.ota_url, "Mozgi512/")) g_settings.ota_url[0] = 0;
    if (g_settings.brightness < 5) g_settings.brightness = 5;
    if (g_settings.ss_idle_min == 0) g_settings.ss_idle_min = 5;
    g_settings.companion_server[sizeof(g_settings.companion_server) - 1] = 0;
    g_settings.claude_path[sizeof(g_settings.claude_path) - 1] = 0;
    g_settings.tz_rule[sizeof(g_settings.tz_rule) - 1] = 0;
    if (!vitals_tz_valid(g_settings.tz_rule)) strcpy(g_settings.tz_rule, TZ_DEFAULT);
    vitals_clamp_dim_off(&g_settings.pw_dim_s, &g_settings.pw_off_s);
    g_settings.pw_deep_idle_min = vitals_clamp_or_zero(g_settings.pw_deep_idle_min, 15, 720);
    g_settings.pw_hibernate_h = vitals_clamp_or_zero(g_settings.pw_hibernate_h, 1, 48);
    g_settings.pw_hibernate_checkin_h = vitals_clamp_or_zero(g_settings.pw_hibernate_checkin_h, 1, 24);
    if (!cc_path_valid(g_settings.claude_path)) strcpy(g_settings.claude_path, "~/.local/bin/claude");
}

void settings_save(void)
{
    cfg_blob_t b = { .magic = 0x54414235, .version = CFG_VERSION, .s = g_settings };
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
