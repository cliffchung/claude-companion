#include "power_rules.h"

#define LIGHT_MIN_BRIGHT 30
#define LIGHT_BASE_AGE_MS 300000

bool pr_rgb_amber_like(uint32_t rgb)
{
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    /* hue 25..55 degrees means red is the largest channel and green sits above blue */
    if (r < g || g < b) return false;
    int d = r - b;
    if (d == 0 || r * 2 < 255) return false;       /* grey, or value < 0.5 */
    if (d * 2 < r) return false;                   /* saturation < 0.5 */
    int h60 = 60 * (g - b);                        /* hue = h60 / d */
    return h60 >= 25 * d && h60 <= 55 * d;
}

bool pr_any_needs_you(const cc_session_t *s, int n)
{
    for (int i = 0; i < n; i++)
        if (cc_session_needs_you(&s[i])) return true;
    return false;
}

pr_led_t pr_kbd_light(const pr_led_t *user, bool setting_on, bool needs_you,
                      int64_t result_age_ms, int64_t max_age_ms, bool screen_off, int off_cap_pct,
                      bool deep_idle)
{
    if (needs_you && setting_on && result_age_ms < max_age_ms) {
        int b = user->bright > LIGHT_MIN_BRIGHT ? user->bright : LIGHT_MIN_BRIGHT;
        if (screen_off) {
            int cap = off_cap_pct > LIGHT_MIN_BRIGHT ? off_cap_pct : LIGHT_MIN_BRIGHT;
            if (b > cap) b = cap;
        }
        if (deep_idle && b > LIGHT_MIN_BRIGHT) b = LIGHT_MIN_BRIGHT;
        return (pr_led_t){ 1, (uint8_t)b, 0xf4, 0xb5, 0x45 };
    }
    if (deep_idle) return PR_LED_OFF;
    return *user;
}

int64_t pr_light_max_age_ms(bool radio_off, int checkin_min)
{
    if (!radio_off) return LIGHT_BASE_AGE_MS;
    int64_t m = ((int64_t)checkin_min + 1) * 60000;
    return m > LIGHT_BASE_AGE_MS ? m : LIGHT_BASE_AGE_MS;
}
