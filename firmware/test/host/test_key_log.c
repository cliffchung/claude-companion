#include <string.h>
#include "unity.h"
#include "input/key_log.h"

void setUp(void) {}
void tearDown(void) {}

static const char *fmt(key_ev_type_t type, uint32_t cp, special_key_t key, uint8_t mods, bool swallowed)
{
    static char out[KEY_LOG_LINE_MAX];
    key_event_t e = { .type = type, .cp = cp, .key = key, .mods = mods };
    key_log_format(&e, swallowed, out, sizeof(out));
    return out;
}

/* No secrets in the logs: a typed character never appears unless a modifier makes it a shortcut */
static void test_plain_chars_masked(void)
{
    for (uint32_t cp = 0x20; cp <= 0x7e; cp++) {
        const char *l = fmt(KEY_EV_CHAR, cp, SK_NONE, 0, false);
        TEST_ASSERT_EQUAL_STRING("key: type=char cp='*' key=none mods=none", l);
        l = fmt(KEY_EV_CHAR, cp, SK_NONE, MOD_SHIFT, false);
        TEST_ASSERT_EQUAL_STRING("key: type=char cp='*' key=none mods=shift", l);
    }
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='*' key=none mods=none", fmt(KEY_EV_CHAR, 0xe9, SK_NONE, 0, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='*' key=none mods=none", fmt(KEY_EV_CHAR, 0x0d, SK_NONE, 0, false));
}

static void test_modified_chars_kept(void)
{
    TEST_ASSERT_EQUAL_STRING("key: type=char cp=']' key=none mods=ctrl", fmt(KEY_EV_CHAR, ']', SK_NONE, MOD_CTRL, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='m' key=none mods=ctrl", fmt(KEY_EV_CHAR, 'm', SK_NONE, MOD_CTRL, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='P' key=none mods=ctrl,shift", fmt(KEY_EV_CHAR, 'P', SK_NONE, MOD_CTRL | MOD_SHIFT, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='p' key=none mods=meta", fmt(KEY_EV_CHAR, 'p', SK_NONE, MOD_META, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='x' key=none mods=alt", fmt(KEY_EV_CHAR, 'x', SK_NONE, MOD_ALT, false));
    TEST_ASSERT_EQUAL_STRING("key: type=char cp=0x1d key=none mods=ctrl", fmt(KEY_EV_CHAR, 0x1d, SK_NONE, MOD_CTRL, false));
}

static void test_special_keys_named(void)
{
    TEST_ASSERT_EQUAL_STRING("key: type=special cp=0x0 key=enter mods=none", fmt(KEY_EV_SPECIAL, 0, SK_ENTER, 0, false));
    TEST_ASSERT_EQUAL_STRING("key: type=special cp=0x0 key=up mods=shift", fmt(KEY_EV_SPECIAL, 0, SK_UP, MOD_SHIFT, false));
    TEST_ASSERT_EQUAL_STRING("key: type=special cp=0x0 key=tab mods=ctrl", fmt(KEY_EV_SPECIAL, 0, SK_TAB, MOD_CTRL, false));
    TEST_ASSERT_EQUAL_STRING("key: type=special cp=0x0 key=ime-kana mods=none", fmt(KEY_EV_SPECIAL, 0, SK_IME_KANA, 0, false));
    TEST_ASSERT_EQUAL_STRING("key: type=special cp=0x0 key=? mods=none", fmt(KEY_EV_SPECIAL, 0, (special_key_t)99, 0, false));
}

static void test_swallowed_suffix(void)
{
    TEST_ASSERT_EQUAL_STRING("key: type=char cp='*' key=none mods=none (wake key, swallowed)", fmt(KEY_EV_CHAR, 'a', SK_NONE, 0, true));
}

static void test_auto_off(void)
{
    TEST_ASSERT_EQUAL(300000, KEY_LOG_AUTO_OFF_MS);
    TEST_ASSERT_FALSE(key_log_expired(1000, 1000 + KEY_LOG_AUTO_OFF_MS - 1));
    TEST_ASSERT_TRUE(key_log_expired(1000, 1000 + KEY_LOG_AUTO_OFF_MS));
    TEST_ASSERT_FALSE(key_log_expired(1000, 500));   /* a clock behind the start is not expiry */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_chars_masked);
    RUN_TEST(test_modified_chars_kept);
    RUN_TEST(test_special_keys_named);
    RUN_TEST(test_swallowed_suffix);
    RUN_TEST(test_auto_off);
    return UNITY_END();
}
