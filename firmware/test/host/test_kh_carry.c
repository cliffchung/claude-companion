#include "unity.h"
#include "companion/kh_carry.h"

void setUp(void) {}
void tearDown(void) {}

/* A LAN pin exists: the caller uses the normal check, whatever the tailnet pin says. */
static void test_lan_pinned_uses_normal_check(void)
{
    TEST_ASSERT_EQUAL(KHC_NORMAL, kh_carry_decide(true, true, true));
    TEST_ASSERT_EQUAL(KHC_NORMAL, kh_carry_decide(true, true, false));
    TEST_ASSERT_EQUAL(KHC_NORMAL, kh_carry_decide(true, false, false));
}

/* No LAN pin, and the key equals the same entry's tailnet pin: pinned without asking. */
static void test_tailnet_pin_equal_pins_silently(void)
{
    TEST_ASSERT_EQUAL(KHC_PIN_SILENT, kh_carry_decide(false, true, true));
}

/* No LAN pin, and the key differs from the tailnet pin: refused as a changed key. */
static void test_tailnet_pin_differs_refuses(void)
{
    TEST_ASSERT_EQUAL(KHC_REFUSE, kh_carry_decide(false, true, false));
}

/* No pin anywhere: unknown, never trusted on first use by the link. */
static void test_no_tailnet_pin_is_unknown(void)
{
    TEST_ASSERT_EQUAL(KHC_UNKNOWN, kh_carry_decide(false, false, false));
    TEST_ASSERT_EQUAL(KHC_UNKNOWN, kh_carry_decide(false, false, true));   /* "equal" to nothing means nothing */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lan_pinned_uses_normal_check);
    RUN_TEST(test_tailnet_pin_equal_pins_silently);
    RUN_TEST(test_tailnet_pin_differs_refuses);
    RUN_TEST(test_no_tailnet_pin_is_unknown);
    return UNITY_END();
}
