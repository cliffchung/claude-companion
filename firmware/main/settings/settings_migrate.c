#include "settings_migrate.h"
#include <string.h>

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
    }
    s->mac_lan_ip[sizeof(s->mac_lan_ip) - 1] = 0;
    s->mac_ts_ip[sizeof(s->mac_ts_ip) - 1] = 0;
    return version;
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
}
