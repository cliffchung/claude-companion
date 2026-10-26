#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "settings/settings.h"
/* The settings blob's layout and its pure migrations (no NVS), so the host tests cover them. */
#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAGIC   0x54414235u
/* 21 is Plan 3's, 23 Plan 4's, 24 Plan 5's, 25 Plan 6's. 22 was reserved for Plan 2's Task 10; a reserved lower
   number can't be used once a higher one ships, so if it lands later it takes 26. Each step is keyed
   "old version < N", so the order the plans land in doesn't matter. */
#define CFG_VERSION 25

typedef struct {
    uint32_t magic;
    uint32_t version;
    app_settings_t s;
} cfg_blob_t;

/* Loads an NVS blob over s, which holds the defaults. A current blob is taken as it is; an older (or
   other) version copies what fits and applies the pure migrations: from 20 or older, ts_enable = 0
   and the v21 addresses cleared (the old tail padding may have landed on them); below 23, the token
   terminal values (term_fg, term_bg, term_cursor, term_ansi), a steady block cursor, line height 32,
   rotation 1 and an amber-like kbd_led_rgb back to 0x0080FF; below 24, whisper_path and whisper_model take their
   defaults (CC_WHISPER_DEFAULT, CC_WHISPER_MODEL_DEFAULT: the old tail padding may have landed on them); below 25,
   fw_path takes CC_FW_PATH_DEFAULT and ota_url (reserved) is zeroed. A newer blob (from a later build) only
   copies what fits. Returns the blob's
   version, or 0 when the blob isn't ours (s keeps the defaults). The caller applies the migrations
   that need ESP-IDF for versions below CFG_VERSION, then saves. */
uint32_t settings_blob_load(app_settings_t *s, const void *blob, size_t len);
/* CFG 23 step 3, the hosts an older build kept on the TF card (spec §10; controller ruling T11-13 over the brief):
   keyed on creds_on_sd == 1 alone, not the version, so a reset between the v23 save and the import retries it.
   attempt: mount and import now (never on a timer check-in boot, which must not pay for a mount). clear: set
   creds_on_sd to 0 and save, only once an import ran (import_result >= 0, the count added); on -1 (no card, or the
   file can't be read) the flag stays and the next full boot tries again. Call with import_result -1 to ask whether
   to attempt, then with the import's result. */
typedef struct { bool attempt, clear; } sd_import_plan_t;
sd_import_plan_t settings_sd_import_step(uint8_t creds_on_sd, bool checkin_boot, int import_result);
/* settings_init's check of the two voice paths: each ends in a NUL and takes its default unless cc_path_valid. */
void settings_voice_paths_check(app_settings_t *s);
/* settings_init's check of the firmware path (v25): it ends in a NUL and takes CC_FW_PATH_DEFAULT unless cc_path_valid. */
void settings_fw_path_check(app_settings_t *s);
/* The blob settings_save writes. */
cfg_blob_t settings_blob_make(const app_settings_t *s);
/* Settings › About › Reset settings (spec §5.12, U3): every pw_* field, ss_disable_in_ssh, the keyboard light
   (kbd_led_mode, kbd_led_bright, kbd_led_rgb, kbd_needs_light, kbd_dark_light) and brightness take their value
   from defaults. Every other field is kept: Wi-Fi, the Host and the Terminal hosts, the pins, Tailscale, the
   time zone, the key layout, the reserved fields, the whisper and model paths, the firmware path. */
void settings_reset_scope(app_settings_t *s, const app_settings_t *defaults);

#ifdef __cplusplus
}
#endif
