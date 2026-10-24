#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "vitals.h"

void setUp(void) {}
void tearDown(void) {}

static void test_curve_points(void)
{
    TEST_ASSERT_EQUAL_INT(100, vitals_percent_from_ocv(8.40f));
    TEST_ASSERT_EQUAL_INT(50, vitals_percent_from_ocv(7.58f));
    TEST_ASSERT_EQUAL_INT(0, vitals_percent_from_ocv(6.60f));
    TEST_ASSERT_INT_WITHIN(1, 42, vitals_percent_from_ocv(7.50f));
    TEST_ASSERT_EQUAL_INT(100, vitals_percent_from_ocv(9.00f));
    TEST_ASSERT_EQUAL_INT(0, vitals_percent_from_ocv(5.00f));
}

static void test_curve_monotonic(void)
{
    int prev = 0;
    for (int i = 0; i <= 200; i++) {
        int p = vitals_percent_from_ocv(6.5f + i * 0.01f);
        TEST_ASSERT_TRUE(p >= prev);
        prev = p;
    }
}

static void test_load_compensation(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.55f, vitals_ocv(7.40f, 1.0f, 0.15f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.025f, vitals_ocv(8.10f, -0.5f, 0.15f));
}

static void test_ema_tau(void)
{
    vitals_batt_t b = { 0 };
    vitals_batt_update(&b, 7.40f, 0.0f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.40f, b.ema_v);
    for (int i = 0; i < 30; i++) vitals_batt_update(&b, 8.00f, 0.0f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 7.40f + 0.6f * 0.63f, b.ema_v);
}

static void test_no_pack_is_unknown(void)
{
    vitals_batt_t b = { 0 };
    TEST_ASSERT_EQUAL_INT(-1, vitals_batt_update(&b, 0.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, vitals_batt_update(&b, 5.0f, 0.0f, 1.0f));
    TEST_ASSERT_FALSE(b.init);
    vitals_batt_update(&b, 7.58f, 0.0f, 1.0f);
    TEST_ASSERT_EQUAL_INT(-1, vitals_batt_update(&b, 5.0f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.58f, b.ema_v);
}

static void test_battery_unknown_not_low(void)
{
    TEST_ASSERT_EQUAL_INT(VB_NONE, vitals_bat_level(-1, false));
    TEST_ASSERT_EQUAL_INT(VB_LOW, vitals_bat_level(15, false));
    TEST_ASSERT_EQUAL_INT(VB_NORMAL, vitals_bat_level(15, true));
    TEST_ASSERT_EQUAL_INT(VB_NORMAL, vitals_bat_level(16, false));
}

static void test_wifi_bars(void)
{
    TEST_ASSERT_EQUAL_INT(0, vitals_wifi_bars(false, -40));
    TEST_ASSERT_EQUAL_INT(4, vitals_wifi_bars(true, -55));
    TEST_ASSERT_EQUAL_INT(3, vitals_wifi_bars(true, -56));
    TEST_ASSERT_EQUAL_INT(3, vitals_wifi_bars(true, -67));
    TEST_ASSERT_EQUAL_INT(2, vitals_wifi_bars(true, -75));
    TEST_ASSERT_EQUAL_INT(1, vitals_wifi_bars(true, -90));
}

static void test_tz_valid(void)
{
    int n = 0;
    for (const vitals_tz_t *p = vitals_tz_presets; p->label; p++, n++) TEST_ASSERT_TRUE(vitals_tz_valid(p->rule));
    TEST_ASSERT_EQUAL_INT(10, n);
    TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", vitals_tz_presets[0].rule);
    TEST_ASSERT_FALSE(vitals_tz_valid(""));
    TEST_ASSERT_FALSE(vitals_tz_valid(NULL));
    TEST_ASSERT_FALSE(vitals_tz_valid("PST8PDT;rm"));
    TEST_ASSERT_FALSE(vitals_tz_valid("x y"));
    TEST_ASSERT_FALSE(vitals_tz_valid("8PDT"));
    TEST_ASSERT_FALSE(vitals_tz_valid("PST8PDT,M3.2.0,M11.1.0,M3.2.0,M11.1.0,M3.2.0,M11"));   /* 48 */
    TEST_ASSERT_TRUE(vitals_tz_valid("<+0330>-3:30"));
}

static void test_tz_dst_end_2026(void)
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    struct tm tm;
    time_t t = 1793523599;
    localtime_r(&t, &tm);
    TEST_ASSERT_EQUAL_INT(1, tm.tm_hour);
    TEST_ASSERT_EQUAL_INT(59, tm.tm_min);
    TEST_ASSERT_EQUAL_INT(59, tm.tm_sec);
    TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
    t = 1793523600;
    localtime_r(&t, &tm);
    TEST_ASSERT_EQUAL_INT(1, tm.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, tm.tm_min);
    TEST_ASSERT_EQUAL_INT(0, tm.tm_sec);
    TEST_ASSERT_EQUAL_INT(0, tm.tm_isdst);
}

/* power source (PT23): the INA226 alone; ma + = discharging. The cases marked "pin low" are where the old
   rule read CHG_STAT low as charging (it reads 0 whether or not the pack charges). */
static void test_src_charging(void)
{
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(7800, 5, PWR_UNKNOWN));        /* pin low: ignored */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(8000, -400, PWR_USB));    /* current in */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(7600, -460, PWR_UNKNOWN));/* boot on USB: at once */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(7600, -6, PWR_UNKNOWN));  /* just under -5 mA */
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(7600, -5, PWR_UNKNOWN));       /* -5 mA is "small" */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(8400, -30, PWR_USB_STANDBY)); /* charging wins at 8.4 V */
}

static void test_charging_is_charge_current(void)
{
    /* "charging" (the bolt, the low-battery rule) is a charge current under -5 mA, nothing else */
    TEST_ASSERT_TRUE(vitals_charging(-6));
    TEST_ASSERT_TRUE(vitals_charging(-460));
    TEST_ASSERT_FALSE(vitals_charging(-5));
    TEST_ASSERT_FALSE(vitals_charging(0));
    TEST_ASSERT_FALSE(vitals_charging(300));
}

static void test_src_idle_battery_reads_battery(void)
{
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 30, PWR_UNKNOWN));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 30, PWR_USB));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 300, PWR_BATTERY));
    /* a full pack on USB is standby even under load (known limit: also a full pack just unplugged) */
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8350, 300, PWR_USB));
}

static void test_src_small_current_keeps_state(void)
{
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(7600, 0, PWR_UNKNOWN));     /* no evidence yet */
    TEST_ASSERT_EQUAL_INT(PWR_USB, vitals_power_src(7600, 3, PWR_USB));
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(7600, 3, PWR_USB_STANDBY));
    TEST_ASSERT_EQUAL_INT(PWR_USB, vitals_power_src(7600, 0, PWR_USB_CHARGING));    /* charge ended, still USB */
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8380, 0, PWR_USB));     /* full on USB */
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8380, -5, PWR_USB_STANDBY));
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8380, -3, PWR_UNKNOWN)); /* boot with a full pack */
}

static void test_src_no_pack_is_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(0, 200, PWR_BATTERY));
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(5000, 200, PWR_USB));
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(5400, -300, PWR_USB_CHARGING));
}

static void test_src_hysteresis(void)
{
    /* on battery, a dip to 12 mA stays battery; from USB the same 12 mA stays USB */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 12, PWR_BATTERY));
    TEST_ASSERT_EQUAL_INT(PWR_USB, vitals_power_src(7600, 12, PWR_USB));
    /* low current is not evidence of USB: -5..20 mA on battery stays battery; charge current ends it */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 2, PWR_BATTERY));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 0, PWR_BATTERY));    /* pin low: ignored */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, -5, PWR_BATTERY));
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(7600, -10, PWR_BATTERY));
    /* charging: any current under -5 mA is charging, from any state */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(8300, -30, PWR_USB_CHARGING));
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(7600, -30, PWR_USB));
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8380, 0, PWR_USB_CHARGING));
}

static power_src_t feed(vitals_src_t *st, int n, int mv, int ma)
{
    power_src_t r = st->src;
    for (int i = 0; i < n; i++) r = vitals_src_step(st, mv, ma);
    return r;
}

static void test_usb_with_pack_supplementing(void)
{
    /* on USB, the pack supplies short bursts under load between charge pauses: never battery */
    vitals_src_t st = { 0 };
    for (int round = 0; round < 20; round++) {
        TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&st, 1, 7900, 0));
        for (int i = 0; i < 9; i++) TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, vitals_src_step(&st, 7800, 150 + i));
        TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, vitals_src_step(&st, 7800, 3));    /* burst ends */
        for (int i = 0; i < 9; i++) TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, vitals_src_step(&st, 7800, 400));
        TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, vitals_src_step(&st, 7800, -120)); /* charging again */
    }
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, st.src);
    /* a long positive stretch from a USB start is not enough before N samples */
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&st, VITALS_BATT_SAMPLES - 1, 7800, 300));
}

static void test_full_pack_on_usb_under_load_is_not_battery(void)
{
    vitals_src_t st = { 0 };
    feed(&st, 3, 8300, 0);
    for (int i = 0; i < 40; i++) TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, vitals_src_step(&st, 8300, 300));
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, st.src);
    /* companion: the pack at 7.6 V, 300 mA, 10 samples: battery */
    vitals_src_t b = { 0 };
    feed(&b, 3, 7600, 0);
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&b, VITALS_BATT_SAMPLES - 1, 7600, 300));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&b, 7600, 300));
    /* sagging below 8.25 V while on battery is fine; reaching 8.25 V ends the run */
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_src_step(&b, 8260, 300));
}

static void test_boot_on_battery_is_unknown_until_decided(void)
{
    vitals_src_t st = { 0 };
    for (int i = 0; i < VITALS_BATT_SAMPLES - 1; i++) TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_src_step(&st, 7600, 30));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7600, 30));
    /* a counted run keeps the previous source, not USB */
    vitals_src_t f = { .src = PWR_USB_STANDBY };
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_src_step(&f, 7600, 300));
}

static void test_settings_clamps(void)
{
    TEST_ASSERT_EQUAL_INT(0, vitals_clamp_or_zero(0, 15, 720));
    TEST_ASSERT_EQUAL_INT(0, vitals_clamp_or_zero(7, 15, 720));
    TEST_ASSERT_EQUAL_INT(15, vitals_clamp_or_zero(8, 15, 720));
    TEST_ASSERT_EQUAL_INT(15, vitals_clamp_or_zero(14, 15, 720));
    TEST_ASSERT_EQUAL_INT(60, vitals_clamp_or_zero(60, 15, 720));
    TEST_ASSERT_EQUAL_INT(720, vitals_clamp_or_zero(9999, 15, 720));
    TEST_ASSERT_EQUAL_INT(1, vitals_clamp_or_zero(1, 1, 48));
    TEST_ASSERT_EQUAL_INT(48, vitals_clamp_or_zero(49, 1, 48));
    uint16_t d = 5, o = 5;
    vitals_clamp_dim_off(&d, &o);
    TEST_ASSERT_EQUAL_INT(10, d); TEST_ASSERT_EQUAL_INT(20, o);
    d = 30; o = 120;
    vitals_clamp_dim_off(&d, &o);
    TEST_ASSERT_EQUAL_INT(30, d); TEST_ASSERT_EQUAL_INT(120, o);
    d = 5000; o = 9000;
    vitals_clamp_dim_off(&d, &o);
    TEST_ASSERT_EQUAL_INT(600, d); TEST_ASSERT_EQUAL_INT(3600, o);
    d = 100; o = 50;
    vitals_clamp_dim_off(&d, &o);
    TEST_ASSERT_EQUAL_INT(110, o);
}

static void test_src_battery_sustained(void)
{
    vitals_src_t st = { 0 };
    feed(&st, 1, 7800, -100);                               /* charging on USB */
    feed(&st, 3, 7800, 0);                                  /* then quiet on USB */
    TEST_ASSERT_EQUAL_INT(PWR_USB, st.src);
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&st, VITALS_BATT_SAMPLES - 1, 7800, 30));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7800, 30));   /* idle 30 mA, N-th sample */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7800, 8));    /* dip: hysteresis */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7800, 0));    /* quiet is not USB */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, feed(&st, 30, 7800, 12));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7800, 0));    /* pin low: ignored */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_src_step(&st, 7800, -10));   /* charge current: at once */
    feed(&st, VITALS_BATT_SAMPLES, 7800, 30);
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, st.src);
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_src_step(&st, 7800, -200));  /* current in: at once */
    feed(&st, VITALS_BATT_SAMPLES, 7800, 30);
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_src_step(&st, 0, 30));      /* pack gone */
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&st, 5, 0, 30));
}

static void test_src_battery_needs_consecutive_samples(void)
{
    vitals_src_t st = { 0 };
    feed(&st, 1, 7800, -300);
    feed(&st, 9, 7800, 25);
    TEST_ASSERT_EQUAL_INT(PWR_USB, vitals_src_step(&st, 7800, 10));    /* 10 mA breaks the run: still USB */
    TEST_ASSERT_NOT_EQUAL(PWR_BATTERY, feed(&st, VITALS_BATT_SAMPLES - 1, 7800, 25));   /* the run restarted */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_src_step(&st, 7800, 25));
}

static void test_src_boundaries(void)
{
    /* chgfix M5: each threshold from both sides (PT23) */
    TEST_ASSERT_EQUAL_INT(PWR_USB_CHARGING, vitals_power_src(7600, -6, PWR_UNKNOWN));
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(7600, -5, PWR_UNKNOWN));    /* -5 holds */
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(7600, 19, PWR_UNKNOWN));    /* 19 holds */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(7600, 20, PWR_UNKNOWN));    /* 20 is a candidate */
    TEST_ASSERT_EQUAL_INT(PWR_USB_STANDBY, vitals_power_src(8250, 40, PWR_UNKNOWN));
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(8249, 40, PWR_UNKNOWN));
    TEST_ASSERT_EQUAL_INT(PWR_UNKNOWN, vitals_power_src(5499, 40, PWR_BATTERY));    /* no pack, whatever before */
    TEST_ASSERT_EQUAL_INT(PWR_BATTERY, vitals_power_src(5500, 40, PWR_UNKNOWN));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_curve_points);
    RUN_TEST(test_curve_monotonic);
    RUN_TEST(test_load_compensation);
    RUN_TEST(test_ema_tau);
    RUN_TEST(test_no_pack_is_unknown);
    RUN_TEST(test_battery_unknown_not_low);
    RUN_TEST(test_wifi_bars);
    RUN_TEST(test_tz_valid);
    RUN_TEST(test_tz_dst_end_2026);
    RUN_TEST(test_src_charging);
    RUN_TEST(test_charging_is_charge_current);
    RUN_TEST(test_src_idle_battery_reads_battery);
    RUN_TEST(test_src_small_current_keeps_state);
    RUN_TEST(test_src_no_pack_is_unknown);
    RUN_TEST(test_src_hysteresis);
    RUN_TEST(test_usb_with_pack_supplementing);
    RUN_TEST(test_src_battery_sustained);
    RUN_TEST(test_src_battery_needs_consecutive_samples);
    RUN_TEST(test_src_boundaries);
    RUN_TEST(test_full_pack_on_usb_under_load_is_not_battery);
    RUN_TEST(test_boot_on_battery_is_unknown_until_decided);
    RUN_TEST(test_settings_clamps);
    return UNITY_END();
}
