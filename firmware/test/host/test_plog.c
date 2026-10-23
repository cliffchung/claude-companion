#include "unity.h"
#include "sys/plog.h"

void setUp(void) {}
void tearDown(void) {}

static plog_sample_t smp(uint32_t t, uint16_t mw)
{
    plog_sample_t s = { .t_s = t, .mv = 7400, .ma = 100, .mw = mw, .tag = 0, .flags = 2 };
    return s;
}

static void test_ring_wraps_oldest_first(void)
{
    plog_sample_t buf[4];
    plog_ring_t r;
    plog_init(&r, buf, 4);
    for (uint32_t t = 1; t <= 6; t++) { plog_sample_t s = smp(t, 100); plog_push(&r, &s); }
    TEST_ASSERT_EQUAL_UINT32(4, r.count);
    plog_sample_t o;
    TEST_ASSERT_TRUE(plog_get(&r, 0, &o));
    TEST_ASSERT_EQUAL_UINT32(3, o.t_s);
    TEST_ASSERT_TRUE(plog_get(&r, 3, &o));
    TEST_ASSERT_EQUAL_UINT32(6, o.t_s);
    TEST_ASSERT_FALSE(plog_get(&r, 4, &o));
}

static void test_sum_avg_and_energy(void)
{
    plog_sum_t s = {0};
    for (uint32_t i = 0; i < 3; i++) { plog_sample_t x = smp(10 + i, 1000 * (i + 1)); plog_sum_add(&s, &x); }
    TEST_ASSERT_EQUAL_UINT32(2000, plog_sum_avg_mw(&s));
    TEST_ASSERT_EQUAL_UINT16(1000, s.min_mw);
    TEST_ASSERT_EQUAL_UINT16(3000, s.max_mw);
    TEST_ASSERT_INT_WITHIN(1, 1667, (int)plog_sum_mwh_x1000(&s));
}

static void test_sum_empty(void)
{
    plog_sum_t s = {0};
    TEST_ASSERT_EQUAL_UINT32(0, plog_sum_avg_mw(&s));
    TEST_ASSERT_EQUAL_UINT32(0, plog_sum_mwh_x1000(&s));
}

static void test_mw_from_volts_and_amps(void)
{
    TEST_ASSERT_EQUAL_UINT16(3139, plog_mw(7620, 412));    /* 3139.4 */
    TEST_ASSERT_EQUAL_UINT16(3141, plog_mw(7623, -412));   /* sign ignored, rounded: 3140.7 */
    TEST_ASSERT_EQUAL_UINT16(8, plog_mw(7590, 1));         /* below the INA's 25 mW step */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, plog_mw(65535, 32767));
}

static void test_step_verdict(void)
{
    plog_sum_t empty = {0}, some = {0};
    plog_sample_t x = smp(1, 500);
    plog_sum_add(&some, &x);
    TEST_ASSERT_EQUAL(PLOG_STEP_OK, plog_step_verdict(&some, false));
    TEST_ASSERT_EQUAL(PLOG_STEP_NO_SAMPLES, plog_step_verdict(&empty, false));
    TEST_ASSERT_EQUAL(PLOG_STEP_USB, plog_step_verdict(&some, true));
    TEST_ASSERT_EQUAL(PLOG_STEP_USB, plog_step_verdict(&empty, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_wraps_oldest_first);
    RUN_TEST(test_sum_avg_and_energy);
    RUN_TEST(test_sum_empty);
    RUN_TEST(test_mw_from_volts_and_amps);
    RUN_TEST(test_step_verdict);
    return UNITY_END();
}
