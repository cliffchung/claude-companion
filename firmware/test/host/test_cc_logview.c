/* The Sessions output pane read off a rendered `claude logs` screen (cc_logview). The fixtures are `claude logs <id>`
   of live companion-test-* background sessions (Claude Code 2.1.310, a 200 x 50 terminal; the plan label and the
   time zone replaced with same-length placeholders). logs_working.bin is a busy one (same version) whose turn outlasted
   the buffer `claude logs` keeps: no clear in it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "companion/cc_logview.h"
#include "companion/cc_cmd.h"
#include "companion/cc_tailbuf.h"
#include "term/vt.h"
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
    return cc_logview_tail(s_raw, n, true, max_lines, s_out, sizeof(s_out));
}

static void has(const char *needle)
{
    if (!strstr(s_out, needle)) {
        printf("missing \"%s\" in:\n%s\n", needle, s_out);
        TEST_FAIL_MESSAGE(needle);
    }
}

static const char *const s_chrome[] = {
    "tokens |", "auto mode on", "shift+tab", "agents", "SessionStart", "Checking for updates", "/effort",
    "weekly limit", "esc to interrupt", "Claude Code v", "Plan label", "\xe2\x96\x90\xe2\x96\x9b",   /* ▐▛ logo */
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",                                                         /* ─── rule */
    "Baked for", "Cogitated for", "Worked for", "Cooked for", "Saut\xc3\xa9" "ed for", "Pollinating", "Computing",
    "\xe2\x9c\xbb ", "\xe2\x9c\xbd ", SEL " \n",
    "tokens)", "thinking with", "Deliberating", "Actualizing", "ctrl+b", "Region/Example_Zone", "hook \xc2\xb7",
};

/* none of the chrome strings in t. The fallback may keep a stray piece that is also a sentence: "3 agents", or a bare
   verb redrawn without its glyph ("Pollinating…", as a reply's "Thinking…" would be). */
static void no_chrome_in_as(const char *t, bool fallback)
{
    static const char *const strays[] = { "agents", "Pollinating", "Computing", "Deliberating", "Actualizing" };
    for (size_t i = 0; i < sizeof(s_chrome) / sizeof(s_chrome[0]); i++) {
        bool stray = false;
        for (size_t k = 0; fallback && k < sizeof(strays) / sizeof(strays[0]); k++) stray |= !strcmp(s_chrome[i], strays[k]);
        if (stray) continue;
        if (strstr(t, s_chrome[i])) {
            printf("chrome \"%s\" in:\n%s\n", s_chrome[i], t);
            TEST_FAIL_MESSAGE(s_chrome[i]);
        }
    }
}

static void no_chrome_in(const char *t) { no_chrome_in_as(t, false); }

/* The footer, the status line, the spinner, the input box, the banner and the update check: none of it shows. */
static void no_chrome(void)
{
    no_chrome_in(s_out);
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
    TEST_ASSERT_TRUE(cc_logview_size(s_raw, n, &cols, &rows, &start, &cleared));
    TEST_ASSERT_EQUAL(208, cols);   /* the 200-column rules + 8 */
    TEST_ASSERT_EQUAL(50, rows);    /* ESC[50;1H parks the cursor on the last row */
    TEST_ASSERT_TRUE(cleared);
    TEST_ASSERT_EQUAL(0, memcmp(s_raw + start, "\x1b[2J", 4));
    /* nothing to go by: the defaults */
    TEST_ASSERT_TRUE(cc_logview_size("plain", 5, &cols, &rows, &start, &cleared));
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

/* The fallback (busy-pane fix): the escape-stripping heuristic on the same bytes used to end in the footer's
   redraws ("auto mode on", tokens, "SessionStart"). It now drops the chrome and keeps the conversation. */
static void test_fallback_drops_chrome(void)
{
    static const char *fx[] = { "logs_short.bin", "logs_long.bin", "logs_busy.bin", "logs_tool.bin", "logs_numbered.bin",
                                "logs_perm.bin", "logs_working.bin" };
    for (size_t i = 0; i < sizeof(fx) / sizeof(fx[0]); i++) {
        load(fx[i]);
        TEST_ASSERT_GREATER_THAN(0, cc_logview_fallback(s_raw, 40));
        no_chrome_in_as(s_raw, true);
    }
    load("logs_short.bin");
    cc_logview_fallback(s_raw, 40);
    TEST_ASSERT_NOT_NULL(strstr(s_raw, "The short test reply is here."));
    load("logs_working.bin");
    cc_logview_fallback(s_raw, 40);
    TEST_ASSERT_NOT_NULL(strstr(s_raw, DOT " Running 1 shell command"));
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

/* A busy session whose turn outlasted `claude logs`'s buffer (it keeps only its recent output, trimmed at a frame):
   no clear, so no rules, no input box and no footer drawn, only the rows redrawn since. The list above the command
   was redrawn as the screen moved up; the spinner (its "(" never redrawn), the usage notice under it, the counter's
   stale fragments and the "(ctrl+b …)" hint don't show. */
static void test_working(void)
{
    size_t n = load("logs_working.bin");
    TEST_ASSERT_NULL(strstr(s_raw, "\x1b[2J"));
    TEST_ASSERT_EQUAL(40, cc_logview_tail(s_raw, n, true, 40, s_out, sizeof(s_out)));
    no_chrome();
    has("  60  Breadfruit\n" DOT " Running 1 shell command \xc2\xb7 44s\xe2\x80\xa6\n");
    has("Passionfruit\n");
    const char *last = "  \xe2\x8e\xbf  $ for i in $(seq 1 120); do echo tick $i; sleep 1; done (42s \xc2\xb7 43 lines)";
    TEST_ASSERT_EQUAL_STRING(last, s_out + strlen(s_out) - strlen(last));
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n, true, 2, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL(0, strncmp(s_out, DOT " Running 1 shell command", strlen(DOT " Running 1 shell command")));
    /* a front cut on the device as well: still refused, as before */
    TEST_ASSERT_EQUAL(CC_LV_PARTIAL, cc_logview_tail(s_raw, n, false, 40, s_out, sizeof(s_out)));
}

/* A sparse busy screen with nothing but the spinner and its stale counters: no conversation (0), and the fallback
   keeps nothing either, so the pane says "Nothing to show yet" rather than "Actualizing…" or "51s · 412 words)". */
static void test_working_spinner_only(void)
{
    static const char frames[] =
        "\x1b[H\r\x1b[8B\x1b[38;2;153;153;153m\xe2\x8f\xba\x1b[39m\x1b[50;1H\x1b[47;3H"
        "\x1b[H\r\x1b[43B\x1b[38;2;215;119;87m\xe2\x9c\xa2\x1b[3G\x1b[38;2;233;154;122mActualizing\xe2\x80\xa6\x1b[39m\x1b[50;1H\x1b[47;3H"
        "\x1b[H\r\x1b[31C\x1b[8B\x1b[38;2;153;153;153m4\r\x1b[35B\x1b[38;2;215;119;87m\xe2\x9c\xb6\x1b[39m\x1b[50;1H\x1b[47;3H"
        "\x1b[H\r\x1b[65C\x1b[9B51s \xc2\xb7 412 words)\x1b[50;1H\x1b[47;3H"
        "\x1b[H\r\x1b[16C\x1b[43B12s \xc2\xb7 \xe2\x86\x93 312 tokens)\x1b[50;1H\x1b[47;3H"
        "\x1b[H\r\x1b[43C\x1b[48B7\x1b[50;1H\x1b[47;3H";
    char buf[sizeof(frames)];
    memcpy(buf, frames, sizeof(frames));
    TEST_ASSERT_EQUAL(CC_LV_NONE, cc_logview_tail(buf, sizeof(frames) - 1, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING("", s_out);
    TEST_ASSERT_EQUAL(0, cc_logview_fallback(buf, 40));
    TEST_ASSERT_EQUAL_STRING("", buf);
    /* the same frames after a clear are no busy sparse screen: no layout, as before */
    char clr[sizeof(frames) + 8] = "\x1b[2J";
    memcpy(clr + 4, frames, sizeof(frames));
    TEST_ASSERT_EQUAL(CC_LV_NO_LAYOUT, cc_logview_tail(clr, sizeof(frames) + 3, true, 40, s_out, sizeof(s_out)));
}

/* The update check is a right-aligned notice on the row above the input box (as live sessions draw it, past the
   middle, reaching the right edge): appended to the short session's stream as a frame of its own. */
static void test_update_check_hidden(void)
{
    size_t n = load("logs_short.bin");
    const char frame[] = "\x1b[H\r\x1b[44B\x1b[176C\x1b[38;2;153;153;153mChecking for updates\xe2\x80\xa6\x1b[39m\x1b[50;1H\x1b[47;3H";
    memcpy(s_raw + n, frame, sizeof(frame));
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n + sizeof(frame) - 1, true, 40, s_out, sizeof(s_out)));
    no_chrome();
    has(DOT " The short test reply is here.");
    /* as a live 155-column session draws it: ending 2 cells short of the rule (measured, positions only) */
    const char frame2[] = "\x1b[H\r\x1b[43B\x1b[178CChecking for updates\xe2\x80\xa6";
    memcpy(s_raw + n, frame2, sizeof(frame2));
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n + sizeof(frame2) - 1, true, 40, s_out, sizeof(s_out)));
    no_chrome();
}

/* Without the full clear the screen is only what was redrawn since: a stream whose front was cut is refused (the
   caller falls back), however much of it is left; the same bytes from the session's start render. */
static void test_partial(void)
{
    size_t n = load("logs_short.bin");
    TEST_ASSERT_EQUAL(0, memcmp(s_raw + 4, "\x1b[2J", 4));
    TEST_ASSERT_EQUAL(CC_LV_PARTIAL, cc_logview_tail(s_raw + 8, n - 8, false, 40, s_out, sizeof(s_out)));   /* past it */
    TEST_ASSERT_EQUAL_STRING("", s_out);
    const char *footer = strstr(s_raw + 100, "\x1b[50;1H\x1b[47;3H");   /* after the first full frame */
    TEST_ASSERT_NOT_NULL(footer);
    size_t at = (size_t)(footer - s_raw);
    TEST_ASSERT_EQUAL(CC_LV_PARTIAL, cc_logview_tail(s_raw + at, n - at, false, 40, s_out, sizeof(s_out)));
    /* a cut front but a clear inside: everything after it is there */
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_raw, n, false, 40, s_out, sizeof(s_out)));
}

static void test_cap_and_args(void)
{
    size_t n = load("logs_long.bin");
    char small[64];
    int k = cc_logview_tail(s_raw, n, true, 40, small, sizeof(small));
    TEST_ASSERT_EQUAL(1, k);   /* the end of the last paragraph, from a character start */
    TEST_ASSERT_TRUE(strlen(small) <= 63 && strlen(small) > 40);
    TEST_ASSERT_TRUE(((unsigned char)small[0] & 0xc0) != 0x80);
    TEST_ASSERT_EQUAL_STRING("remarkable accuracy.", small + strlen(small) - 20);
    TEST_ASSERT_EQUAL(0, cc_logview_tail(NULL, 5, true, 40, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, 0, true, 40, small, sizeof(small)));
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, n, true, 0, small, sizeof(small)));
    TEST_ASSERT_EQUAL(0, cc_logview_tail(s_raw, n, true, 40, NULL, 10));
    TEST_ASSERT_EQUAL(CC_LV_NO_LAYOUT, cc_logview_tail("\x1b[2J\x1b[H", 7, true, 40, small, sizeof(small)));   /* empty */
    TEST_ASSERT_TRUE(cc_logview_tail("\xe2\x94\x80\xe2", 4, true, 40, small, sizeof(small)) <= 0);   /* cut mid-character */
}

/* ------------------------------------------------------------ the stream since its latest clear (I1, I2) */
static char s_ring[512 * 1024];
static char s_big[2 * 1024 * 1024];

/* Feeds n bytes of p into a tail buffer in chunks of `chunk` (512 as cc_link reads them; 3 splits every clear). */
static size_t tb_run(cc_tailbuf_t *t, const char *p, size_t n, size_t chunk)
{
    for (size_t off = 0; off < n; off += chunk) cc_tailbuf_put(t, p + off, n - off < chunk ? n - off : chunk);
    return cc_tailbuf_finish(t);
}

/* 1.5 MB of older frames (no clear) before the fixture's clear: only the fixture's bytes are left, and they render. */
static void test_tailbuf_padding_before_clear(void)
{
    size_t n = load("logs_short.bin");
    size_t pad = 1536 * 1024;
    for (size_t i = 0; i < pad; i++) s_big[i] = (i % 64 == 63) ? '\r' : (char)('a' + i % 26);
    memcpy(s_big + pad, s_raw, n);
    for (size_t chunk = 3; chunk <= 512; chunk += 509) {
        cc_tailbuf_t t;
        cc_tailbuf_init(&t, s_ring, sizeof(s_ring), true);
        size_t len = tb_run(&t, s_big, pad + n, chunk);
        TEST_ASSERT_FALSE(t.dropped);   /* nothing since the clear was lost */
        TEST_ASSERT_TRUE(t.cleared);
        size_t clr = (size_t)(strstr(s_raw, "\x1b[2J") - s_raw);
        TEST_ASSERT_EQUAL(n - clr, len);
        TEST_ASSERT_EQUAL(0, memcmp(s_ring, s_raw + clr, len));
        TEST_ASSERT_EQUAL(2, cc_logview_tail(s_ring, len, !t.dropped, 40, s_out, sizeof(s_out)));
        has(DOT " The short test reply is here.");
    }
}

/* A clear, then more frames than the buffer holds: the front since the clear is gone, so the screen is refused. */
static void test_tailbuf_clear_then_overflow(void)
{
    size_t n = load("logs_short.bin");
    memcpy(s_big, s_raw, n);
    /* footer-only redraws, as a long-running session's spinner adds them */
    const char frame[] = "\x1b[H\r\x1b[43B\x1b[38;2;215;119;87m\xe2\x9c\xbb\x1b[39m\x1b[50;1H\x1b[47;3H";
    size_t o = n;
    while (o + sizeof(frame) < 1024 * 1024) { memcpy(s_big + o, frame, sizeof(frame) - 1); o += sizeof(frame) - 1; }
    cc_tailbuf_t t;
    cc_tailbuf_init(&t, s_ring, sizeof(s_ring), true);
    size_t len = tb_run(&t, s_big, o, 512);
    TEST_ASSERT_TRUE(t.dropped);
    TEST_ASSERT_EQUAL(sizeof(s_ring) - 1, len);
    TEST_ASSERT_NULL(strstr(s_ring, "\x1b[2J"));
    TEST_ASSERT_EQUAL(0, memcmp(s_ring + len - (sizeof(frame) - 1), frame, sizeof(frame) - 1));   /* the newest, in order */
    TEST_ASSERT_EQUAL(CC_LV_PARTIAL, cc_logview_tail(s_ring, len, !t.dropped, 40, s_out, sizeof(s_out)));
    /* a later clear starts it again: complete once more */
    cc_tailbuf_put(&t, s_raw, n);
    len = cc_tailbuf_finish(&t);
    TEST_ASSERT_FALSE(t.dropped);
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_ring, len, !t.dropped, 40, s_out, sizeof(s_out)));
}

/* No clear at all and the front cut (or the plain newest-bytes mode): dropped, refused. Whole and short: renders. */
static void test_tailbuf_no_clear(void)
{
    size_t n = load("logs_short.bin");
    char *body = strstr(s_raw, "\x1b[2J") + 4;   /* the fixture without its clear */
    size_t bn = n - (size_t)(body - s_raw);
    cc_tailbuf_t t;
    char small[4096];
    cc_tailbuf_init(&t, small, sizeof(small), true);
    size_t len = tb_run(&t, body, bn, 512);
    TEST_ASSERT_TRUE(t.dropped && !t.cleared);
    TEST_ASSERT_EQUAL(sizeof(small) - 1, len);
    TEST_ASSERT_EQUAL(0, memcmp(small, body + bn - len, len));
    TEST_ASSERT_EQUAL(CC_LV_PARTIAL, cc_logview_tail(small, len, !t.dropped, 40, s_out, sizeof(s_out)));
    cc_tailbuf_init(&t, s_ring, sizeof(s_ring), true);
    len = tb_run(&t, body, bn, 512);
    TEST_ASSERT_FALSE(t.dropped);   /* the whole stream: from its start */
    TEST_ASSERT_EQUAL(2, cc_logview_tail(s_ring, len, !t.dropped, 40, s_out, sizeof(s_out)));
    /* the plain mode ignores clears: the newest bytes, in order */
    cc_tailbuf_init(&t, small, sizeof(small), false);
    len = tb_run(&t, s_raw, n, 7);
    TEST_ASSERT_TRUE(t.dropped);
    TEST_ASSERT_EQUAL(0, memcmp(small, s_raw + n - len, len));
    TEST_ASSERT_EQUAL(0, small[len]);
    cc_tailbuf_init(&t, small, 1, true);   /* no room: nothing kept, still terminated */
    TEST_ASSERT_EQUAL(0, tb_run(&t, "abc", 3, 1));
    TEST_ASSERT_TRUE(t.dropped);
}

/* ------------------------------------------------------------ synthetic screens (M2-M6) */
/* A 100-column screen: rows of text at given rows, an input box (rule, "❯", rule) and a footer under it. */
static size_t synth(char *buf, const char *const *rows, int n, bool box, int box_row)
{
    size_t o = (size_t)sprintf(buf, "\x1b[2J\x1b[H");
    for (int i = 0; i < n; i++)
        if (rows[i]) o += (size_t)sprintf(buf + o, "\x1b[%d;1H%s", i + 1, rows[i]);
    if (box) {
        o += (size_t)sprintf(buf + o, "\x1b[%d;1H", box_row + 1);
        for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
        o += (size_t)sprintf(buf + o, "\x1b[%d;1H" SEL " ", box_row + 2);
        o += (size_t)sprintf(buf + o, "\x1b[%d;1H", box_row + 3);
        for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
        o += (size_t)sprintf(buf + o, "\x1b[%d;3H12k tokens | Opus\x1b[%d;3Hauto mode on (shift+tab to cycle)", box_row + 4, box_row + 5);
    }
    return o;
}

static char s_line[400];
static const char *full_line(void)   /* "  " + words to column 98 of 100 */
{
    strcpy(s_line, "  ");
    while (strlen(s_line) < 92) strcat(s_line, "word ");
    strcat(s_line, "endof");   /* 97 cells */
    return s_line;
}

/* M2: a full-width row followed by a list item, a bullet or a marker isn't joined; a continuation is. */
static void test_join_rules(void)
{
    char buf[8192];
    const char *rows[] = { NULL, full_line(), "  1. Apple", "  2. Pear", NULL, full_line(), "  - a bullet",
                           NULL, full_line(), "  continued here" };
    size_t n = synth(buf, rows, 10, true, 14);
    TEST_ASSERT_GREATER_THAN(0, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    has("endof\n  1. Apple\n  2. Pear\n");
    has("endof\n  - a bullet\n");
    has("endof continued here");
    /* the reply ending in its list: the options line sees both */
    const char *rows2[] = { NULL, DOT " Pick one, said in a sentence long enough to fill the whole row of this screen: here",
                            "  1. Apple", "  2. Pear" };
    n = synth(buf, rows2, 4, true, 10);
    TEST_ASSERT_GREATER_THAN(0, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL(2, sm_tail_options(s_out));
}

/* M2: a CJK paragraph the TUI wrapped joins without a space. */
static void test_join_cjk(void)
{
    char buf[8192], a[400] = "  ", b[64];
    for (int i = 0; i < 48; i++) strcat(a, "\xe6\x97\xa5");   /* 日 x 48 = 96 cells + 2 */
    snprintf(b, sizeof(b), "  \xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82");   /* 本語。 */
    const char *rows[] = { NULL, a, b };
    size_t n = synth(buf, rows, 3, true, 6);
    TEST_ASSERT_EQUAL(1, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    has("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82");   /* 日本語。 */
}

/* M3: a draft taller than the old 16-row window still finds the input box; its rows don't show. */
static void test_tall_draft(void)
{
    char buf[16384];
    size_t o = synth(buf, (const char *const[]){ NULL, DOT " The reply above the draft." }, 2, false, 0);
    o += (size_t)sprintf(buf + o, "\x1b[5;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[6;1H" SEL " draft line 0");
    for (int r = 7; r < 30; r++) o += (size_t)sprintf(buf + o, "\x1b[%d;3Hdraft line %d", r, r - 6);
    o += (size_t)sprintf(buf + o, "\x1b[30;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[31;3H12k tokens | Opus\x1b[32;3Hauto mode on");
    TEST_ASSERT_EQUAL(1, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " The reply above the draft.", s_out);
}

/* M3: no input box and no dialog: a rule near the bottom ends the view; with no such rule, no layout. */
static void test_no_box(void)
{
    char buf[8192];
    size_t o = synth(buf, (const char *const[]){ NULL, DOT " A reply.", NULL, "  More of it." }, 4, false, 0);
    o += (size_t)sprintf(buf + o, "\x1b[10;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[11;1H> a changed prompt glyph\x1b[12;3H12k tokens | Opus");
    TEST_ASSERT_EQUAL(2, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " A reply.\n  More of it.", s_out);
    o = synth(buf, (const char *const[]){ NULL, DOT " A reply.", "  12k tokens | Opus" }, 3, false, 0);
    TEST_ASSERT_EQUAL(CC_LV_NO_LAYOUT, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    /* a selector whose "❯ 1." sits right under its own frame rule is a dialog, not an input box */
    o = synth(buf, (const char *const[]){ NULL, DOT " Asking.", "Pick a fruit?" }, 3, false, 0);
    o += (size_t)sprintf(buf + o, "\x1b[4;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[5;1H" SEL " 1. Apple\x1b[6;3H2. Pear\x1b[7;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[8;3H    Esc to cancel");
    TEST_ASSERT_EQUAL(5, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    has("Pick a fruit?\n" SEL " 1. Apple\n  2. Pear\n      Esc to cancel");   /* the indented hint stays */
    TEST_ASSERT_EQUAL(0, sm_tail_options(s_out));
}

/* M4: a reply's "* ", "· " or "✻ " rows and indented rows that don't reach the right edge survive. */
static void test_lookalikes_survive(void)
{
    char buf[8192];
    const char *rows[] = { NULL, "* Item one", "\xc2\xb7 a middle dot note", "\xe2\x9c\xbb Starred thought here",
                           "\xe2\x9c\xbb Baked for 5s \xc2\xb7 done 8:29 PM", "\xc2\xb7 Computing\xe2\x80\xa6 (3s \xc2\xb7 esc to interrupt)",
                           "                                        centred code, column 40" };
    size_t n = synth(buf, rows, 7, true, 10);
    TEST_ASSERT_EQUAL(4, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING("* Item one\n\xc2\xb7 a middle dot note\n\xe2\x9c\xbb Starred thought here\n"
                             "                                        centred code, column 40", s_out);
}

/* ------------------------------------------------------------ chrome vs prose (busy-pane review round 1) */
/* Ordinary lines that look a little like chrome: the review's probe. The pane must keep every one. */
static const char *const s_prose[] = {
    "The model has a 200k tokens window", "Wait\xe2\x80\xa6 that's wrong", "Hmm\xe2\x80\xa6 let me check", "(1) Install it",
    "The server is still running on 8080", "I thought for a moment", "Press ctrl+b to bold", "Turn dark mode on",
    "Spawned 3 agents in parallel", "The pre-commit hook is running lint", "Retry (wait for 5 seconds)", "Please wait\xe2\x80\xa6",
    "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 src", "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 README.md", "3.14159", "});",
    "Press \xe2\x86\x91 to recall", "Tip: use git", "\xe5\x8f\xb2\xe5\xaf\x86\xe6\x96\xaf\xc2\xb7\xe7\xba\xa6\xe7\xbf\xb0",
    "\xe8\x80\x83\xe3\x81\x88\xe4\xb8\xad\xe2\x80\xa6", "\xe6\x97\xa5\xe6\x9c\xac(\xe4\xbe\x8b)", "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 40% done",
    "* Pay for 3 items", "OK", "thinking", "Loading data\xe2\x80\xa6", "1. Use 2 shells", "Thinking\xe2\x80\xa6", "The webhook is running",
    "Done (took 3s)", "Step 2 (of 5)", "2 agents finished", "20 tokens", "I'll keep thinking", "120", "$ 1,234", DOT " Done.",
    "Use auto mode for that", "1) First", "Run it (for 2 minutes)", "Wrote 12 lines to a.c", "Edited (3 files)",
    "\xe4\xbd\xa0\xe5\xa5\xbd\xe2\x80\xa6", "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\x90\x97", "(ctrl+c stops the server)",
    "Error: prompt is too long: 210000 tokens > 200000 maximum", "API Error: usage limit reached",
    "error: Claude Code v2.1 requires a newer Node", "esc to interrupt the build, then retry",
};
#define N_PROSE (sizeof(s_prose) / sizeof(s_prose[0]))

/* Chrome as the TUI's redraws leave it in the escape-stripped tail: each goes. */
static const char *const s_chrome_lines[] = {
    "\xe2\x9c\xbd Swirling\xe2\x80\xa6 (12s \xc2\xb7 \xe2\x86\x93 3k tokens)", "Swirling\xe2\x80\xa6 (12s \xc2\xb7 \xe2\x86\x93 3k tokens)",
    "\xc2\xb7 Computing\xe2\x80\xa6", "\xe2\x9c\xbb Baked for 5s", "\xe2\x9c\xbb Worked for 2m 6s \xc2\xb7 1 shell still running",
    "\xe2\x9c\xb3 n (2s \xc2\xb7 thinking with medium effort)", "\xe2\x9c\xa2 5 thinking", "\xe2\x9c\xb3 313",
    "a n thinking with medium effort", "thinking with medium effort", "10s \xc2\xb7 11 lines)", "51s \xc2\xb7 412 words)",
    "12s \xc2\xb7 \xe2\x86\x93 312 tokens)", "2 0s \xc2\xb7 1 8 lines)", "5 thinking)",
    "12k tokens | Opus 5.5", "46k tokens | [\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91] 7% reset | 51% 3d 14h weekly reset",
    "\xe2\x8f\xb5\xe2\x8f\xb5 auto mode on (shift+tab to cycle) \xc2\xb7 2 agents", "\xe2\x8f\xb8 manual mode on \xc2\xb7 \xe2\x86\x90 for agents",
    "auto mode on (shift+tab to cycle)", "? for shortcuts", "Checking for updates\xe2\x80\xa6", "running stop hooks\xe2\x80\xa6 1/2",
    "UserPromptSubmit hook \xc2\xb7 0s)", "SessionStart:startup hook succeeded",
    "\xe2\x8e\xbf  Tip: Use /statusline to set up a custom status line", "\xe2\x97\x90 medium \xc2\xb7 /effort",
    "You've used 89% of your weekly limit \xc2\xb7 resets Oct 27 at 7pm (Region/Example_Zone)", "(ctrl+b to run in background)",
    "(ctrl+b to  run in background)", "\xe2\x96\x90\xe2\x96\x9b\xe2\x96\x88\xe2\x96\x88 Claude Code v2.1.310",
    "\xe2\x96\x9d\xe2\x96\x9c\xe2\x96\x88\xe2\x96\x88 Opus 5.5 with medium effort \xc2\xb7 Plan label",
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 companion-test-busy \xe2\x94\x80", "/rc", "3  12s\xe2\x80\xa6", "\xc2\xb7 4",
};
#define N_CHROME (sizeof(s_chrome_lines) / sizeof(s_chrome_lines[0]))

static void test_fallback_chrome_shapes(void)
{
    char buf[512];
    for (size_t i = 0; i < N_CHROME; i++) {
        snprintf(buf, sizeof(buf), "%s\r\n", s_chrome_lines[i]);
        if (cc_logview_fallback(buf, 40) != 0) { printf("kept chrome: %s\n", s_chrome_lines[i]); TEST_FAIL(); }
        TEST_ASSERT_EQUAL_STRING("", buf);
    }
    /* chrome around the conversation: only the conversation stays, in order */
    static char all[16384];
    size_t o = 0;
    for (size_t i = 0; i < N_CHROME; i++) {
        o += (size_t)sprintf(all + o, "%s\r\n", s_chrome_lines[i]);
        if (i == 3) o += (size_t)sprintf(all + o, "What's your favorite color?\r\n");
        if (i == 20) o += (size_t)sprintf(all + o, "Ran 2 shell commands\r\n");
    }
    sprintf(all + o, DOT " Done. The color is blue.\r\n");
    TEST_ASSERT_EQUAL(3, cc_logview_fallback(all, 40));
    TEST_ASSERT_EQUAL_STRING("What's your favorite color?\nRan 2 shell commands\n" DOT " Done. The color is blue.", all);
    /* a bare verb the spinner redraws again and again goes; a reply's one "Thinking…" stays */
    char rep[] = "Deliberating\xe2\x80\xa6\r\nThinking\xe2\x80\xa6\r\nDeliberating\xe2\x80\xa6\r\nI checked it.\r\nDeliberating\xe2\x80\xa6\r\n";
    TEST_ASSERT_EQUAL(2, cc_logview_fallback(rep, 40));
    TEST_ASSERT_EQUAL_STRING("Thinking\xe2\x80\xa6\nI checked it.", rep);
}

/* M5: prose survives the fallback (as cc_log_tail keeps it), the chrome matcher, and the pane on an idle screen
   and on a busy sparse one. */
static void test_prose_survives(void)
{
    char buf[512];
    int kept = 0;
    for (size_t i = 0; i < N_PROSE; i++) {
        TEST_ASSERT_FALSE_MESSAGE(cc_logview_chrome_line(s_prose[i]), s_prose[i]);
        snprintf(buf, sizeof(buf), "%s\r\n", s_prose[i]);
        char plain[512];
        snprintf(plain, sizeof(plain), "%s\r\n", s_prose[i]);
        int base = cc_log_tail(plain, 40);   /* what the plain heuristic keeps (it drops two CJK characters alone) */
        int got = cc_logview_fallback(buf, 40);
        if (got != base) { printf("fallback dropped: %s\n", s_prose[i]); TEST_FAIL(); }
        kept += got;
    }
    printf("prose kept by the fallback: %d of %d\n", kept, (int)N_PROSE);
    TEST_ASSERT_EQUAL((int)N_PROSE, kept);
    /* the pane: an idle screen with its input box, then the same rows on a busy sparse screen */
    static char screen[16384];
    const char *rows[N_PROSE + 1];
    rows[0] = NULL;
    for (size_t i = 0; i < N_PROSE; i++) rows[i + 1] = s_prose[i];
    for (int sparse = 0; sparse < 2; sparse++) {
        size_t n = synth(screen, rows, (int)N_PROSE + 1, !sparse, (int)N_PROSE + 2);
        if (sparse) {   /* no clear, a spinner under the rows and the cursor parked below it */
            memmove(screen, screen + 7, n - 7);
            n -= 7;
            n += (size_t)sprintf(screen + n, "\x1b[%d;1H\xe2\x9c\xa2 Actualizing\xe2\x80\xa6\x1b[%d;1H\x1b[%d;3H",
                                 (int)N_PROSE + 3, (int)N_PROSE + 9, (int)N_PROSE + 6);
        }
        TEST_ASSERT_EQUAL_MESSAGE((int)N_PROSE, cc_logview_tail(screen, n, true, 80, s_out, sizeof(s_out)),
                                  sparse ? "sparse" : "idle");
        for (size_t i = 0; i < N_PROSE; i++) has(s_prose[i]);
    }
}

/* A sparse screen of rows that open with a busy screen's buffer (the spinner and the parked cursor appended). */
static size_t sparse_screen(char *buf, const char *body, int spin_row)
{
    return (size_t)sprintf(buf, "%s\x1b[%d;1H\xe2\x9c\xa2 Actualizing\xe2\x80\xa6\x1b[%d;1H\x1b[%d;3H", body, spin_row,
                           spin_row + 6, spin_row + 3);
}

/* M1-M3: row by row on a sparse screen: a stale counter goes wherever it starts; short rows, deep code and a reply's
   own "(ctrl+…)" stay; the TUI's own hint goes (on an idle screen too). */
static void test_sparse_rows(void)
{
    char buf[4096];
    size_t n = sparse_screen(buf, "\x1b[30;3HHello world\x1b[31;17H12s \xc2\xb7 \xe2\x86\x93 312 tokens)\x1b[32;5H51s \xc2\xb7 412 words)"
                                  "\x1b[33;3H1. A\x1b[34;3H2. B\x1b[35;3Hx = 1\x1b[36;3H42\x1b[37;3Hif a:\x1b[38;29Hdeep_call(arg)"
                                  "\x1b[39;3H(ctrl+c stops the server)\x1b[40;6H(ctrl+b to run in background)", 42);
    TEST_ASSERT_EQUAL(8, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING("  Hello world\n  1. A\n  2. B\n  x = 1\n  42\n  if a:\n                            deep_call(arg)\n"
                             "  (ctrl+c stops the server)", s_out);
    const char *rows[] = { NULL, DOT " Started it.", "  (ctrl+c stops the server)", "     (ctrl+b to run in background)" };
    n = synth(buf, rows, 4, true, 6);
    TEST_ASSERT_EQUAL(2, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " Started it.\n  (ctrl+c stops the server)", s_out);
}

/* I3: a sparse screen has no rule to give the width: a table and a code block stay row by row; a paragraph the TUI
   wrapped (the row above running to the widest row, the next starting lowercase at its text column) still joins. */
static void test_sparse_join(void)
{
    char buf[8192];
    size_t n = sparse_screen(buf, "\x1b[30;3H\xe2\x94\x82 Name  \xe2\x94\x82 Age \xe2\x94\x82\x1b[31;3H\xe2\x94\x82 Alice \xe2\x94\x82 30  \xe2\x94\x82"
                                  "\x1b[32;3H\xe2\x94\x82 Bob   \xe2\x94\x82 41  \xe2\x94\x82", 34);
    TEST_ASSERT_EQUAL(3, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING("  \xe2\x94\x82 Name  \xe2\x94\x82 Age \xe2\x94\x82\n  \xe2\x94\x82 Alice \xe2\x94\x82 30  \xe2\x94\x82\n"
                             "  \xe2\x94\x82 Bob   \xe2\x94\x82 41  \xe2\x94\x82", s_out);
    /* code: rows of equal length, wide enough to look wrapped */
    char body[4096];
    const char *code = "result_%c = compute(alpha, beta, gamma, delta, epsilon, zeta, eta, theta, iota, kappa, lambda)";
    size_t o = (size_t)sprintf(body, "\x1b[30;1H" DOT " Here:");
    for (int i = 0; i < 3; i++) { o += (size_t)sprintf(body + o, "\x1b[%d;3H", 31 + i); o += (size_t)sprintf(body + o, code, 'a' + i); }
    o += (size_t)sprintf(body + o, "\x1b[34;3Hdef f(x):\x1b[35;7Hreturn x + 1\x1b[36;7Hpass\x1b[37;3Hprint(f(2))");
    n = sparse_screen(buf, body, 39);
    TEST_ASSERT_EQUAL(8, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    has("\n  def f(x):\n      return x + 1\n      pass\n  print(f(2))");
    /* a wrapped paragraph: one line */
    char para[400] = DOT " Tides";
    while (strlen(para) < 140) strcat(para, " begin");
    n = (size_t)sprintf(body, "\x1b[30;1H%s\x1b[31;3Hwith gravity, mostly the Moon's.\x1b[32;3HA new sentence here.", para);
    n = sparse_screen(buf, body, 34);
    TEST_ASSERT_EQUAL(2, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    has(" begin with gravity, mostly the Moon's.\n  A new sentence here.");
}

/* M4: the anchor when the stream ends mid-frame (cursor above the spinner) and with a tall todo list under it. */
static void test_sparse_anchor(void)
{
    char buf[4096];
    size_t n = sparse_screen(buf, "\x1b[30;1H" DOT " Working on it.", 43);
    n += (size_t)sprintf(buf + n, "\x1b[H\r\x1b[8B");   /* a frame cut after its first move */
    TEST_ASSERT_EQUAL(1, cc_logview_tail(buf, n, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " Working on it.", s_out);
    size_t o = (size_t)sprintf(buf, "\x1b[5;1H" DOT " Planning.\x1b[8;1H\xe2\x9c\xa2 Actualizing\xe2\x80\xa6");
    for (int i = 0; i < 16; i++) o += (size_t)sprintf(buf + o, "\x1b[%d;3H\xe2\x8e\xbf  \xe2\x98\x90 Task number %d", 9 + i, i);
    o += (size_t)sprintf(buf + o, "\x1b[33;1H\x1b[30;3H");
    TEST_ASSERT_EQUAL(1, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " Planning.", s_out);
}

/* M5: a recording wider than 312 columns or taller than 160 rows is refused, not clamped. */
static void test_too_big(void)
{
    char buf[8192];
    size_t o = (size_t)sprintf(buf, "\x1b[2J\x1b[H\x1b[2;1H");
    for (int i = 0; i < 313; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    TEST_ASSERT_FALSE(cc_logview_size(buf, o, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(CC_LV_TOO_BIG, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    o = (size_t)sprintf(buf, "\x1b[2J\x1b[H\x1b[161;1Hx");
    TEST_ASSERT_EQUAL(CC_LV_TOO_BIG, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    o = (size_t)sprintf(buf, "\x1b[2J\x1b[H\x1b[160;312Hx");
    TEST_ASSERT_TRUE(cc_logview_size(buf, o, NULL, NULL, NULL, NULL));
}

/* M6: a stream of scrolls costs a screen per sequence: the replay stops at its budget. Real streams are far below. */
static void test_replay_budget(void)
{
    size_t o = (size_t)sprintf(s_big, "\x1b[2J\x1b[H\x1b[160;300H");
    while (o + 8 < 512 * 1024) o += (size_t)sprintf(s_big + o, "\x1b[9S");
    TEST_ASSERT_EQUAL(CC_LV_TOO_COSTLY, cc_logview_tail(s_big, o, true, 40, s_out, sizeof(s_out)));
    o = (size_t)sprintf(s_big, "\x1b[2J\x1b[H\x1b[160;300H");
    while (o + 8 < 512 * 1024) o += (size_t)sprintf(s_big + o, "\x1b[H\x1b[J");   /* erase below, from home */
    TEST_ASSERT_EQUAL(CC_LV_TOO_COSTLY, cc_logview_tail(s_big, o, true, 40, s_out, sizeof(s_out)));
    static const char *fx[] = { "logs_short.bin", "logs_long.bin", "logs_overflow.bin", "logs_perm.bin" };
    for (size_t i = 0; i < 4; i++) {
        size_t n = load(fx[i]);
        int cols, rows;
        size_t st;
        cc_logview_size(s_raw, n, &cols, &rows, &st, NULL);
        vt_t *vt = vt_create(cols, rows, 0);
        vt_feed(vt, (const uint8_t *)s_raw + st, n - st);
        TEST_ASSERT_TRUE(vt_work(vt) < 6 * (n - st));   /* at most 5 units a byte on the live captures */
        vt_destroy(vt);
    }
}

/* M7: the alternate screen is made on its first use; a resize keeps it; both still work. */
static void test_vt_lazy_alt(void)
{
    vt_t *vt = vt_create(20, 4, 0);
    TEST_ASSERT_NOT_NULL(vt);
    char t[256];
    vt_feed(vt, (const uint8_t *)"main", 4);
    vt_feed(vt, (const uint8_t *)"\x1b[?1049h\x1b[Halt", 14);
    vt_screen_text(vt, t, sizeof(t));
    TEST_ASSERT_EQUAL(0, strncmp(t, "alt\n", 4));
    TEST_ASSERT_TRUE(vt_resize(vt, 30, 5));
    vt_feed(vt, (const uint8_t *)"\x1b[?1049l", 8);
    vt_screen_text(vt, t, sizeof(t));
    TEST_ASSERT_EQUAL(0, strncmp(t, "main\n", 5));
    TEST_ASSERT_FALSE(vt_resize(vt, 1, 1));   /* a bad size: the old screen stays */
    TEST_ASSERT_EQUAL(30, vt_cols(vt));
    TEST_ASSERT_FALSE(vt_resize(NULL, 30, 5));
    vt_destroy(vt);
}

/* N1: a draft that starts with a number is still the input box: it doesn't show, and no picks line. */
static void test_numbered_draft(void)
{
    char buf[8192];
    size_t o = synth(buf, (const char *const[]){ NULL, DOT " Tell me your list." }, 2, false, 0);
    o += (size_t)sprintf(buf + o, "\x1b[5;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[6;1H" SEL " 1. buy apples\x1b[7;3H2. buy pears\x1b[8;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[9;3H12k tokens | Opus\x1b[10;3Hauto mode on");
    TEST_ASSERT_EQUAL(1, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    TEST_ASSERT_EQUAL_STRING(DOT " Tell me your list.", s_out);
    TEST_ASSERT_EQUAL(0, sm_tail_options(s_out));
}

/* N2: with a dialog up, a rule right above an earlier "❯ prompt" row isn't taken for the input box. */
static void test_dialog_over_old_prompt(void)
{
    char buf[8192];
    size_t o = (size_t)sprintf(buf, "\x1b[2J\x1b[H\x1b[3;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[4;1H" SEL " an earlier prompt\x1b[6;1H" DOT " Running a command\x1b[8;1H");
    for (int i = 0; i < 100; i++) o += (size_t)sprintf(buf + o, "\xe2\x94\x80");
    o += (size_t)sprintf(buf + o, "\x1b[9;2HDo you want to proceed?\x1b[10;2H" SEL " 1. Yes\x1b[11;4H2. No\x1b[13;2HEsc to cancel");
    TEST_ASSERT_EQUAL(6, cc_logview_tail(buf, o, true, 40, s_out, sizeof(s_out)));
    has(SEL " an earlier prompt\n" DOT " Running a command\n Do you want to proceed?\n " SEL " 1. Yes\n   2. No\n Esc to cancel");
}

/* N3: sequences that cost a screen or a line each (DECALN, tab moves clamped to the width, scrolls) stop at the
   budget, and fast: the check is inside vt_feed, not per 4 KB chunk. */
static void test_replay_budget_sequences(void)
{
    static const char *seqs[] = { "\x1b#8", "\x1b[32767I", "\x1b[32767Z", "\x1b[S" };
    for (size_t i = 0; i < 4; i++) {
        size_t o = (size_t)sprintf(s_big, "\x1b[2J\x1b[H\x1b[160;300H\x1b[80;150H");
        size_t k = strlen(seqs[i]);
        while (o + k < 512 * 1024) { memcpy(s_big + o, seqs[i], k); o += k; }
        clock_t t0 = clock();
        int r = cc_logview_tail(s_big, o, true, 40, s_out, sizeof(s_out));
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("budget %zu: %d in %.1f ms\n", i, r, ms);
        TEST_ASSERT_EQUAL(CC_LV_TOO_COSTLY, r);
        TEST_ASSERT_TRUE(ms < 100.0);
    }
    /* the overshoot is at most one sequence: a single 4 KB chunk of scrolls stops within a screen of the budget */
    vt_t *vt = vt_create(308, 160, 0);
    size_t o = 0;
    while (o + 3 < 4096) { memcpy(s_big + o, "\x1b[S", 3); o += 3; }
    vt_set_work_limit(vt, CC_LOGVIEW_WORK_MAX);
    for (int i = 0; i < 64; i++) vt_feed(vt, (const uint8_t *)s_big, o);
    TEST_ASSERT_TRUE(vt_work(vt) > CC_LOGVIEW_WORK_MAX);
    TEST_ASSERT_TRUE(vt_work(vt) <= CC_LOGVIEW_WORK_MAX + 2 * 308 * 160);
    vt_destroy(vt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_size);
    RUN_TEST(test_short_reply);
    RUN_TEST(test_fallback_drops_chrome);
    RUN_TEST(test_fallback_chrome_shapes);
    RUN_TEST(test_prose_survives);
    RUN_TEST(test_sparse_rows);
    RUN_TEST(test_sparse_join);
    RUN_TEST(test_sparse_anchor);
    RUN_TEST(test_long_reply);
    RUN_TEST(test_tool_call);
    RUN_TEST(test_numbered_question);
    RUN_TEST(test_ask_user_question);
    RUN_TEST(test_permission_dialog);
    RUN_TEST(test_overflow);
    RUN_TEST(test_busy);
    RUN_TEST(test_working);
    RUN_TEST(test_working_spinner_only);
    RUN_TEST(test_update_check_hidden);
    RUN_TEST(test_partial);
    RUN_TEST(test_cap_and_args);
    RUN_TEST(test_tailbuf_padding_before_clear);
    RUN_TEST(test_tailbuf_clear_then_overflow);
    RUN_TEST(test_tailbuf_no_clear);
    RUN_TEST(test_join_rules);
    RUN_TEST(test_join_cjk);
    RUN_TEST(test_tall_draft);
    RUN_TEST(test_no_box);
    RUN_TEST(test_lookalikes_survive);
    RUN_TEST(test_too_big);
    RUN_TEST(test_replay_budget);
    RUN_TEST(test_vt_lazy_alt);
    RUN_TEST(test_numbered_draft);
    RUN_TEST(test_dialog_over_old_prompt);
    RUN_TEST(test_replay_budget_sequences);
    return UNITY_END();
}
