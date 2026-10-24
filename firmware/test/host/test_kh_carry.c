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

/* ---- kh_carry_action: the whole decision connect_once makes (M10) ---- */

/* A LAN address that has its own pin: the normal check, whatever else is true. */
static void test_action_lan_pin_found_is_normal(void)
{
    const kh_result_t r[] = { KH_OK, KH_CHANGED, KH_ERROR, KH_PIN_ERROR };
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(KHA_NORMAL, kh_carry_action(r[i], false, false, KHS_EQUAL));
        TEST_ASSERT_EQUAL(KHA_NORMAL, kh_carry_action(r[i], false, true, KHS_NONE));
        TEST_ASSERT_EQUAL(KHA_NORMAL, kh_carry_action(r[i], true, false, KHS_DIFFERENT));
    }
}

/* First contact on a tailnet address pins as in Plan 2; so does any address after the user's forget. */
static void test_action_pin_new(void)
{
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, true, false, KHS_NONE));
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, true, false, KHS_DIFFERENT));
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, false, true, KHS_NONE));
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, false, true, KHS_DIFFERENT));
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, false, true, KHS_ERROR));
}

/* A LAN address with no pin and no forget: only an identical key carries over. */
static void test_action_lan_unknown(void)
{
    TEST_ASSERT_EQUAL(KHA_PIN_SILENT, kh_carry_action(KH_UNKNOWN, false, false, KHS_EQUAL));
    TEST_ASSERT_EQUAL(KHA_REFUSE_CHANGED, kh_carry_action(KH_UNKNOWN, false, false, KHS_DIFFERENT));
    TEST_ASSERT_EQUAL(KHA_REFUSE_UNKNOWN, kh_carry_action(KH_UNKNOWN, false, false, KHS_NONE));
    TEST_ASSERT_EQUAL(KHA_PIN_ERROR, kh_carry_action(KH_UNKNOWN, false, false, KHS_ERROR));
}

/* Nothing but PIN_NEW and PIN_SILENT ever pins, and PIN_SILENT needs an identical key. */
static void test_action_never_trusts_more(void)
{
    const kh_result_t r[] = { KH_OK, KH_UNKNOWN, KH_CHANGED, KH_ERROR, KH_PIN_ERROR };
    const khs_t c[] = { KHS_NONE, KHS_EQUAL, KHS_DIFFERENT, KHS_ERROR };
    for (unsigned i = 0; i < 5; i++)
        for (int t = 0; t < 2; t++)
            for (int f = 0; f < 2; f++)
                for (unsigned k = 0; k < 4; k++) {
                    kha_t a = kh_carry_action(r[i], t, f, c[k]);
                    if (a == KHA_PIN_SILENT) TEST_ASSERT_TRUE(r[i] == KH_UNKNOWN && !t && !f && c[k] == KHS_EQUAL);
                    if (a == KHA_PIN_NEW) TEST_ASSERT_TRUE(r[i] == KH_UNKNOWN && (t || f));
                }
}

/* ---- kh_forget_applies: a forget takes any key, a trust from the screen only the confirmed one ---- */

static void test_forget_applies_any_key(void)
{
    TEST_ASSERT_TRUE(kh_forget_applies(true, NULL, "SHA256:abc"));
    TEST_ASSERT_TRUE(kh_forget_applies(true, "", "SHA256:abc"));
}

static void test_trust_applies_only_to_the_confirmed_key(void)
{
    TEST_ASSERT_TRUE(kh_forget_applies(true, "SHA256:abc", "SHA256:abc"));
    TEST_ASSERT_FALSE(kh_forget_applies(true, "SHA256:abc", "SHA256:abd"));
    TEST_ASSERT_FALSE(kh_forget_applies(true, "SHA256:abc", "SHA256:ab"));
    TEST_ASSERT_FALSE(kh_forget_applies(true, "SHA256:abc", ""));
    TEST_ASSERT_FALSE(kh_forget_applies(true, "SHA256:abc", NULL));
}

static void test_nothing_pending_never_applies(void)
{
    TEST_ASSERT_FALSE(kh_forget_applies(false, NULL, "SHA256:abc"));
    TEST_ASSERT_FALSE(kh_forget_applies(false, "SHA256:abc", "SHA256:abc"));
}

/* A trust that doesn't apply leaves an unknown LAN key refused as unknown, and a changed one as changed. */
static void test_trust_mismatch_keeps_the_refusal(void)
{
    bool f = kh_forget_applies(true, "SHA256:abc", "SHA256:xyz");
    TEST_ASSERT_EQUAL(KHA_REFUSE_UNKNOWN, kh_carry_action(KH_UNKNOWN, false, f, KHS_NONE));
    TEST_ASSERT_EQUAL(KHA_REFUSE_CHANGED, kh_carry_action(KH_UNKNOWN, false, f, KHS_DIFFERENT));
    TEST_ASSERT_EQUAL(KHA_NORMAL, kh_carry_action(KH_CHANGED, false, f, KHS_NONE));
    f = kh_forget_applies(true, "SHA256:abc", "SHA256:abc");
    TEST_ASSERT_EQUAL(KHA_PIN_NEW, kh_carry_action(KH_UNKNOWN, false, f, KHS_NONE));
}

/* ---- kh_trust_allowed: a trust only where nothing is pinned or carried, for this entry, in time (I2) ---- */

static void test_trust_allowed_with_nothing_pinned(void)
{
    const kh_pin_t none[3] = { KH_PIN_NONE, KH_PIN_NONE, KH_PIN_NONE };
    TEST_ASSERT_TRUE(kh_trust_allowed(none, 3, false, true, false));
    TEST_ASSERT_TRUE(kh_trust_allowed(NULL, 0, false, true, false));
}

static void test_trust_refused_over_any_pin(void)
{
    kh_pin_t p[3] = { KH_PIN_NONE, KH_PIN_NONE, KH_PIN_NONE };
    for (int i = 0; i < 3; i++) {
        p[i] = KH_PIN_FOUND;
        TEST_ASSERT_FALSE(kh_trust_allowed(p, 3, false, true, false));
        p[i] = KH_PIN_READ_FAILED;   /* an unreadable pin is never re-pinned over */
        TEST_ASSERT_FALSE(kh_trust_allowed(p, 3, false, true, false));
        p[i] = KH_PIN_NONE;
    }
}

static void test_trust_refused_over_carry_entry_change_or_expiry(void)
{
    const kh_pin_t none[2] = { KH_PIN_NONE, KH_PIN_NONE };
    TEST_ASSERT_FALSE(kh_trust_allowed(none, 2, true, true, false));    /* a carry source: a key unlike it is changed */
    TEST_ASSERT_FALSE(kh_trust_allowed(none, 2, false, false, false));  /* another Host, or its address edited */
    TEST_ASSERT_FALSE(kh_trust_allowed(none, 2, false, true, true));    /* past its deadline */
    TEST_ASSERT_FALSE(kh_trust_allowed(NULL, 2, false, true, false));
}

/* A trust that is no longer allowed doesn't apply: the connect refuses as it would with none (I2's scenario: a
   carry source appeared meanwhile, so the confirmed key is refused as changed). */
static void test_disallowed_trust_keeps_the_refusal(void)
{
    const kh_pin_t none[1] = { KH_PIN_NONE };
    bool allowed = kh_trust_allowed(none, 1, true, true, false);
    bool f = kh_forget_applies(allowed, "SHA256:abc", "SHA256:abc");
    TEST_ASSERT_FALSE(f);
    TEST_ASSERT_EQUAL(KHA_REFUSE_CHANGED, kh_carry_action(KH_UNKNOWN, false, f, KHS_DIFFERENT));
    allowed = kh_trust_allowed(none, 1, false, true, true);
    f = kh_forget_applies(allowed, "SHA256:abc", "SHA256:abc");
    TEST_ASSERT_EQUAL(KHA_REFUSE_UNKNOWN, kh_carry_action(KH_UNKNOWN, false, f, KHS_NONE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_action_lan_pin_found_is_normal);
    RUN_TEST(test_action_pin_new);
    RUN_TEST(test_action_lan_unknown);
    RUN_TEST(test_action_never_trusts_more);
    RUN_TEST(test_lan_pinned_uses_normal_check);
    RUN_TEST(test_tailnet_pin_equal_pins_silently);
    RUN_TEST(test_tailnet_pin_differs_refuses);
    RUN_TEST(test_no_tailnet_pin_is_unknown);
    RUN_TEST(test_forget_applies_any_key);
    RUN_TEST(test_trust_applies_only_to_the_confirmed_key);
    RUN_TEST(test_nothing_pending_never_applies);
    RUN_TEST(test_trust_mismatch_keeps_the_refusal);
    RUN_TEST(test_trust_allowed_with_nothing_pinned);
    RUN_TEST(test_trust_refused_over_any_pin);
    RUN_TEST(test_trust_refused_over_carry_entry_change_or_expiry);
    RUN_TEST(test_disallowed_trust_keeps_the_refusal);
    return UNITY_END();
}
