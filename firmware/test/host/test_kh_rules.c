#include <string.h>
#include "unity.h"
#include "ssh/kh_rules.h"

void setUp(void) {}
void tearDown(void) {}

static const unsigned char A[32] = { 1, 2, 3 };
static const unsigned char B[32] = { 9, 9, 9 };

static void test_classify(void)
{
    TEST_ASSERT_EQUAL(KH_OK, kh_classify(KH_PIN_FOUND, A, 32, A, 32));
    TEST_ASSERT_EQUAL(KH_CHANGED, kh_classify(KH_PIN_FOUND, B, 32, A, 32));
    TEST_ASSERT_EQUAL(KH_CHANGED, kh_classify(KH_PIN_FOUND, A, 31, A, 32));   /* short pin */
    TEST_ASSERT_EQUAL(KH_CHANGED, kh_classify(KH_PIN_FOUND, NULL, 0, A, 32));
    TEST_ASSERT_EQUAL(KH_UNKNOWN, kh_classify(KH_PIN_NONE, NULL, 0, A, 32));
}

/* An NVS read error is never "unknown", so nothing re-pins over it. */
static void test_read_error_is_not_unknown(void)
{
    TEST_ASSERT_EQUAL(KH_PIN_ERROR, kh_classify(KH_PIN_READ_FAILED, NULL, 0, A, 32));
    TEST_ASSERT_EQUAL(KH_PIN_ERROR, kh_classify(KH_PIN_READ_FAILED, A, 32, A, 32));
    TEST_ASSERT_FALSE(kh_needs_answer(KH_PIN_ERROR));
    TEST_ASSERT_FALSE(kh_accept(KH_PIN_ERROR, true, true));
}

/* With no callback to ask (a stop cleared it), nothing new or changed is trusted. */
static void test_no_callback_rejects(void)
{
    TEST_ASSERT_FALSE(kh_accept(KH_UNKNOWN, false, true));
    TEST_ASSERT_FALSE(kh_accept(KH_CHANGED, false, true));
    TEST_ASSERT_TRUE(kh_accept(KH_OK, false, false));
}

static void test_accept(void)
{
    TEST_ASSERT_TRUE(kh_accept(KH_UNKNOWN, true, true));
    TEST_ASSERT_TRUE(kh_accept(KH_CHANGED, true, true));
    TEST_ASSERT_FALSE(kh_accept(KH_UNKNOWN, true, false));
    TEST_ASSERT_FALSE(kh_accept(KH_CHANGED, true, false));
    TEST_ASSERT_FALSE(kh_accept(KH_ERROR, true, true));
    TEST_ASSERT_TRUE(kh_needs_answer(KH_UNKNOWN));
    TEST_ASSERT_TRUE(kh_needs_answer(KH_CHANGED));
    TEST_ASSERT_FALSE(kh_needs_answer(KH_OK));
    TEST_ASSERT_FALSE(kh_needs_answer(KH_ERROR));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_classify);
    RUN_TEST(test_read_error_is_not_unknown);
    RUN_TEST(test_no_callback_rejects);
    RUN_TEST(test_accept);
    return UNITY_END();
}
