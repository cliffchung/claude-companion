#include "power_rules.h"
#include <string.h>

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

/* ---- the ladder ------------------------------------------------------- */

power_state_t pr_next_state(const pr_in_t *in)
{
    if (in->held) return PS_ACTIVE;
    if (!in->on_battery && !in->sleep_on_usb) return PS_ACTIVE;
    int64_t idle = in->now_ms - in->last_input_ms;
    if (idle < (int64_t)in->dim_s * 1000) return PS_ACTIVE;
    if (idle < (int64_t)in->off_s * 1000) return PS_DIM;
    if (!in->on_battery) return PS_OFF;   /* no Radio off, Deep idle or Hibernate without a battery */
    if (in->hibernate_ok) {
        bool hours = in->hibernate_h > 0 && idle >= (int64_t)in->hibernate_h * 3600000;
        bool low = in->battery_pct >= 0 && in->battery_pct <= PR_LOW_BATTERY_PCT && !in->charging;
        bool still = in->still_hibernate_ms > 0 && in->still_ms >= in->still_hibernate_ms &&
                     idle >= in->still_hibernate_ms;
        if (hours || low || still) return PS_HIBERNATE;
    }
    if (in->deep_idle_min > 0 && idle >= (int64_t)in->deep_idle_min * 60000) return PS_DEEP_IDLE;
    if ((in->state == PS_OFF || in->state == PS_RADIO_OFF) && in->radio_off_enabled &&
        in->now_ms - in->off_since_ms >= (int64_t)in->radio_off_min * 60000)
        return PS_RADIO_OFF;
    return PS_OFF;
}

pr_cards_t pr_cards_view(bool hibernate_boot, uint32_t gen_now, uint32_t gen_at_wake, bool needs_you_wake)
{
    if (hibernate_boot && gen_now == 0) return PR_CARDS_WAKING;
    if (gen_now > gen_at_wake) return PR_CARDS_LIVE;
    if (needs_you_wake) return PR_CARDS_LIVE;
    return PR_CARDS_STALE;
}

bool pr_swallow(power_state_t state_at_input, int64_t now_ms, int64_t woke_at_ms, int64_t last_swallowed_ms)
{
    if (state_at_input != PS_ACTIVE) return true;
    if (now_ms - woke_at_ms < PR_SWALLOW_MS) return true;
    return now_ms - last_swallowed_ms < PR_SWALLOW_MS;
}

void pr_needs_you_ids(const cc_session_t *s, int n, pr_idset_t *out)
{
    out->n = 0;
    for (int i = 0; i < n && out->n < PR_IDS_MAX; i++) {
        if (!cc_session_needs_you(&s[i]) || !s[i].id[0]) continue;
        strncpy(out->ids[out->n], s[i].id, sizeof(out->ids[0]) - 1);
        out->ids[out->n][sizeof(out->ids[0]) - 1] = 0;
        out->n++;
    }
}

static bool idset_has(const pr_idset_t *set, const char *id)
{
    for (int i = 0; i < set->n; i++)
        if (strcmp(set->ids[i], id) == 0) return true;
    return false;
}

bool pr_needs_you_new(const pr_idset_t *armed, const pr_idset_t *now)
{
    for (int i = 0; i < now->n; i++)
        if (!idset_has(armed, now->ids[i])) return true;
    return false;
}

int pr_dim_brightness(int brightness)
{
    int b = brightness * 20 / 100;
    return b > 5 ? b : 5;
}
