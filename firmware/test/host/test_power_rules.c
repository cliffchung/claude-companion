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
    return UNITY_END();
}
