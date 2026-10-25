#include "settings_migrate.h"
#include <string.h>
#include "ui/ui_tokens.h"
#include "sys/power_rules.h"
#include "companion/cc_cmd.h"

uint32_t settings_blob_load(app_settings_t *s, const void *blob, size_t len)
{
    if (!s || !blob || len < 2 * sizeof(uint32_t)) return 0;
    const cfg_blob_t *b = blob;
    uint32_t magic, version;
    memcpy(&magic, &b->magic, sizeof(magic));
    memcpy(&version, &b->version, sizeof(version));
    if (magic != CFG_MAGIC) return 0;
    size_t n = len - 2 * sizeof(uint32_t);
    if (version == CFG_VERSION && n >= sizeof(app_settings_t)) {
        memcpy(s, &b->s, sizeof(app_settings_t));
    } else {
        /* older layout: copy what fits, keep defaults for the rest */
        if (n > sizeof(app_settings_t)) n = sizeof(app_settings_t);
        memcpy(s, &b->s, n);
        if (version <= 20) {
            /* v21: Tailscale off once (user, 2026-10-23); the appended addresses start empty */
            s->ts_enable = 0;
            memset(s->mac_lan_ip, 0, sizeof(s->mac_lan_ip));
            memset(s->mac_ts_ip, 0, sizeof(s->mac_ts_ip));
        }
        if (version < 23) {
            /* v23 (Plan 4, spec §10): the terminal slots hold the token values (nothing edits them, and the
               terminal draws the tokens), a steady block cursor (B1), the fixed landscape rotation, and the
               keyboard light's amber-like colour back to blue (settings_init does that on every load too).
               Step 3, the TF-card hosts, needs ESP-IDF: settings_post_load_migrate (settings.c) runs it. */
            s->term_fg = UI_TK_TERM_FG;
            s->term_bg = UI_TK_TERM_BG;
            s->term_cursor = UI_TK_TERM_CURSOR;
            memcpy(s->term_ansi, ui_tk_term_ansi, sizeof(s->term_ansi));
            s->term_cursor_style = 0;
            s->term_cursor_blink = 0;
            s->term_line_height = UI_TK_TERM_LINE_H;
            s->rotation = 1;
            if (pr_rgb_amber_like(((uint32_t)s->kbd_led_rgb[0] << 16) | ((uint32_t)s->kbd_led_rgb[1] << 8) |
                                  s->kbd_led_rgb[2])) {
                s->kbd_led_rgb[0] = 0x00; s->kbd_led_rgb[1] = 0x80; s->kbd_led_rgb[2] = 0xFF;
            }
        }
        if (version < 24) {
            /* v24 (Plan 5): the voice paths appended at the end; the old tail padding may hold anything */
            memset(s->whisper_path, 0, sizeof(s->whisper_path));
            memset(s->whisper_model, 0, sizeof(s->whisper_model));
            strcpy(s->whisper_path, CC_WHISPER_DEFAULT);
            strcpy(s->whisper_model, CC_WHISPER_MODEL_DEFAULT);
        }
        if (version < 25) {
            /* v25 (Plan 6): the firmware path appended at the end; the old OTA manifest URL (the removed SD updater's,
               which could hold a token) wiped, its bytes kept as reserved */
            memset(s->fw_path, 0, sizeof(s->fw_path));
            strcpy(s->fw_path, CC_FW_PATH_DEFAULT);
            memset(s->ota_url, 0, sizeof(s->ota_url));
        }
    }
    s->mac_lan_ip[sizeof(s->mac_lan_ip) - 1] = 0;
    s->mac_ts_ip[sizeof(s->mac_ts_ip) - 1] = 0;
    return version;
}

sd_import_plan_t settings_sd_import_step(uint8_t creds_on_sd, bool checkin_boot, int import_result)
{
    sd_import_plan_t p = { .attempt = creds_on_sd == 1 && !checkin_boot };
    p.clear = p.attempt && import_result >= 0;
    return p;
}

void settings_voice_paths_check(app_settings_t *s)
{
    if (!s) return;
    s->whisper_path[sizeof(s->whisper_path) - 1] = 0;
    s->whisper_model[sizeof(s->whisper_model) - 1] = 0;
    if (!cc_path_valid(s->whisper_path)) {
        memset(s->whisper_path, 0, sizeof(s->whisper_path));
        strcpy(s->whisper_path, CC_WHISPER_DEFAULT);
    }
    if (!cc_path_valid(s->whisper_model)) {
        memset(s->whisper_model, 0, sizeof(s->whisper_model));
        strcpy(s->whisper_model, CC_WHISPER_MODEL_DEFAULT);
    }
}

void settings_fw_path_check(app_settings_t *s)
{
    if (!s) return;
    s->fw_path[sizeof(s->fw_path) - 1] = 0;
    if (!cc_path_valid(s->fw_path)) {
        memset(s->fw_path, 0, sizeof(s->fw_path));
        strcpy(s->fw_path, CC_FW_PATH_DEFAULT);
    }
}

cfg_blob_t settings_blob_make(const app_settings_t *s)
{
    cfg_blob_t b = { .magic = CFG_MAGIC, .version = CFG_VERSION, .s = *s };
    return b;
}

void settings_reset_scope(app_settings_t *s, const app_settings_t *d)
{
    if (!s || !d) return;
    /* power (Plan 2): pw_dim_s through pw_hibernate_checkin_min, which hold the two needs-you light fields too */
    s->pw_dim_s = d->pw_dim_s;
    s->pw_off_s = d->pw_off_s;
    s->pw_sleep_on_usb = d->pw_sleep_on_usb;
    s->pw_wake_lift = d->pw_wake_lift;
    s->pw_wake_needs_you = d->pw_wake_needs_you;
    s->pw_radio_off = d->pw_radio_off;
    s->pw_radio_off_min = d->pw_radio_off_min;
    s->pw_checkin_min = d->pw_checkin_min;
    s->pw_light_sleep = d->pw_light_sleep;
    s->pw_deep_idle_min = d->pw_deep_idle_min;
    s->pw_hibernate_h = d->pw_hibernate_h;
    s->pw_hibernate_checkin_h = d->pw_hibernate_checkin_h;
    s->pw_hibernate_checkin_min = d->pw_hibernate_checkin_min;
    s->ss_disable_in_ssh = d->ss_disable_in_ssh;
    /* the keyboard light */
    s->kbd_led_mode = d->kbd_led_mode;
    s->kbd_led_bright = d->kbd_led_bright;
    memcpy(s->kbd_led_rgb, d->kbd_led_rgb, sizeof(s->kbd_led_rgb));
    s->kbd_needs_light = d->kbd_needs_light;
    s->kbd_dark_light = d->kbd_dark_light;
    /* display */
    s->brightness = d->brightness;
    /* whisper_path and whisper_model (v24) and fw_path (v25) are kept: they describe the Host, like claude_path */
}
