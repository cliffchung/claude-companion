#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "companion/cc_sessions.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure power rules (no ESP-IDF headers; also built by test/host). Task 5 adds the state rules. */

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

#ifdef __cplusplus
}
#endif
