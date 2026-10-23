#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "i18n/i18n.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Status bar elements, as a bitmask in app_settings_t.status_items. */
#define STATUS_WIFI      (1u << 0)
#define STATUS_TAILSCALE (1u << 1)
#define STATUS_IP        (1u << 2)
#define STATUS_BATTERY   (1u << 3)
#define STATUS_KEYBOARD  (1u << 4)
#define STATUS_CLOCK     (1u << 5)
#define STATUS_DATE      (1u << 6)
#define STATUS_YEAR      (1u << 7)
#define STATUS_DEFAULT   (STATUS_WIFI | STATUS_TAILSCALE | STATUS_IP | STATUS_BATTERY | \
                          STATUS_KEYBOARD | STATUS_CLOCK | STATUS_DATE)

typedef struct {
    /* Reserved fields keep the NVS blob layout of older builds; unused. */
    uint8_t  lang;              /* reserved (was lang_t): English only */
    uint8_t  reserved_src;          /* reserved: formerly the input source */
    uint8_t  reserved_kana_mode;    /* reserved: formerly the kana mode */
    uint8_t  rotation;          /* 0..3 -> lv_display_rotation_t */
    uint8_t  brightness;        /* 5..100 */
    /* ss_*: reserved, so old blobs keep their layout; the fork's screensaver was removed (Plan 2) */
    uint8_t  ss_enable;         /* reserved */
    uint16_t ss_idle_min;       /* reserved */
    uint16_t ss_switch_min;     /* reserved */
    uint8_t  ss_disable_in_ssh; /* in use: "Stay awake in SSH Terminal" (POWER_HOLD_SSH) */
    char     ss_wallpaper_dir[96];   /* reserved */
    int16_t  ss_elem_x[4];      /* reserved */
    int16_t  ss_elem_y[4];      /* reserved */
    uint32_t ss_elem_color[4];  /* reserved */
    int16_t  utc_offset_min;    /* reserved: the time zone is tz_rule */
    uint8_t  kbd_led_mode;      /* 0=bind, 1=user */
    uint8_t  kbd_led_bright;    /* 0..100 */
    uint8_t  kbd_led_rgb[3];
    uint8_t  term_font;         /* 0=22px (only size shipped) */
    uint8_t  volume;            /* 0..100 */
    uint8_t  reserved_autostart;        /* reserved: formerly a web service autostart */
    uint8_t  reserved_dict_sd;          /* reserved: formerly a dictionary option */
    char     ota_url[160];
    uint8_t  ssh_bell;
    uint8_t  ts_enable;
    char     ts_auth_key[160];
    char     ts_device_name[40];
    char     ts_ctrl_host[96];
    uint8_t  ts_direct;          /* try direct UDP paths (experimental); 0 = DERP relay only */
    uint8_t  status_items;       /* STATUS_* bitmask */
    uint8_t  creds_on_sd;        /* keep the SSH server list on the TF card, not in NVS */
    uint8_t  theme_preset;       /* index into the preset table, UI_THEME_CUSTOM for custom */
    uint32_t theme_colors[8];    /* used when the preset is custom */
    uint8_t  theme_radius;       /* corner rounding, 0..24 */
    uint8_t  theme_density;      /* ui_density_t: padding scale */
    uint8_t  theme_tile_accent;  /* tint the home tiles with the accent colour */
    uint8_t  theme_glow;         /* coloured halo behind cards and buttons */
    uint8_t  theme_scanlines;    /* CRT/EL scan lines drawn over every screen */
    uint32_t term_fg, term_bg, term_cursor;
    uint32_t term_ansi[16];
    uint8_t  term_cursor_style;  /* 0 block, 1 underline, 2 bar */
    uint8_t  term_cursor_blink;
    uint8_t  term_line_height;   /* 28..40 px, MaruMinya 24px glyphs + leading */
    uint8_t ss_classic;          /* reserved (the removed screensaver's layout) */
    uint8_t  kbd_layout;         /* kbd_layout_t: STOCK follows the keycaps */
    char     companion_server[48];   /* name of the Mac in the server list; "" = none */
    char     claude_path[64];        /* Claude CLI on the Mac; must pass cc_path_valid */
    /* Plan 2 (v18) */
    char     tz_rule[48];        /* POSIX TZ, default "PST8PDT,M3.2.0,M11.1.0" */
    uint16_t pw_dim_s;           /* 30 */
    uint16_t pw_off_s;           /* 120 */
    uint8_t  pw_sleep_on_usb;    /* 1: dim and screen off also on USB */
    uint8_t  pw_wake_lift;       /* 1 */
    uint8_t  pw_wake_needs_you;  /* 1 */
    uint8_t  kbd_needs_light;    /* 1: keyboard light shows needs-you */
    uint8_t  kbd_off_cap;        /* 100 = no cap; gated, set in Task 10 */
    uint8_t  pw_radio_off;       /* 0; gated, set in Task 10 */
    uint16_t pw_radio_off_min;   /* 20 */
    uint16_t pw_checkin_min;     /* 5 */
    uint8_t  pw_light_sleep;     /* 0; gated (Task 6, Task 10) */
    uint16_t pw_deep_idle_min;   /* 60; 0 = never */
    uint8_t  pw_hibernate_h;     /* 2; 0 = never (the battery <= 10% trigger stays) */
    uint8_t  pw_hibernate_checkin_h; /* reserved (v18): uint8 hours can't hold 15-720 min; see below */
    /* v19 (Task 8, PT16) */
    uint16_t pw_hibernate_checkin_min; /* spec hibernate_checkin: 15; 0 = off; clamped by pr_hib_checkin_clamp_min to 0 or 15-720 */
} app_settings_t;

#define TZ_DEFAULT "PST8PDT,M3.2.0,M11.1.0"

extern app_settings_t g_settings;

void settings_init(void);         /* load from NVS or defaults */
void settings_save(void);
/* The Plan 2 power settings (pw_*, kbd_needs_light, kbd_off_cap) back to their defaults; the time zone stays. */
void settings_power_defaults(void);
void settings_reset_defaults(void);

/* Generic small NVS helpers used by other modules. */
bool nvs_get_blob_alloc(const char *ns, const char *key, void **out, size_t *len);
bool nvs_set_blob_ns(const char *ns, const char *key, const void *data, size_t len);
bool nvs_erase_key_ns(const char *ns, const char *key);
bool nvs_get_str_alloc(const char *ns, const char *key, char **out);
bool nvs_set_str_ns(const char *ns, const char *key, const char *val);

#ifdef __cplusplus
}
#endif
