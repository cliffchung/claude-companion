#pragma once
#include <stdint.h>
#include <stddef.h>
#include "settings/settings.h"
/* The settings blob's layout and its pure migrations (no NVS), so the host tests cover them. */
#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAGIC   0x54414235u
#define CFG_VERSION 21

typedef struct {
    uint32_t magic;
    uint32_t version;
    app_settings_t s;
} cfg_blob_t;

/* Loads an NVS blob over s, which holds the defaults. A current blob is taken as it is; an older (or
   other) version copies what fits and applies the pure migrations: from 20 or older, ts_enable = 0
   and the v21 addresses cleared (the old tail padding may have landed on them). Returns the blob's
   version, or 0 when the blob isn't ours (s keeps the defaults). The caller applies the migrations
   that need ESP-IDF for versions below CFG_VERSION, then saves. */
uint32_t settings_blob_load(app_settings_t *s, const void *blob, size_t len);
/* The blob settings_save writes. */
cfg_blob_t settings_blob_make(const app_settings_t *s);
/* Settings › About › Reset settings (spec §5.12, U3): every pw_* field, ss_disable_in_ssh, the keyboard light
   (kbd_led_mode, kbd_led_bright, kbd_led_rgb, kbd_needs_light, kbd_dark_light) and brightness take their value
   from defaults. Every other field is kept: Wi-Fi, the Host and the Terminal hosts, the pins, Tailscale, the
   time zone, the key layout, the OTA URL. */
void settings_reset_scope(app_settings_t *s, const app_settings_t *defaults);

#ifdef __cplusplus
}
#endif
