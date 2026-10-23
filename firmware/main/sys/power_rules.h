#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "companion/cc_sessions.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure power rules (no ESP-IDF headers; also built by test/host). */

typedef struct { uint8_t mode, bright, r, g, b; } pr_led_t;   /* mode 0 = bind, 1 = user colour */
#define PR_LED_OFF ((pr_led_t){1, 0, 0, 0, 0})   /* mode 1, brightness 0, black */

/* HSV hue 25..55 deg, saturation >= 0.5, value >= 0.5: such a colour is never shown as the user's. */
bool pr_rgb_amber_like(uint32_t rgb);
/* background && live && CC_NEEDS_YOU */
bool pr_any_needs_you(const cc_session_t *s, int n);
/* needs_you && setting_on && result_age_ms < max_age_ms -> {1, b, 0xf4, 0xb5, 0x45} with
   b = max(user->bright, 30), then min(b, max(off_cap_pct, 30)) when screen_off, then min(b, 30)
   when deep_idle; else, in deep_idle, PR_LED_OFF; else *user */
pr_led_t pr_kbd_light(const pr_led_t *user, bool setting_on, bool needs_you,
                      int64_t result_age_ms, int64_t max_age_ms, bool screen_off, int off_cap_pct,
                      bool deep_idle);
/* 300 000, or max(300 000, (checkin_min + 1) * 60 000) in Radio off */
int64_t pr_light_max_age_ms(bool radio_off, int checkin_min);

/* ---- the ladder (Task 5) ---------------------------------------------- */

typedef enum { PS_ACTIVE, PS_DIM, PS_OFF, PS_RADIO_OFF, PS_DEEP_IDLE, PS_HIBERNATE } power_state_t;
/* also the power-log tags 0..5; check-ins log 0x80|state (0x83 Radio off, 0x84 Deep idle, 0x85 Hibernate) */
typedef struct {
    power_state_t state;
    int64_t now_ms, last_input_ms, off_since_ms;   /* monotonic (esp_timer), never wall clock */
    int dim_s, off_s, radio_off_min;
    int deep_idle_min, hibernate_h;                /* 0 = never */
    int64_t still_ms;                              /* how long the IMU has seen no motion; 0 = unknown or moving */
    int64_t still_hibernate_ms;                    /* Hibernate when still this long; 0 = off */
    int battery_pct;                               /* -1 = unknown */
    bool held, on_battery, sleep_on_usb, radio_off_enabled;
    bool charging, hibernate_ok;                   /* hibernate_ok = hibernate_supported() (false until Task 6) */
} pr_in_t;
#define PR_LOW_BATTERY_PCT 10
/* In order:
   held -> ACTIVE; on USB with !sleep_on_usb -> ACTIVE; idle < dim_s -> ACTIVE; idle < off_s -> DIM.
   From here idle >= off_s:
   on_battery && hibernate_ok && ((hibernate_h > 0 && idle >= hibernate_h h) ||
       (battery_pct >= 0 && battery_pct <= PR_LOW_BATTERY_PCT && !charging) ||
       (still_hibernate_ms > 0 && still_ms >= still_hibernate_ms && idle >= still_hibernate_ms)) -> HIBERNATE;
   on_battery && deep_idle_min > 0 && idle >= deep_idle_min min -> DEEP_IDLE;
   RADIO_OFF when already OFF/RADIO_OFF, on_battery, radio_off_enabled and now - off_since >= radio_off_min min;
   else OFF. Without on_battery, RADIO_OFF and DEEP_IDLE fall back to OFF and HIBERNATE is never chosen.
   HIBERNATE is terminal: the caller never feeds it back in (the device powers down). */
power_state_t pr_next_state(const pr_in_t *in);

typedef enum { PR_CARDS_LIVE, PR_CARDS_STALE, PR_CARDS_WAKING } pr_cards_t;
/* hibernate_boot && gen_now == 0 -> WAKING (no cards, "Waking: reconnecting to Mac");
   gen_now > gen_at_wake -> LIVE; needs_you_wake -> LIVE (its block is fresh); else STALE */
pr_cards_t pr_cards_view(bool hibernate_boot, uint32_t gen_now, uint32_t gen_at_wake, bool needs_you_wake);

#define PR_SWALLOW_MS 600       /* longer than the A164's 500 ms auto-repeat delay (keyboard.c) */
/* true when state_at_input != ACTIVE, or now - woke_at < PR_SWALLOW_MS, or now - last_swallowed < PR_SWALLOW_MS */
bool pr_swallow(power_state_t state_at_input, int64_t now_ms, int64_t woke_at_ms, int64_t last_swallowed_ms);

#define PR_IDS_MAX 32
typedef struct { char ids[PR_IDS_MAX][16]; int n; } pr_idset_t;
void pr_needs_you_ids(const cc_session_t *s, int n, pr_idset_t *out);   /* background, live, NEEDS_YOU */
bool pr_needs_you_new(const pr_idset_t *armed, const pr_idset_t *now);   /* an id in now not in armed */
int  pr_dim_brightness(int brightness);   /* max(5, brightness * 20 / 100) */

#ifdef __cplusplus
}
#endif
