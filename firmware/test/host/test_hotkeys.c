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
    e = ch('c', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));   /* Claude Code gets it */
}

static void test_ssh_ctrl_bracket_leaves(void)
{
    key_event_t e;
    e = ch(']', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, false));
    e = ch('q', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, false));
}

static void test_terminal_passes_host_keys(void)
{
    key_event_t e;
    for (int t = 0; t < 2; t++) {
        e = ch('k', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = ch('t', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = ch(' ', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
    }
}

static void test_dashboard_terminal_keys(void)
{
    key_event_t e;
    e = ch('t', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
    e = ch('T', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
    e = ch('m', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
}

static void test_ctrl_space_is_voice(void)
{
    key_event_t e = ch(' ', MOD_CTRL);
    TEST_ASSERT_EQUAL(HK_D_VOICE, hk_dashboard(&e));
    e = ch(' ', 0);
    TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
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

static void test_overlay_lists_every_shortcut(void)
{
    const char *need[] = { "Up / Down", "Enter", "Ctrl+O", "Ctrl+T", "Ctrl+P", "Ctrl+K", "Ctrl+Space",
                           "Esc", "Ctrl+Q", "Shift+Up / Down" };
    int n = hk_overlay_n;
    TEST_ASSERT_EQUAL(10, n);
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        bool found = false;
        for (int k = 0; k < n; k++)
            if (strstr(tr(hk_overlay[k].keys), need[i]) && tr(hk_overlay[k].what)[0]) found = true;
        TEST_ASSERT_TRUE_MESSAGE(found, need[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_takeover_leave);
    RUN_TEST(test_ssh_ctrl_bracket_leaves);
    RUN_TEST(test_terminal_passes_host_keys);
    RUN_TEST(test_dashboard_terminal_keys);
    RUN_TEST(test_ctrl_space_is_voice);
    RUN_TEST(test_takeover_scroll);
    RUN_TEST(test_dashboard_keys);
    RUN_TEST(test_dashboard_ctrl_never_typed);
    RUN_TEST(test_ctrl_m_is_not_enter);
    RUN_TEST(test_overlay_lists_every_shortcut);
    return UNITY_END();
}
