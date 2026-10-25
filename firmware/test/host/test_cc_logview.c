/* The Sessions output pane read off a rendered `claude logs` screen (cc_logview). The fixtures are `claude logs <id>`
   of live companion-test-* background sessions (Claude Code 2.1.310, a 200 x 50 terminal; the plan label and the
   time zone replaced with same-length placeholders). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "companion/cc_logview.h"
#include "companion/cc_cmd.h"
#include "ui/sessions_model.h"

#define OUT_CAP 4096
#define SEL "\xe2\x9d\xaf"   /* ❯ */
#define DOT "\xe2\x8f\xba"   /* ⏺ */

void setUp(void) {}
void tearDown(void) {}

static char s_raw[256 * 1024];
static char s_out[OUT_CAP];

static size_t load(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, name);
    size_t n = fread(s_raw, 1, sizeof(s_raw) - 1, f);
    fclose(f);
    s_raw[n] = 0;
    TEST_ASSERT_GREATER_THAN(1000, n);
    return n;
}

static int render(const char *name, int max_lines)
{
    size_t n = load(name);
    return cc_logview_tail(s_raw, n, max_lines, s_out, sizeof(s_out));
}

static void has(const char *needle)
{
    if (!strstr(s_out, needle)) {
        printf("missing \"%s\" in:\n%s\n", needle, s_out);
        TEST_FAIL_MESSAGE(needle);
    }
}

/* The footer, the status line, the spinner, the input box, the banner and the update check: none of it shows. */
static void no_chrome(void)
{
    static const char *chrome[] = {
        "tokens |", "auto mode on", "shift+tab", "agents", "SessionStart", "Checking for updates", "/effort",
        "weekly limit", "esc to interrupt", "Claude Code v", "Plan label", "\xe2\x96\x90\xe2\x96\x9b",   /* ▐▛ logo */
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",                                                         /* ─── rule */
        "Baked for", "Cogitated for", "Worked for", "Cooked for", "Saut\xc3\xa9" "ed for", "Pollinating", "Computing",
        "\xe2\x9c\xbb ", "\xe2\x9c\xbd ", SEL " \n",
    };
    for (size_t i = 0; i < sizeof(chrome) / sizeof(chrome[0]); i++) {
        if (strstr(s_out, chrome[i])) {
            printf("chrome \"%s\" in:\n%s\n", chrome[i], s_out);
            TEST_FAIL_MESSAGE(chrome[i]);
        }
    }
    size_t n = strlen(s_out);
    TEST_ASSERT_TRUE(n > 0 && s_out[n - 1] != '\n' && s_out[n - 1] != ' ');
    TEST_ASSERT_FALSE(cc_screen_input_ready(s_out));   /* no empty input line left */
}

static void test_size(void)
{
    size_t n = load("logs_short.bin");
    int cols = 0, rows = 0;
    size_t start = 99;
    bool cleared = false;
    cc_logview_size(s_raw, n, &cols, &rows, &start, &cleared);
    TEST_ASSERT_EQUAL(208, cols);   /* the 200-column rules + 8 */
    TEST_ASSERT_EQUAL(50, rows);    /* ESC[50;1H parks the cursor on the last row */
    TEST_ASSERT_TRUE(cleared);
    TEST_ASSERT_EQUAL(0, memcmp(s_raw + start, "\x1b[2J", 4));
    /* nothing to go by: the defaults */
    cc_logview_size("plain", 5, &cols, &rows, &start, &cleared);
    TEST_ASSERT_EQUAL(264, cols);
    TEST_ASSERT_EQUAL(60, rows);
    TEST_ASSERT_FALSE(cleared);
    TEST_ASSERT_EQUAL(0, start);
}

static void test_short_reply(void)
{
    TEST_ASSERT_EQUAL(2, render("logs_short.bin", 40));
    TEST_ASSERT_EQUAL_STRING(SEL " Reply with exactly this sentence and nothing else: The short test reply is here.\n"
                             DOT " The short test reply is here.", s_out);
    no_chrome();
    TEST_ASSERT_EQUAL(0, sm_tail_options(s_out));
}

/* The bug: the escape-stripping heuristic on the same bytes ends in the footer's redraws. */
static void test_heuristic_shows_chrome(void)
{
    load("logs_short.bin");
    cc_log_tail(s_raw, 40);
    TEST_ASSERT_TRUE(strstr(s_raw, "auto mode on") || strstr(s_raw, "tokens") || strstr(s_raw, "SessionStart"));
}

/* Four paragraphs on a 200-column screen: each wrapped paragraph comes back as one line for the pane to wrap. */
static void test_long_reply(void)
{
    TEST_ASSERT_EQUAL(5, render("logs_long.bin", 40));
    no_chrome();
    has(SEL " Without using any tools, write four paragraphs");
    has("\n" DOT " Tides begin with gravity");
    has("across Earth's diameter is called the tidal force.");   /* joined across the TUI's wrap */
    has("\n  The Sun also raises tides");
    has("remarkable accuracy.");
    TEST_ASSERT_NULL(strstr(s_out, "  called the tidal force"));
    /* the last two lines only */
    TEST_ASSERT_EQUAL(2, render("logs_long.bin", 2));
    TEST_ASSERT_EQUAL(0, strncmp(s_out, "  The simple two-bulge picture", 30));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\n  Tides matter far beyond the beach."));
}

static void test_tool_call(void)
{
    TEST_ASSERT_EQUAL(3, render("logs_tool.bin", 40));
    no_chrome();
    has(SEL " Use the Bash tool to run: ls -1");
    has("\n  Listed 1 directory\n");
    has(DOT " Entry count: 6.");
}

/* A plain-text question ending in a numbered list: the options line still finds the three options. */
static void test_numbered_question(void)
{
    TEST_ASSERT_EQUAL(5, render("logs_numbered.bin", 40));
    no_chrome();
    has(DOT " Which test fruit do you want?\n  1. Apple\n  2. Pear\n  3. Plum");
    has("on their own lines: 1. Apple 2. Pear 3. Plum. Put nothing after the options.");   /* the prompt, joined */
    TEST_ASSERT_EQUAL(3, sm_tail_options(s_out));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(s_out));
}

/* AskUserQuestion: no input box under the dialog; its question and options show, its frame rules and the options'
   descriptions don't. It ends in
   the dialog's own hint, so no "1-N picks an option" (typing can't answer a selector). */
static void test_ask_user_question(void)
{
    TEST_ASSERT_EQUAL(9, render("logs_question.bin", 40));
    no_chrome();
    has("Which colour do you prefer for the test banner?\n" SEL " 1. Red\n  2. Green\n  3. Blue\n  4. Type something.\n"
        "  5. Chat about this\nEnter to select");   /* the options' descriptions go: the question and options fit */
    TEST_ASSERT_NULL(strstr(s_out, "Use red for the test banner."));
    TEST_ASSERT_EQUAL(0, sm_tail_options(s_out));
}

/* A permission dialog: the tool call above it and the dialog; the dialog matcher still sees it. */
static void test_permission_dialog(void)
{
    TEST_ASSERT_GREATER_THAN(10, render("logs_perm.bin", 40));
    no_chrome();
    has(DOT " Running 1 shell command");
    has("$ mkdir -p /tmp/companion-test-perm-dir");
    has(" Do you want to proceed?\n " SEL " 1. Yes\n");
    has("   4. No\n");
    TEST_ASSERT_NULL(strstr(s_out, "\xe2\x95\x8c\xe2\x95\x8c"));   /* ╌╌ the dashed frame */
    TEST_ASSERT_TRUE(cc_screen_is_permission_prompt(s_out));
    TEST_ASSERT_EQUAL(0, sm_tail_options(s_out));
}

/* 70 lines on a 50-row screen: the last of them, newest last. */
static void test_overflow(void)
{
    TEST_ASSERT_EQUAL(40, render("logs_overflow.bin", 40));
    no_chrome();
    TEST_ASSERT_EQUAL(0, strncmp(s_out, "  Line 31: the test word is ready\n", 34));
    const char *last = "  Line 70: the test word is ready";
    TEST_ASSERT_EQUAL_STRING(last, s_out + strlen(s_out) - strlen(last));
    TEST_ASSERT_EQUAL(10, render("logs_overflow.bin", 10));
    TEST_ASSERT_EQUAL(0, strncmp(s_out, "  Line 61:", 10));
}

/* Mid-reply: the streamed paragraphs show; the spinner, the effort and a usage notice (right-aligned) don't. */
static void test_busy(void)
{
    TEST_ASSERT_EQUAL(3, render("logs_busy.bin", 40));
    no_chrome();
    has(DOT " The light on Garrow Point");
    has("\n  They came to her in cardboard boxes");
}

/* The update check is a right-aligned notice on the row above the input box (as live sessions draw it, past the
   middle, reaching the right edge): appended to the short session's stream as a frame of its own. */
static void test_update_check_hidden(void)
{
    size_t n = load("logs_short.bin");
    const char frame[] = "\x1b[H\r\x1b[44B\x1b[176C\x1b[38;2;153;153;153mChecking for updates\xe2\x80\xa6\x1b[39m\x1b[50;1H\x1b[47;3H";
    memcpy(s_raw + n, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n + sizeof(frame) - 1, 40, s_out, sizeof(s_out)));
    no_chrome();
    has(DOT " The short test reply is here.");
    /* a short notice past the middle goes too */
    const char frame2[] = "\x1b[H\r\x1b[43B\x1b[150CChecking for updates\xe2\x80\xa6";
    memcpy(s_raw + n, frame2, sizeof(frame2));
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n + sizeof(frame2) - 1, 40, s_out, sizeof(s_out)));
    no_chrome();
}

/* Without the full clear the screen is only what was redrawn since: with the input box found it is still read;
   with neither (only footer redraws) it's 0, and the caller falls back. */
static void test_partial(void)
{
    size_t n = load("logs_short.bin");
    const char *footer = strstr(s_raw + 100, "\x1b[50;1H\x1b[47;3H");   /* after the first full frame */
    TEST_ASSERT_NOT_NULL(footer);
    size_t at = (size_t)(footer - s_raw);
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw + at, n - at, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING("", s_out);
    /* the first bytes cut away (as a 256 KB tail of a longer stream would): no clear, but the input box is there */
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw + 4, n - 4, 40, s_out, sizeof(s_out)));
    no_chrome();
}

static void test_cap_and_args(void)
{
    size_t n = load("logs_long.bin");
    char small[64];
    int k = cc_logview_tail(s_raw, n, 40, small, sizeof(small));
    TEST_ASSERT_EQUAL(1, k);   /* the end of the last paragraph, from a character start */
    TEST_ASSERT_TRUE(strlen(small) <= 63 && strlen(small) > 40);
    TEST_ASSERT_TRUE(((unsigned char)small[0] & 0xc0) != 0x80);
    TEST_ASSERT_EQUAL_STRING("remarkable accuracy.", small + strlen(small) - 20);
    TEST_ASSERT_EQUAL(0, cc_logview_tail(NULL, 5, 40, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, 0, 40, small, sizeof(small)));
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, n, 0, small, sizeof(small)));
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, n, 40, NULL, 10));
    TEST_ASSERT_EQUAL(0, cc_logview_tail("\x1b[2J\x1b[H", 7, 40, small, sizeof(small)));   /* an empty screen */
    TEST_ASSERT_EQUAL(0, cc_logview_tail("\xe2\x94\x80\xe2", 4, 40, small, sizeof(small)));   /* cut mid-character */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_size);
    RUN_TEST(test_short_reply);
    RUN_TEST(test_heuristic_shows_chrome);
    RUN_TEST(test_long_reply);
    RUN_TEST(test_tool_call);
    RUN_TEST(test_numbered_question);
    RUN_TEST(test_ask_user_question);
    RUN_TEST(test_permission_dialog);
    RUN_TEST(test_overflow);
    RUN_TEST(test_busy);
    RUN_TEST(test_update_check_hidden);
    RUN_TEST(test_partial);
    RUN_TEST(test_cap_and_args);
    return UNITY_END();
}
