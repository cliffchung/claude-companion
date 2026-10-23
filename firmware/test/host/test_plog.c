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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_wraps_oldest_first);
    RUN_TEST(test_sum_avg_and_energy);
    RUN_TEST(test_sum_empty);
    return UNITY_END();
}
