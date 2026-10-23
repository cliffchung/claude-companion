#include "unity.h"
#include <string.h>
#include "power_rules.h"

void setUp(void) {}
void tearDown(void) {}

#define AMBER(b) ((pr_led_t){1, (b), 0xf4, 0xb5, 0x45})

static void eq(pr_led_t want, pr_led_t got)
{
    TEST_ASSERT_EQUAL_UINT8(want.mode, got.mode);
    TEST_ASSERT_EQUAL_UINT8(want.bright, got.bright);
    TEST_ASSERT_EQUAL_UINT8(want.r, got.r);
    TEST_ASSERT_EQUAL_UINT8(want.g, got.g);
    TEST_ASSERT_EQUAL_UINT8(want.b, got.b);
}

void test_amber_like_colours(void)
{
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xF4B545));
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xFFD700));
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xFF8000));
    /* boundaries: hue 24/25/55/56 deg, saturation 0.49/0.5, value 0.49/0.5 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0xFF6100));   /* hue 22.8 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0xFF6A00));   /* hue 24.9 */
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xFF6B00));    /* hue 25.2 */
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xFFE900));    /* hue 54.8 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0xFFEA00));   /* hue 55.06 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0xFFFF00));   /* hue 60 */
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0xFEB57F));    /* sat exactly 0.5 (127/254), hue 25.5 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0xFEB580));   /* sat 126/254 = 0.496 */
    TEST_ASSERT_TRUE(pr_rgb_amber_like(0x805800));    /* value 128/255 >= 0.5 */
    TEST_ASSERT_FALSE(pr_rgb_amber_like(0x7F5700));   /* value 127/255 < 0.5 */
    uint32_t no[] = { 0x0080FF, 0xFFFFFF, 0xFF4040, 0x40FF40, 0xFF40FF, 0x00FFFF, 0x604818, 0x000000 };
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++) TEST_ASSERT_FALSE_MESSAGE(pr_rgb_amber_like(no[i]), "colour");
}

void test_light_needs_you_amber(void)
{
    pr_led_t u = { 0, 20, 0, 128, 255 };
    eq(AMBER(30), pr_kbd_light(&u, true, true, 1000, 300000, false, 100, false));
    u.bright = 0;
    eq(AMBER(30), pr_kbd_light(&u, true, true, 1000, 300000, false, 100, false));
    u.bright = 80;
    eq(AMBER(80), pr_kbd_light(&u, true, true, 1000, 300000, false, 100, false));
}

void test_light_user_when_clear(void)
{
    pr_led_t u = { 0, 20, 0, 128, 255 };
    eq(u, pr_kbd_light(&u, true, false, 1000, 300000, false, 100, false));
    eq(u, pr_kbd_light(&u, false, true, 1000, 300000, false, 100, false));
}

void test_light_stale_result_reverts(void)
{
    pr_led_t u = { 0, 20, 0, 128, 255 };
    eq(u, pr_kbd_light(&u, true, true, 300001, 300000, false, 100, false));
    eq(AMBER(30), pr_kbd_light(&u, true, true, 299999, 300000, false, 100, false));
}

void test_light_off_cap(void)
{
    pr_led_t u = { 1, 80, 0, 128, 255 };
    eq(AMBER(40), pr_kbd_light(&u, true, true, 0, 300000, true, 40, false));
    eq(AMBER(30), pr_kbd_light(&u, true, true, 0, 300000, true, 10, false));
    eq(AMBER(80), pr_kbd_light(&u, true, true, 0, 300000, false, 10, false));
}

void test_light_deep_idle(void)
{
    pr_led_t u = { 1, 80, 0, 128, 255 };
    eq(AMBER(30), pr_kbd_light(&u, true, true, 0, 300000, true, 100, true));
    u.bright = 0;
    eq(AMBER(30), pr_kbd_light(&u, true, true, 0, 300000, true, 100, true));
    u.bright = 80;
    eq(PR_LED_OFF, pr_kbd_light(&u, true, false, 0, 300000, true, 100, true));
    eq(PR_LED_OFF, pr_kbd_light(&u, true, true, 300000, 300000, true, 100, true));
    eq(PR_LED_OFF, pr_kbd_light(&u, false, true, 0, 300000, true, 100, true));
}

void test_light_age_limit_follows_checkin(void)
{
    TEST_ASSERT_EQUAL_INT64(300000, pr_light_max_age_ms(false, 5));
    TEST_ASSERT_EQUAL_INT64(360000, pr_light_max_age_ms(true, 5));
    TEST_ASSERT_EQUAL_INT64(1860000, pr_light_max_age_ms(true, 30));
}

void test_any_needs_you(void)
{
    cc_session_t s[3];
    memset(s, 0, sizeof(s));
    s[0].state = CC_NEEDS_YOU; s[0].background = false; s[0].live = true;     /* interactive */
    s[1].state = CC_NEEDS_YOU; s[1].background = true; s[1].live = false;     /* stopped */
    TEST_ASSERT_FALSE(pr_any_needs_you(s, 2));
    s[2].state = CC_NEEDS_YOU; s[2].background = true; s[2].live = true;
    TEST_ASSERT_TRUE(pr_any_needs_you(s, 3));
    TEST_ASSERT_FALSE(pr_any_needs_you(s, 0));
}

/* ---- the ladder (Task 5) ---------------------------------------------- */

#define NOW 100000000LL   /* far from 0, so "ago" never goes negative */
#define MIN_MS 60000LL
#define H_MS 3600000LL

/* The long-idle defaults: dim 30, off 120, radio off 20 min (disabled), deep idle 60 min,
   hibernate 2 h, on battery at 50%, not charging, hibernate_ok, nothing held. */
static pr_in_t base(power_state_t st, int64_t idle_ms)
{
    pr_in_t in;
    memset(&in, 0, sizeof(in));
    in.state = st;
    in.now_ms = NOW;
    in.last_input_ms = NOW - idle_ms;
    in.off_since_ms = NOW;
    in.dim_s = 30; in.off_s = 120; in.radio_off_min = 20;
    in.deep_idle_min = 60; in.hibernate_h = 2;
    in.still_ms = 0; in.still_hibernate_ms = 0;
    in.battery_pct = 50;
    in.held = false; in.on_battery = true; in.sleep_on_usb = true; in.radio_off_enabled = false;
    in.charging = false; in.hibernate_ok = true;
    return in;
}

static void next_is(power_state_t want, const pr_in_t *in)
{
    TEST_ASSERT_EQUAL_INT(want, pr_next_state(in));
}

void test_ladder_thresholds(void)
{
    pr_in_t in = base(PS_ACTIVE, 29000);
    next_is(PS_ACTIVE, &in);
    in = base(PS_ACTIVE, 30000);
    next_is(PS_DIM, &in);
    in = base(PS_DIM, 119999);
    next_is(PS_DIM, &in);
    in = base(PS_DIM, 120000);
    next_is(PS_OFF, &in);
}

void test_held_stays_active(void)
{
    pr_in_t in = base(PS_ACTIVE, 500000);
    in.held = true;
    next_is(PS_ACTIVE, &in);
    in.state = PS_DIM;
    next_is(PS_ACTIVE, &in);
}

void test_usb_without_sleep(void)
{
    pr_in_t in = base(PS_ACTIVE, 500000);
    in.on_battery = false; in.sleep_on_usb = false;
    next_is(PS_ACTIVE, &in);
    in.sleep_on_usb = true;
    next_is(PS_OFF, &in);
}

void test_radio_off_needs_battery(void)
{
    pr_in_t in = base(PS_OFF, 23 * MIN_MS);
    in.off_since_ms = NOW - 21 * MIN_MS;
    in.radio_off_enabled = true;
    in.on_battery = false;
    next_is(PS_OFF, &in);
    in.on_battery = true;
    next_is(PS_RADIO_OFF, &in);
    in.radio_off_enabled = false;
    next_is(PS_OFF, &in);
    in.radio_off_enabled = true;
    in.state = PS_RADIO_OFF; in.on_battery = false;
    next_is(PS_OFF, &in);
    /* not yet 20 min in Off */
    in.state = PS_OFF; in.on_battery = true; in.off_since_ms = NOW - 19 * MIN_MS;
    next_is(PS_OFF, &in);
}

void test_swallow_rules(void)
{
    int64_t never = -1000000;
    TEST_ASSERT_TRUE(pr_swallow(PS_DIM, NOW, never, never));
    TEST_ASSERT_TRUE(pr_swallow(PS_OFF, NOW, never, never));
    TEST_ASSERT_TRUE(pr_swallow(PS_ACTIVE, NOW + 100, NOW, never));
    TEST_ASSERT_FALSE(pr_swallow(PS_ACTIVE, NOW + 601, NOW, never));
}

void test_swallow_held_key_repeats(void)
{
    int64_t woke = 0, last = -1000000;
    for (int64_t t = 500; t <= 1500; t += 60) {
        bool drop = pr_swallow(PS_ACTIVE, t, woke, last);
        TEST_ASSERT_TRUE_MESSAGE(drop, "auto-repeat after the wake");
        if (drop) last = t;
    }
    TEST_ASSERT_FALSE(pr_swallow(PS_ACTIVE, 2200, woke, last));
}

static pr_idset_t ids(const char *a, const char *b)
{
    pr_idset_t s;
    memset(&s, 0, sizeof(s));
    if (a) strcpy(s.ids[s.n++], a);
    if (b) strcpy(s.ids[s.n++], b);
    return s;
}

void test_needs_you_new(void)
{
    pr_idset_t armed = ids("a", NULL), now = ids("a", NULL);
    TEST_ASSERT_FALSE(pr_needs_you_new(&armed, &now));
    now = ids("a", "b");
    TEST_ASSERT_TRUE(pr_needs_you_new(&armed, &now));
    armed = ids("a", "b"); now = ids("b", NULL);
    TEST_ASSERT_FALSE(pr_needs_you_new(&armed, &now));
    armed = ids(NULL, NULL); now = ids(NULL, NULL);
    TEST_ASSERT_FALSE(pr_needs_you_new(&armed, &now));

    /* pr_needs_you_ids keeps only live background sessions that need you */
    cc_session_t s[4];
    memset(s, 0, sizeof(s));
    strcpy(s[0].id, "aa"); s[0].state = CC_NEEDS_YOU; s[0].background = true; s[0].live = true;
    strcpy(s[1].id, ""); s[1].state = CC_NEEDS_YOU; s[1].background = false; s[1].live = true;
    strcpy(s[2].id, "cc"); s[2].state = CC_NEEDS_YOU; s[2].background = true; s[2].live = false;
    strcpy(s[3].id, "dd"); s[3].state = CC_WORKING; s[3].background = true; s[3].live = true;
    pr_idset_t got;
    pr_needs_you_ids(s, 4, &got);
    TEST_ASSERT_EQUAL_INT(1, got.n);
    TEST_ASSERT_EQUAL_STRING("aa", got.ids[0]);
}

void test_dim_brightness(void)
{
    TEST_ASSERT_EQUAL_INT(16, pr_dim_brightness(80));
    TEST_ASSERT_EQUAL_INT(5, pr_dim_brightness(10));
    TEST_ASSERT_EQUAL_INT(20, pr_dim_brightness(100));
}

void test_deep_idle_after_minutes(void)
{
    pr_in_t in = base(PS_OFF, 59 * MIN_MS);
    next_is(PS_OFF, &in);
    in = base(PS_OFF, 60 * MIN_MS);
    next_is(PS_DEEP_IDLE, &in);
    in = base(PS_OFF, 90 * MIN_MS);
    in.deep_idle_min = 0;
    next_is(PS_OFF, &in);
    in = base(PS_OFF, 60 * MIN_MS);
    in.radio_off_enabled = true; in.off_since_ms = NOW - 40 * MIN_MS;
    next_is(PS_DEEP_IDLE, &in);   /* Deep idle wins over Radio off */
}

void test_hibernate_after_hours(void)
{
    pr_in_t in = base(PS_DEEP_IDLE, 2 * H_MS);
    next_is(PS_HIBERNATE, &in);
    in = base(PS_DEEP_IDLE, 119 * MIN_MS);
    next_is(PS_DEEP_IDLE, &in);
    in = base(PS_DEEP_IDLE, 5 * H_MS);
    in.hibernate_ok = false;
    next_is(PS_DEEP_IDLE, &in);
    in = base(PS_DEEP_IDLE, 5 * H_MS);
    in.hibernate_h = 0;
    next_is(PS_DEEP_IDLE, &in);
}

void test_hibernate_low_battery(void)
{
    pr_in_t in = base(PS_OFF, 200000);
    in.battery_pct = 10;
    next_is(PS_HIBERNATE, &in);
    in.battery_pct = 11;
    next_is(PS_OFF, &in);
    in.battery_pct = 10; in.charging = true;
    next_is(PS_OFF, &in);
    in = base(PS_DIM, 60000);
    in.battery_pct = 10;
    next_is(PS_DIM, &in);   /* the low-battery trigger acts only from Off or deeper */
    in = base(PS_OFF, 200000);
    in.battery_pct = 10; in.hibernate_h = 0;
    next_is(PS_HIBERNATE, &in);
    in.hibernate_h = 2; in.hibernate_ok = false;
    next_is(PS_OFF, &in);
}

void test_long_idle_needs_battery(void)
{
    pr_in_t in = base(PS_OFF, 10 * H_MS);
    in.on_battery = false; in.sleep_on_usb = true; in.battery_pct = -1;
    next_is(PS_OFF, &in);
    in.battery_pct = 5;
    next_is(PS_OFF, &in);
    in.state = PS_DEEP_IDLE;
    next_is(PS_OFF, &in);
    in = base(PS_OFF, 200000);
    in.battery_pct = -1;
    next_is(PS_OFF, &in);   /* an unknown % never counts as low */
}

void test_long_idle_held(void)
{
    pr_in_t in = base(PS_OFF, 10 * H_MS);
    in.held = true; in.battery_pct = 5;
    next_is(PS_ACTIVE, &in);
    in.state = PS_DEEP_IDLE;
    next_is(PS_ACTIVE, &in);
}

void test_still_brings_hibernate_sooner(void)
{
    pr_in_t in = base(PS_OFF, 30 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS;
    in.still_ms = 30 * MIN_MS;
    next_is(PS_HIBERNATE, &in);
    in.still_ms = 29 * MIN_MS;
    next_is(PS_OFF, &in);
    in = base(PS_OFF, 29 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS; in.still_ms = 45 * MIN_MS;
    next_is(PS_OFF, &in);   /* an input 29 min ago */
    in = base(PS_OFF, 30 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS; in.still_ms = 0;
    next_is(PS_OFF, &in);   /* unknown */
    in = base(PS_OFF, 45 * MIN_MS);
    in.still_hibernate_ms = 0; in.still_ms = 45 * MIN_MS;
    next_is(PS_OFF, &in);   /* the trigger is off */
    in = base(PS_OFF, 30 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS; in.still_ms = 30 * MIN_MS; in.hibernate_h = 0;
    next_is(PS_HIBERNATE, &in);
    in = base(PS_DEEP_IDLE, 70 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS; in.still_ms = 40 * MIN_MS;
    next_is(PS_HIBERNATE, &in);
    in = base(PS_OFF, 45 * MIN_MS);
    in.still_hibernate_ms = 30 * MIN_MS; in.still_ms = 45 * MIN_MS;
    in.on_battery = false; in.sleep_on_usb = true;
    next_is(PS_OFF, &in);
    in.on_battery = true; in.hibernate_ok = false;
    next_is(PS_OFF, &in);
    in.hibernate_ok = true; in.held = true;
    next_is(PS_ACTIVE, &in);
}

void test_cards_view_after_hibernate(void)
{
    TEST_ASSERT_EQUAL_INT(PR_CARDS_WAKING, pr_cards_view(true, 0, 0, false));
    TEST_ASSERT_EQUAL_INT(PR_CARDS_LIVE, pr_cards_view(true, 1, 0, false));
    TEST_ASSERT_EQUAL_INT(PR_CARDS_STALE, pr_cards_view(false, 5, 5, false));
    TEST_ASSERT_EQUAL_INT(PR_CARDS_LIVE, pr_cards_view(false, 6, 5, false));
    TEST_ASSERT_EQUAL_INT(PR_CARDS_LIVE, pr_cards_view(false, 5, 5, true));
    TEST_ASSERT_EQUAL_INT(PR_CARDS_STALE, pr_cards_view(false, 0, 0, false));
}

/* ---- Task 8: reconnect backoff, Radio off, check-ins ------------------ */

void test_backoff(void)
{
    int want[] = { 1, 2, 4, 8, 16, 30, 30, 30 };
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_INT(want[i], pr_backoff_s(i));
    TEST_ASSERT_EQUAL_INT(30, pr_backoff_s(1000));
    TEST_ASSERT_EQUAL_INT(30, pr_backoff_s(2147483647));
    TEST_ASSERT_EQUAL_INT(1, pr_backoff_s(-1));   /* never below the first step */
}

void test_radio_off_rule(void)
{
    /* saving 20 mWh per period; the average falls from 300 to 84 mW */
    TEST_ASSERT_TRUE(pr_radio_off_worth_it(300, 60, 2.0f, 5, 12));
    TEST_ASSERT_FALSE(pr_radio_off_worth_it(300, 60, 25.0f, 5, 12));
    TEST_ASSERT_FALSE(pr_radio_off_worth_it(300, 60, 2.0f, 5, 31));
    TEST_ASSERT_FALSE(pr_radio_off_worth_it(300, 60, 2.0f, 5, 30));   /* "under 30 s" */
    /* the saving (0.83 mWh) passes the first condition; the average falls only 8.8% */
    TEST_ASSERT_FALSE(pr_radio_off_worth_it(100, 90, 0.1f, 5, 12));
    TEST_ASSERT_FALSE(pr_radio_off_worth_it(300, 60, 2.0f, 0, 12));   /* no period */
}

void test_checkin_needs_you_keeps_light(void)
{
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE_KEEP_RAIL, pr_hib_checkin_next(true, true, true));
    TEST_ASSERT_EQUAL_INT(PR_HC_STAY_DEEP_IDLE, pr_hib_checkin_next(true, true, false));
}

void test_checkin_nothing_rehibernates(void)
{
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE, pr_hib_checkin_next(true, false, true));
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE, pr_hib_checkin_next(true, false, false));
    /* a failed poll never lights a stale needs-you */
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE, pr_hib_checkin_next(false, true, true));
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE, pr_hib_checkin_next(false, false, false));
    TEST_ASSERT_EQUAL_INT(PR_HC_REHIBERNATE, pr_hib_checkin_next(false, true, false));
}

void test_checkin_clamp(void)
{
    int in[] = { 0, 1, 14, 15, 60, 720, 721, 5000, -5 };
    int want[] = { 0, 15, 15, 15, 60, 720, 720, 720, 0 };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++)
        TEST_ASSERT_EQUAL_INT(want[i], pr_hib_checkin_clamp_min(in[i]));
}

void test_deep_idle_backoff(void)
{
    TEST_ASSERT_EQUAL_INT(5, pr_checkin_period_min(PS_RADIO_OFF, 5, 0));
    TEST_ASSERT_EQUAL_INT(5, pr_checkin_period_min(PS_RADIO_OFF, 5, 9));   /* k is Deep idle's only */
    TEST_ASSERT_EQUAL_INT(15, pr_checkin_period_min(PS_DEEP_IDLE, 5, 0));
    TEST_ASSERT_EQUAL_INT(30, pr_checkin_period_min(PS_DEEP_IDLE, 5, 1));
    TEST_ASSERT_EQUAL_INT(30, pr_checkin_period_min(PS_DEEP_IDLE, 5, 7));
    TEST_ASSERT_EQUAL_INT(20, pr_checkin_period_min(PS_DEEP_IDLE, 20, 0));
    TEST_ASSERT_EQUAL_INT(30, pr_checkin_period_min(PS_DEEP_IDLE, 20, 1));
    TEST_ASSERT_EQUAL_INT(45, pr_checkin_period_min(PS_DEEP_IDLE, 45, 3));   /* never shorter than the user's */
    TEST_ASSERT_EQUAL_INT(0, pr_checkin_period_min(PS_OFF, 5, 0));
    TEST_ASSERT_EQUAL_INT(0, pr_checkin_period_min(PS_ACTIVE, 5, 0));
    TEST_ASSERT_EQUAL_INT(0, pr_checkin_period_min(PS_DIM, 5, 0));
    TEST_ASSERT_EQUAL_INT(0, pr_checkin_period_min(PS_HIBERNATE, 5, 0));
    /* a 30-minute Deep-idle period keeps the amber */
    TEST_ASSERT_EQUAL_INT64(1860000, pr_light_max_age_ms(true, 30));
}

void test_hib_checkin_hold(void)
{
    /* a needs-you check-in that stays in Deep idle: no hibernate by the hours rule */
    pr_in_t in = base(PS_DEEP_IDLE, 10 * H_MS);
    in.hib_checkin_hold = true;
    next_is(PS_DEEP_IDLE, &in);
    in.battery_pct = 10;
    next_is(PS_HIBERNATE, &in);   /* except the battery <= 10% trigger */
    in.battery_pct = 50; in.hib_checkin_hold = false;
    next_is(PS_HIBERNATE, &in);
    in = base(PS_OFF, 10 * H_MS);
    in.hib_checkin_hold = true; in.deep_idle_min = 0;
    next_is(PS_OFF, &in);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_amber_like_colours);
    RUN_TEST(test_light_needs_you_amber);
    RUN_TEST(test_light_user_when_clear);
    RUN_TEST(test_light_stale_result_reverts);
    RUN_TEST(test_light_off_cap);
    RUN_TEST(test_light_deep_idle);
    RUN_TEST(test_light_age_limit_follows_checkin);
    RUN_TEST(test_any_needs_you);
    RUN_TEST(test_ladder_thresholds);
    RUN_TEST(test_held_stays_active);
    RUN_TEST(test_usb_without_sleep);
    RUN_TEST(test_radio_off_needs_battery);
    RUN_TEST(test_swallow_rules);
    RUN_TEST(test_swallow_held_key_repeats);
    RUN_TEST(test_needs_you_new);
    RUN_TEST(test_dim_brightness);
    RUN_TEST(test_deep_idle_after_minutes);
    RUN_TEST(test_hibernate_after_hours);
    RUN_TEST(test_hibernate_low_battery);
    RUN_TEST(test_long_idle_needs_battery);
    RUN_TEST(test_long_idle_held);
    RUN_TEST(test_still_brings_hibernate_sooner);
    RUN_TEST(test_cards_view_after_hibernate);
    RUN_TEST(test_backoff);
    RUN_TEST(test_radio_off_rule);
    RUN_TEST(test_checkin_needs_you_keeps_light);
    RUN_TEST(test_checkin_nothing_rehibernates);
    RUN_TEST(test_checkin_clamp);
    RUN_TEST(test_deep_idle_backoff);
    RUN_TEST(test_hib_checkin_hold);
    return UNITY_END();
}
