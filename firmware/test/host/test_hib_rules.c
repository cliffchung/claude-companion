#include "unity.h"
#include "hib_rules.h"

void setUp(void) {}
void tearDown(void) {}

static hib_wake_in_t in0(void)
{
    hib_wake_in_t in = { 0 };
    return in;
}

static void test_wakeup_cause_timer(void)
{
    hib_wake_in_t in = in0();
    in.wakeup_timer = true;
    TEST_ASSERT_EQUAL(HIB_CAUSE_TIMER, hib_cause(&in));
}

/* this P4: a deep-sleep timer wake arrives as an HP watchdog reset with no wakeup cause */
static void test_wdt_reset_after_timer_is_timer(void)
{
    hib_wake_in_t in = in0();
    in.reset_deepsleep_or_wdt = true;
    in.wake_after_s = 30;
    in.asleep_s = 33;
    TEST_ASSERT_EQUAL(HIB_CAUSE_TIMER, hib_cause(&in));
}

static void test_wdt_reset_long_after_timer_is_reset(void)
{
    hib_wake_in_t in = in0();
    in.reset_deepsleep_or_wdt = true;
    in.wake_after_s = 30;
    in.asleep_s = 300;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
}

static void test_wdt_reset_without_timer_is_reset(void)
{
    hib_wake_in_t in = in0();
    in.reset_deepsleep_or_wdt = true;
    in.asleep_s = 5;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
}

static void test_rtc_flag_wins(void)
{
    hib_wake_in_t in = in0();
    in.reset_poweron = true;
    in.rtc_flags = 0x10;   /* TF */
    in.wake_after_s = 60;
    in.asleep_s = -5;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RTC, hib_cause(&in));
    in.rtc_flags = 0x08;   /* AF */
    TEST_ASSERT_EQUAL(HIB_CAUSE_RTC, hib_cause(&in));
}

static void test_poweron_is_button(void)
{
    hib_wake_in_t in = in0();
    in.reset_poweron = true;
    in.rtc_flags = 0x21;   /* UF and bit 0: not a wake */
    in.asleep_s = -1792787409;   /* the clock starts at 0 after a power-off */
    TEST_ASSERT_EQUAL(HIB_CAUSE_BUTTON, hib_cause(&in));
}

static void test_probe_verdicts(void)
{
    TEST_ASSERT_EQUAL(HIB_PROBE_NO_SAMPLES, hib_probe_verdict(0, true, 0));
    TEST_ASSERT_EQUAL(HIB_PROBE_USB, hib_probe_verdict(3, true, 12));
    TEST_ASSERT_EQUAL(HIB_PROBE_USB, hib_probe_verdict(3, false, -4));
    TEST_ASSERT_EQUAL(HIB_PROBE_OK, hib_probe_verdict(3, false, 6));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wakeup_cause_timer);
    RUN_TEST(test_wdt_reset_after_timer_is_timer);
    RUN_TEST(test_wdt_reset_long_after_timer_is_reset);
    RUN_TEST(test_wdt_reset_without_timer_is_reset);
    RUN_TEST(test_rtc_flag_wins);
    RUN_TEST(test_poweron_is_button);
    RUN_TEST(test_probe_verdicts);
    return UNITY_END();
}
