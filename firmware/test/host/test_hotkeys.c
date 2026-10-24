#include <string.h>
#include "unity.h"
#include "ui/hotkeys.h"

void setUp(void) {}
void tearDown(void) {}

static key_event_t ch(uint32_t cp, uint8_t mods)
{
    key_event_t e = { .type = KEY_EV_CHAR, .cp = cp, .key = SK_NONE, .mods = mods };
    return e;
}

static key_event_t sk(special_key_t k, uint8_t mods)
{
    key_event_t e = { .type = KEY_EV_SPECIAL, .cp = 0, .key = k, .mods = mods };
    return e;
}

static void test_takeover_leave(void)
{
    key_event_t e;
    e = ch(']', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, true));
    e = ch('q', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, true));
    e = ch('Q', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, true));
    e = ch(']', 0);        TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));
    e = ch(']', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, false));
    e = ch('c', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));   /* Claude Code gets it */
}

static void test_takeover_scroll(void)
{
    key_event_t e;
    e = sk(SK_UP, MOD_SHIFT);   TEST_ASSERT_EQUAL(HK_T_SCROLL_UP, hk_takeover(&e, true));
    e = sk(SK_DOWN, MOD_SHIFT); TEST_ASSERT_EQUAL(HK_T_SCROLL_DOWN, hk_takeover(&e, true));
    e = sk(SK_UP, 0);           TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));
    e = sk(SK_UP, MOD_SHIFT);   TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, false));
    e = sk(SK_PGUP, MOD_SHIFT); TEST_ASSERT_EQUAL(HK_T_SCROLL_UP, hk_takeover(&e, false));
    e = sk(SK_PGDN, MOD_SHIFT); TEST_ASSERT_EQUAL(HK_T_SCROLL_DOWN, hk_takeover(&e, true));
}

static void test_dashboard_keys(void)
{
    key_event_t e;
    e = ch('o', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_OPEN, hk_dashboard(&e));
    e = ch('P', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_SETTINGS, hk_dashboard(&e));
    e = ch('m', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
    e = ch('k', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_KEYS, hk_dashboard(&e));
    e = ch('p', MOD_META); TEST_ASSERT_EQUAL(HK_D_SETTINGS, hk_dashboard(&e));
    e = ch('x', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_LOCAL, hk_dashboard(&e));
    e = ch('p', 0);        TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
    e = sk(SK_ENTER, 0);   TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
}

static void test_dashboard_ctrl_never_typed(void)
{
    for (uint32_t cp = 0x20; cp <= 0x7e; cp++) {
        key_event_t e = ch(cp, MOD_CTRL);
        TEST_ASSERT_NOT_EQUAL(HK_D_NONE, hk_dashboard(&e));
        e = ch(cp, MOD_CTRL | MOD_SHIFT);
        TEST_ASSERT_NOT_EQUAL(HK_D_NONE, hk_dashboard(&e));
    }
}

static void test_ctrl_m_is_not_enter(void)
{
    key_event_t e = ch('m', MOD_CTRL);
    TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
    e = sk(SK_ENTER, MOD_CTRL);
    TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));   /* Ctrl+Enter keeps today's meaning */
}

static bool any_line_has(const char *needle)
{
    for (int i = 0; hk_overlay_lines[i]; i++)
        if (strstr(hk_overlay_lines[i], needle)) return true;
    return false;
}

static void test_overlay_lists_every_shortcut(void)
{
    int n = 0;
    while (hk_overlay_lines[n]) n++;
    TEST_ASSERT_EQUAL(10, n);
    const char *need[] = { "Ctrl+]", "Ctrl+P", "Ctrl+M", "Ctrl+K", "Ctrl+O", "Shift+Up" };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++)
        TEST_ASSERT_TRUE_MESSAGE(any_line_has(need[i]), need[i]);
}

/* The spec's strings, pinned (review M3) */
static void test_hint_and_toast_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("Ctrl+O open \xc2\xb7 Ctrl+P settings \xc2\xb7 Ctrl+K keys", HK_DASH_HINT);
    TEST_ASSERT_EQUAL_STRING("Ctrl+] to leave", HK_TAKEOVER_TOAST);
    TEST_ASSERT_EQUAL(3000, HK_TAKEOVER_TOAST_MS);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_takeover_leave);
    RUN_TEST(test_takeover_scroll);
    RUN_TEST(test_dashboard_keys);
    RUN_TEST(test_dashboard_ctrl_never_typed);
    RUN_TEST(test_ctrl_m_is_not_enter);
    RUN_TEST(test_overlay_lists_every_shortcut);
    RUN_TEST(test_hint_and_toast_strings);
    return UNITY_END();
}
