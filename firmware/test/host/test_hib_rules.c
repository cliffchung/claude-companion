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

/* the RX8130 switched it on: a flag, and the time asleep matches its timer */
static void test_rtc_flag_in_window_is_rtc(void)
{
    hib_wake_in_t in = in0();
    in.reset_poweron = true;
    in.rtc_flags = 0x10;   /* TF */
    in.wake_after_s = 60;
    in.asleep_s = 61;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RTC, hib_cause(&in));
    in.rtc_flags = 0x08;   /* AF */
    TEST_ASSERT_EQUAL(HIB_CAUSE_RTC, hib_cause(&in));
}

/* the timer fired long ago without switching it on; a press did */
static void test_stale_rtc_flag_on_power_on_is_button(void)
{
    hib_wake_in_t in = in0();
    in.reset_poweron = true;
    in.rtc_flags = 0x10;
    in.wake_after_s = 60;
    in.asleep_s = 3 * 3600;
    TEST_ASSERT_EQUAL(HIB_CAUSE_BUTTON, hib_cause(&in));
    in.asleep_s = -1792787409;   /* no valid clock: the flag alone proves nothing */
    TEST_ASSERT_EQUAL(HIB_CAUSE_BUTTON, hib_cause(&in));
}

/* M1: a crash on the way down, right after a 1-2 s timer was armed, is not a timer wake */
static void test_short_timer_window_has_a_floor(void)
{
    hib_wake_in_t in = in0();
    in.reset_deepsleep_or_wdt = true;
    in.wake_after_s = 1;
    in.asleep_s = 0;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
    in.asleep_s = 3;
    TEST_ASSERT_EQUAL(HIB_CAUSE_TIMER, hib_cause(&in));
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

/* found on hardware: a power-on by the side button after a PWROFF power-off arrives as an HP watchdog reset
   (hw reason 7), like a deep-sleep wake, not as ESP_RST_POWERON */
static void test_wdt_reset_after_power_off_is_button(void)
{
    hib_wake_in_t in = in0();
    in.was_poweroff = true;
    in.reset_deepsleep_or_wdt = true;
    in.rtc_flags = 0x10;     /* the 60 s timer fired 2 h earlier */
    in.wake_after_s = 60;
    in.asleep_s = 7735;
    TEST_ASSERT_EQUAL(HIB_CAUSE_BUTTON, hib_cause(&in));
    in.asleep_s = 61;        /* had the RTC switched it on, it would be in the window */
    TEST_ASSERT_EQUAL(HIB_CAUSE_RTC, hib_cause(&in));
}


/* PT27: after a deep-sleep record, a reset-type boot before the armed timer is the side button (found on
   hardware: a press 76 s into a 900 s timer arrived as hw reason 7, ESP_RST_WDT) */
static void test_early_reset_after_deep_sleep_is_button(void)
{
    hib_wake_in_t in = in0();
    in.was_deepsleep = true;
    in.reset_deepsleep_or_wdt = true;
    in.wake_after_s = 900;
    in.asleep_s = 76;
    TEST_ASSERT_EQUAL(HIB_CAUSE_BUTTON, hib_cause(&in));
    in.asleep_s = 899;    /* inside the timer's window: the timer */
    TEST_ASSERT_EQUAL(HIB_CAUSE_TIMER, hib_cause(&in));
    in.asleep_s = 1000;   /* after the window: not before the timer, still a reset */
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
    in.asleep_s = -5;     /* no valid clock: proves nothing */
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
}

static void test_early_crash_after_deep_sleep_is_reset(void)
{
    /* M4 stands: a panic or brownout on the way down is not a wake */
    hib_wake_in_t in = in0();
    in.was_deepsleep = true;
    in.reset_crash = true;
    in.wake_after_s = 900;
    in.asleep_s = 1;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
}

static void test_early_reset_without_deep_sleep_record_is_reset(void)
{
    hib_wake_in_t in = in0();
    in.reset_deepsleep_or_wdt = true;
    in.wake_after_s = 900;
    in.asleep_s = 76;
    TEST_ASSERT_EQUAL(HIB_CAUSE_RESET, hib_cause(&in));
}

static void test_wakeup_timer_beats_early(void)
{
    hib_wake_in_t in = in0();
    in.was_deepsleep = true;
    in.wakeup_timer = true;
    in.wake_after_s = 900;
    in.asleep_s = 10;
    TEST_ASSERT_EQUAL(HIB_CAUSE_TIMER, hib_cause(&in));
}

/* M9: a button never boots dark; a reset is a normal boot; a timer follows the record */
static void test_boot_kind_from_cause(void)
{
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, hib_boot_kind(POWER_BOOT_HIB_CHECKIN, HIB_CAUSE_BUTTON));
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, hib_boot_kind(POWER_BOOT_HIB_POWER_PROBE, HIB_CAUSE_BUTTON));
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, hib_boot_kind(POWER_BOOT_HIB_PROBE, HIB_CAUSE_BUTTON));
    TEST_ASSERT_EQUAL(POWER_BOOT_NORMAL, hib_boot_kind(POWER_BOOT_HIB_CHECKIN, HIB_CAUSE_RESET));
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_CHECKIN, hib_boot_kind(POWER_BOOT_HIB_CHECKIN, HIB_CAUSE_TIMER));
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_POWER_PROBE, hib_boot_kind(POWER_BOOT_HIB_POWER_PROBE, HIB_CAUSE_TIMER));
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, hib_boot_kind(POWER_BOOT_HIB_WAKE, HIB_CAUSE_RTC));
}

/* M4: every deep sleep has a timer */
static void test_sleep_plan_never_without_timer(void)
{
    hib_plan_t p = hib_sleep_plan(0, POWER_BOOT_HIB_WAKE, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(HIB_POWER_PROBE_S, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_POWER_PROBE, p.next);
    p = hib_sleep_plan(0, POWER_BOOT_HIB_WAKE, 40, 0);   /* `power hib test-wake 40` */
    TEST_ASSERT_EQUAL_UINT32(40, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, p.next);
    p = hib_sleep_plan(900, POWER_BOOT_HIB_CHECKIN, 40, 0);   /* an armed timer wins over test-wake */
    TEST_ASSERT_EQUAL_UINT32(900, p.timer_s);
    p = hib_sleep_plan(900, POWER_BOOT_HIB_CHECKIN, 0, 30);   /* the backstop shortens it, same kind */
    TEST_ASSERT_EQUAL_UINT32(30, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_CHECKIN, p.next);
    p = hib_sleep_plan(20, POWER_BOOT_HIB_CHECKIN, 0, 30);    /* never lengthens */
    TEST_ASSERT_EQUAL_UINT32(20, p.timer_s);
    p = hib_sleep_plan(0, POWER_BOOT_HIB_WAKE, 0, 30);
    TEST_ASSERT_EQUAL_UINT32(30, p.timer_s);
    for (uint32_t w = 0; w < 3; w++)
        for (uint32_t t = 0; t < 3; t++)
            for (uint32_t b = 0; b < 3; b++)
                TEST_ASSERT_TRUE(hib_sleep_plan(w * 60, POWER_BOOT_HIB_CHECKIN, t * 60, b * 60).timer_s > 0);
}

/* PT26: the ladder's hibernate is always a deep sleep: a check-in, or the 2 h power probe */
static void test_ladder_plan(void)
{
    hib_plan_t p = hib_ladder_plan(false, 900, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(900, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_CHECKIN, p.next);
    p = hib_ladder_plan(true, 900, 0, 0, 0);    /* <= 10% */
    TEST_ASSERT_EQUAL_UINT32(HIB_POWER_PROBE_S, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_POWER_PROBE, p.next);
    p = hib_ladder_plan(false, 0, 0, 0, 0);     /* check-in Off */
    TEST_ASSERT_EQUAL_UINT32(HIB_POWER_PROBE_S, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_POWER_PROBE, p.next);
    p = hib_ladder_plan(true, 900, 0, 0, 30);   /* the RAM probe override */
    TEST_ASSERT_EQUAL_UINT32(30, p.timer_s);
    p = hib_ladder_plan(false, 900, 2, 0, 0);   /* I3: two failures before */
    TEST_ASSERT_EQUAL_UINT32(3600, p.timer_s);
    p = hib_ladder_plan(true, 900, 0, 45, 0);   /* `power hib test-wake 45` */
    TEST_ASSERT_EQUAL_UINT32(45, p.timer_s);
    TEST_ASSERT_EQUAL(POWER_BOOT_HIB_WAKE, p.next);
}

/* I3 / PT28: 15 -> 30 -> 60 -> 120 min (cap); a success resets (fails = 0) */
static void test_checkin_backoff(void)
{
    TEST_ASSERT_EQUAL_UINT32(900, hib_checkin_backoff_s(900, 0));
    TEST_ASSERT_EQUAL_UINT32(1800, hib_checkin_backoff_s(900, 1));
    TEST_ASSERT_EQUAL_UINT32(3600, hib_checkin_backoff_s(900, 2));
    TEST_ASSERT_EQUAL_UINT32(7200, hib_checkin_backoff_s(900, 3));
    TEST_ASSERT_EQUAL_UINT32(7200, hib_checkin_backoff_s(900, 4));
    TEST_ASSERT_EQUAL_UINT32(7200, hib_checkin_backoff_s(900, 255));
    TEST_ASSERT_EQUAL_UINT32(7200, hib_checkin_backoff_s(3600, 1));   /* capped */
    TEST_ASSERT_EQUAL_UINT32(43200, hib_checkin_backoff_s(43200, 3)); /* a period over the cap never shortens */
    TEST_ASSERT_EQUAL_UINT32(0, hib_checkin_backoff_s(0, 2));         /* Off stays Off */
    TEST_ASSERT_EQUAL_UINT32(900, hib_checkin_backoff_s(900, -1));
}

static void test_checkin_fails_next(void)
{
    TEST_ASSERT_EQUAL(0, hib_checkin_fails_next(3, true));
    TEST_ASSERT_EQUAL(1, hib_checkin_fails_next(0, false));
    TEST_ASSERT_EQUAL(4, hib_checkin_fails_next(3, false));
    TEST_ASSERT_EQUAL(255, hib_checkin_fails_next(255, false));   /* saturates in the record's byte */
}

/* PT26: a power probe wake reads the source only; USB (charging, standby or no pack) boots, else sleeps */
static void test_power_probe(void)
{
    TEST_ASSERT_EQUAL(HIB_PP_BOOT, hib_power_probe(true, 7600, -460));   /* charging */
    TEST_ASSERT_EQUAL(HIB_PP_BOOT, hib_power_probe(true, 8300, 3));      /* USB standby, full pack */
    TEST_ASSERT_EQUAL(HIB_PP_BOOT, hib_power_probe(true, 5000, 0));      /* no pack: something powers it */
    TEST_ASSERT_EQUAL(HIB_PP_SLEEP, hib_power_probe(true, 7200, 40));    /* battery */
    TEST_ASSERT_EQUAL(HIB_PP_SLEEP, hib_power_probe(true, 7200, 3));     /* no evidence: stay asleep */
    TEST_ASSERT_EQUAL(HIB_PP_SLEEP, hib_power_probe(true, 7200, -5));    /* -5 is not charging */
    TEST_ASSERT_EQUAL(HIB_PP_BOOT, hib_power_probe(true, 7200, -6));
    TEST_ASSERT_EQUAL(HIB_PP_BOOT, hib_power_probe(true, 8250, 40));
    TEST_ASSERT_EQUAL(HIB_PP_SLEEP, hib_power_probe(true, 8249, 40));
    TEST_ASSERT_EQUAL(HIB_PP_SLEEP, hib_power_probe(false, 0, 0));       /* no reading: the button still wakes it */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wakeup_cause_timer);
    RUN_TEST(test_wdt_reset_after_timer_is_timer);
    RUN_TEST(test_wdt_reset_long_after_timer_is_reset);
    RUN_TEST(test_wdt_reset_without_timer_is_reset);
    RUN_TEST(test_rtc_flag_in_window_is_rtc);
    RUN_TEST(test_stale_rtc_flag_on_power_on_is_button);
    RUN_TEST(test_short_timer_window_has_a_floor);
    RUN_TEST(test_poweron_is_button);
    RUN_TEST(test_wdt_reset_after_power_off_is_button);
    RUN_TEST(test_probe_verdicts);
    RUN_TEST(test_early_reset_after_deep_sleep_is_button);
    RUN_TEST(test_early_crash_after_deep_sleep_is_reset);
    RUN_TEST(test_early_reset_without_deep_sleep_record_is_reset);
    RUN_TEST(test_wakeup_timer_beats_early);
    RUN_TEST(test_boot_kind_from_cause);
    RUN_TEST(test_sleep_plan_never_without_timer);
    RUN_TEST(test_ladder_plan);
    RUN_TEST(test_checkin_backoff);
    RUN_TEST(test_checkin_fails_next);
    RUN_TEST(test_power_probe);
    return UNITY_END();
}
