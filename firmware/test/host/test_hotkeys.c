#include <stdio.h>
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
    e = sk(SK_LEFT, MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, true));
    e = sk(SK_LEFT, MOD_CTRL | MOD_SHIFT); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, true));
    e = sk(SK_LEFT, 0);    TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));   /* plain ← goes to the Host */
    e = sk(SK_RIGHT, MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));
}

static void test_ssh_ctrl_bracket_leaves(void)
{
    key_event_t e;
    e = ch(']', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, false));
    e = ch('q', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, false));
    e = sk(SK_LEFT, MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, false));
}

static void test_terminal_passes_host_keys(void)
{
    key_event_t e;
    for (int t = 0; t < 2; t++) {
        e = ch('k', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = ch('t', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = ch('@', MOD_CTRL); TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));   /* NUL still reaches the Host */
        e = ch(' ', 0);        TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
    }
}

/* Voice in a session (follow-up 2026-10-24): Ctrl+Space is taken in a take-over and an SSH session alike */
static void test_terminal_ctrl_space_is_voice(void)
{
    key_event_t e = ch(' ', MOD_CTRL);
    TEST_ASSERT_EQUAL(HK_T_VOICE, hk_takeover(&e, true));
    TEST_ASSERT_EQUAL(HK_T_VOICE, hk_takeover(&e, false));
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

/* The Tab5 keyboard has no Shift+arrow, Fn or PgUp/PgDn: Ctrl+Up / Ctrl+Down scroll, in a take-over and an SSH
   session alike, and never reach the Host. Alt+Up/Down and Ctrl+Right stay with the Host. */
static void test_ctrl_updown_scrolls(void)
{
    key_event_t e;
    for (int t = 0; t < 2; t++) {
        e = sk(SK_UP, MOD_CTRL);               TEST_ASSERT_EQUAL(HK_T_SCROLL_UP, hk_takeover(&e, t));
        e = sk(SK_DOWN, MOD_CTRL);             TEST_ASSERT_EQUAL(HK_T_SCROLL_DOWN, hk_takeover(&e, t));
        e = sk(SK_UP, MOD_CTRL | MOD_SHIFT);   TEST_ASSERT_EQUAL(HK_T_SCROLL_UP, hk_takeover(&e, t));
        /* negatives: these go to the Host */
        e = sk(SK_UP, MOD_ALT);                TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_DOWN, MOD_ALT);              TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_UP, MOD_META);               TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_UP, MOD_CTRL | MOD_ALT);     TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_RIGHT, MOD_CTRL);            TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_UP, 0);                      TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_DOWN, 0);                    TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        e = sk(SK_PGUP, MOD_CTRL);             TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, t));
        /* Ctrl+Left is still the leave key */
        e = sk(SK_LEFT, MOD_CTRL);             TEST_ASSERT_EQUAL(HK_T_LEAVE, hk_takeover(&e, t));
    }
}

static void test_dashboard_keys(void)
{
    key_event_t e;
    e = ch('o', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_OPEN, hk_dashboard(&e));
    e = ch('P', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_SETTINGS, hk_dashboard(&e));
    e = ch('m', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_TERMINAL, hk_dashboard(&e));
    e = ch('k', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_KEYS, hk_dashboard(&e));
    e = ch('p', MOD_META); TEST_ASSERT_EQUAL(HK_D_SETTINGS, hk_dashboard(&e));
    e = ch('z', MOD_CTRL); TEST_ASSERT_EQUAL(HK_D_LOCAL, hk_dashboard(&e));
    e = ch('p', 0);        TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
    e = sk(SK_ENTER, 0);   TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
}

/* Ctrl+X stops the selected session (after a confirm): its own action, never the silent LOCAL fallback */
static void test_dashboard_ctrl_x_is_stop(void)
{
    key_event_t e;
    e = ch('x', MOD_CTRL);             TEST_ASSERT_EQUAL(HK_D_STOP, hk_dashboard(&e));
    e = ch('X', MOD_CTRL);             TEST_ASSERT_EQUAL(HK_D_STOP, hk_dashboard(&e));
    e = ch('X', MOD_CTRL | MOD_SHIFT); TEST_ASSERT_EQUAL(HK_D_STOP, hk_dashboard(&e));
    e = ch('x', MOD_META);             TEST_ASSERT_EQUAL(HK_D_STOP, hk_dashboard(&e));
    e = ch('x', 0);                    TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));   /* a plain x is typed */
    e = ch('x', MOD_SHIFT);            TEST_ASSERT_EQUAL(HK_D_NONE, hk_dashboard(&e));
    /* only x: its neighbours stay LOCAL */
    e = ch('z', MOD_CTRL);             TEST_ASSERT_EQUAL(HK_D_LOCAL, hk_dashboard(&e));
    e = ch('c', MOD_CTRL);             TEST_ASSERT_EQUAL(HK_D_LOCAL, hk_dashboard(&e));
    /* inside a take-over Ctrl+X goes to Claude Code */
    e = ch('x', MOD_CTRL);             TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, true));
    TEST_ASSERT_EQUAL(HK_T_NONE, hk_takeover(&e, false));
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
    struct { hk_sec_t sec; const char *keys; const char *what; } need[] = {
        { HK_SEC_SESSIONS, "Up / Down", "move between sessions" },
        { HK_SEC_SESSIONS, "Enter", "send the answer (empty: open)" },
        { HK_SEC_SESSIONS, "Ctrl+O", "open the session" },
        { HK_SEC_SESSIONS, "Ctrl+X", "stop the selected session" },
        { HK_SEC_SESSIONS, "Ctrl+T / Ctrl+M", "terminal (other SSH hosts)" },
        { HK_SEC_SESSIONS, "Ctrl+P", "settings" },
        { HK_SEC_SESSIONS, "Ctrl+K", "this list" },
        { HK_SEC_SESSIONS, "Ctrl+Space", "voice input: press to start, again to stop" },
        { HK_SEC_SESSIONS, "Esc", "clear the answer (cancels a recording)" },
        { HK_SEC_SESSION, "Ctrl+\xe2\x86\x90", "leave; it keeps running (Ctrl+Q too, Ctrl+] from USB)" },
        { HK_SEC_SESSION, "Ctrl+Up / Down", "scroll: PgUp / PgDn to Claude Code, local in SSH; or drag" },
        { HK_SEC_SESSION, "Shift+Up / Down", "the same from a USB keyboard (Shift+PgUp / PgDn too)" },
        { HK_SEC_SESSION, "Ctrl+Space", "dictate into the prompt: typed, not sent" },
    };
    int n = hk_overlay_n;
    TEST_ASSERT_EQUAL(13, n);
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        bool found = false;
        for (int k = 0; k < n; k++)
            if (hk_overlay[k].sec == need[i].sec && !strcmp(tr(hk_overlay[k].keys), need[i].keys) && !strcmp(tr(hk_overlay[k].what), need[i].what)) found = true;
        TEST_ASSERT_TRUE_MESSAGE(found, need[i].keys);
    }
    /* the sections come in order, Sessions first, and each line documents at least one key */
    for (int k = 0; k < n; k++) {
        if (k) TEST_ASSERT_TRUE(hk_overlay[k].sec >= hk_overlay[k - 1].sec);
        TEST_ASSERT_TRUE(hk_overlay[k].covers[0].type != 0 || hk_overlay[k].covers[0].code != 0);
    }
    TEST_ASSERT_EQUAL_STRING("Sessions", tr(hk_section_title(HK_SEC_SESSIONS)));
    TEST_ASSERT_EQUAL_STRING("In a session", tr(hk_section_title(HK_SEC_SESSION)));
}

/* Every key hk_dashboard or hk_takeover keeps has a help line in its section: a new key fails here until the Ctrl+K
   list documents it. LOCAL (any other Ctrl or Meta character on Sessions: eaten, does nothing) is not a shortcut. */
static bool covered(hk_sec_t sec, const key_event_t *e)
{
    for (int k = 0; k < hk_overlay_n; k++)
        if (hk_overlay[k].sec == sec && hk_line_covers(&hk_overlay[k], e)) return true;
    return false;
}
static void test_every_handled_key_has_a_line(void)
{
    char why[64];
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t c = pass ? SK_ENTER : 0x20; c <= (pass ? (uint32_t)SK_F12 : 0x7e); c++)
            for (uint8_t m = 0; m < 16; m++) {
                key_event_t e = pass ? sk((special_key_t)c, m) : ch(c, m);
                hk_dash_t d = hk_dashboard(&e);
                snprintf(why, sizeof(why), "Sessions: %s 0x%x mods 0x%x", pass ? "key" : "char", (unsigned)c, m);
                if (d != HK_D_NONE && d != HK_D_LOCAL) TEST_ASSERT_TRUE_MESSAGE(covered(HK_SEC_SESSIONS, &e), why);
                for (int t = 0; t < 2; t++) {
                    snprintf(why, sizeof(why), "session (%s): %s 0x%x mods 0x%x", t ? "take-over" : "SSH", pass ? "key" : "char", (unsigned)c, m);
                    if (hk_takeover(&e, t) != HK_T_NONE) TEST_ASSERT_TRUE_MESSAGE(covered(HK_SEC_SESSION, &e), why);
                }
            }
    /* the check bites: a key no line lists is not covered */
    key_event_t e = ch('z', MOD_CTRL);
    TEST_ASSERT_FALSE(covered(HK_SEC_SESSIONS, &e));
    e = sk(SK_F5, 0);
    TEST_ASSERT_FALSE(covered(HK_SEC_SESSION, &e));
    e = ch('O', MOD_META);   /* Meta counts as Ctrl on Sessions, upper case as lower */
    TEST_ASSERT_TRUE(covered(HK_SEC_SESSIONS, &e));
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
    RUN_TEST(test_ctrl_updown_scrolls);
    RUN_TEST(test_terminal_ctrl_space_is_voice);
    RUN_TEST(test_dashboard_keys);
    RUN_TEST(test_dashboard_ctrl_x_is_stop);
    RUN_TEST(test_dashboard_ctrl_never_typed);
    RUN_TEST(test_ctrl_m_is_not_enter);
    RUN_TEST(test_overlay_lists_every_shortcut);
    RUN_TEST(test_every_handled_key_has_a_line);
    return UNITY_END();
}
