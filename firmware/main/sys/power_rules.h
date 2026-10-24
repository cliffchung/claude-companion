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
/* PT24: the needs-you amber's brightness while the screen is dark (Off, Radio off, Deep idle, Hibernate),
   "Needs-you light when dark" in Settings -> Power. The A164 scales each channel as c * bright / 100, so at 1%
   amber is 2,1,0 (orange); 2% (4,3,1) is the lowest step that keeps its hue. */
#define PR_DARK_LIGHT_LOWEST 2
#define PR_DARK_LIGHT_LOW 5
#define PR_DARK_LIGHT_MEDIUM 15
#define PR_DARK_LIGHT_DEFAULT PR_DARK_LIGHT_LOW
/* 2..100 */
int pr_dark_light_clamp(int pct);
/* needs_you && setting_on && result_age_ms < max_age_ms -> {1, b, 0xf4, 0xb5, 0x45} with
   b = pr_dark_light_clamp(dark_pct) when dark or deep_idle, else max(user->bright, 30) (screen on);
   else, in deep_idle, PR_LED_OFF; else *user. The A164 has no breathe mode (PT24): the light is steady. */
pr_led_t pr_kbd_light(const pr_led_t *user, bool setting_on, bool needs_you,
                      int64_t result_age_ms, int64_t max_age_ms, bool dark, int dark_pct,
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
    int battery_pct;                               /* -1 = unknown */
    bool held, on_battery, sleep_on_usb, radio_off_enabled;
    bool charging, hibernate_ok;                   /* hibernate_supported() and not blocked by a failed try (M5) */
    bool bench_held;                               /* the bench's hold (part of held): never cut short (I1) */
    bool hib_checkin_hold;                         /* a Hibernate check-in found needs-you and stays awake (PT16) */
} pr_in_t;
#define PR_LOW_BATTERY_PCT 10
/* In order:
   held -> ACTIVE, except (I1, PT28) on_battery && hibernate_ok && the battery <= 10% && !bench_held &&
   idle >= off_s -> HIBERNATE (a take-over or SSH terminal left open never drains the pack flat; someone typing
   is never cut off); on USB with !sleep_on_usb -> ACTIVE; idle < dim_s -> ACTIVE; idle < off_s -> DIM.
   From here idle >= off_s:
   on_battery && hibernate_ok && ((hibernate_h > 0 && idle >= hibernate_h h) ||
       (battery_pct >= 0 && battery_pct <= PR_LOW_BATTERY_PCT && !charging)) -> HIBERNATE;
   on_battery && deep_idle_min > 0 && idle >= deep_idle_min min -> DEEP_IDLE;
   RADIO_OFF when already OFF/RADIO_OFF, on_battery, radio_off_enabled and now - off_since >= radio_off_min min;
   else OFF. Without on_battery, RADIO_OFF and DEEP_IDLE fall back to OFF and HIBERNATE is never chosen.
   hib_checkin_hold drops the hours trigger (never the battery <= 10% one).
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

/* ---- reconnect, Radio off and check-ins (Task 8) ---------------------- */

/* Wi-Fi background reconnect wait: 1, 2, 4, 8, 16, then 30 s for every later attempt (no cap on attempts) */
int  pr_backoff_s(int attempt);
/* The spec's rule for shipping Radio off on: with period = checkin_min min,
   (a - b) * period > c, the average with toggling (b + c / period) is at least 20% under a,
   and checkin_s < 30. a, b in mW; c in mWh per check-in. */
bool pr_radio_off_worth_it(float a_mw, float b_mw, float c_mwh, int checkin_min, int checkin_s);
/* Minutes to the next check-in: Radio off -> checkin_min; Deep idle -> max(checkin_min, 15) for the first
   check-in after entering it (deep_checkins_done == 0), then max(checkin_min, 30); any other state 0 (none). */
int  pr_checkin_period_min(power_state_t st, int checkin_min, int deep_checkins_done);

/* PT16: what a Hibernate check-in does next */
typedef enum { PR_HC_REHIBERNATE, PR_HC_REHIBERNATE_KEEP_RAIL, PR_HC_STAY_DEEP_IDLE,
               PR_HC_LOW   /* the battery <= 10% trigger: deep sleep with the light off and the power probe (PT26) */
} pr_hc_t;
/* !poll_ok -> REHIBERNATE (light off; recorded as hib-failed); !needs_you -> REHIBERNATE (PR_LED_OFF);
   needs_you && kbd_rail_survives -> REHIBERNATE_KEEP_RAIL (dim amber held through deep sleep, PT24);
   needs_you && !kbd_rail_survives -> STAY_DEEP_IDLE (radio parked, dim amber until input or nothing needs you) */
pr_hc_t pr_hib_checkin_next(bool poll_ok, bool needs_you, bool kbd_rail_survives);
/* on_battery && 0 <= pct <= PR_LOW_BATTERY_PCT && !charging (an unknown pct is never low) */
bool pr_battery_low(bool on_battery, int pct, bool charging);
/* The check-in boot's whole decision: low_battery -> LOW, else pr_hib_checkin_next (C1) */
pr_hc_t pr_hib_checkin_decide(bool low_battery, bool poll_ok, bool needs_you, bool kbd_rail_survives);
/* The check-in boot's early gate (N1), on the power source once it has left UNKNOWN (or timed out):
   UNKNOWN -> GIVE_UP (no blind re-arm: the ladder takes over, which treats unknown as not battery);
   USB -> WAKE (I2, PT28: boot fully and stay up under the normal ladder; Hibernate is for battery only);
   BATTERY with pr_battery_low -> LOW; else GO. USB and charging read as USB at once. */
typedef enum { PR_SRC_UNKNOWN, PR_SRC_USB, PR_SRC_BATTERY } pr_src_t;
typedef enum { PR_HG_GO, PR_HG_LOW, PR_HG_GIVE_UP, PR_HG_WAKE } pr_hg_t;
pr_hg_t pr_hib_checkin_gate(pr_src_t src, int pct, bool charging);
/* B1: after the gate gave up on UNKNOWN at armed_at_ms: CANCEL when the source settled or there was input
   after arming (last_input_ms > armed_at_ms); FIRE (the low-battery hibernate: deep sleep with the power probe,
   PT26) once it has stayed
   unknown for PR_UNKNOWN_BACKSTOP_MS; else WAIT. */
#define PR_UNKNOWN_BACKSTOP_MS (30 * 60 * 1000LL)
typedef enum { PR_UB_WAIT, PR_UB_FIRE, PR_UB_CANCEL } pr_ub_t;
pr_ub_t pr_unknown_backstop(pr_src_t src, int64_t armed_at_ms, int64_t now_ms, int64_t last_input_ms);
/* 0 (or less) -> 0 (Off); 1..14 -> 15; 721.. -> 720 */
int  pr_hib_checkin_clamp_min(int min);

/* M5: after a failed hibernate (blocked), the next try waits PR_HIB_RETRY_MS from blocked_at_ms */
#define PR_HIB_RETRY_MS (5 * 60 * 1000LL)
bool pr_hib_allowed(bool blocked, int64_t blocked_at_ms, int64_t now_ms);

/* The A164's light registers 0x60..0x67: two LEDs, each B, G, R, unused (P121; seen right on the device). */
void pr_a164_pack(uint8_t out[8], uint8_t r, uint8_t g, uint8_t b);
void pr_a164_unpack(const uint8_t in[3], uint8_t *r, uint8_t *g, uint8_t *b);
/* P123: the settings byte read from a blob of old_version: <= 19 held kbd_off_cap -> the default; else clamped */
int  pr_dark_light_load(uint32_t old_version, int stored);

#ifdef __cplusplus
}
#endif
