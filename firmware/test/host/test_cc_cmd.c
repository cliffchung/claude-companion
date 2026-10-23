#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "cc_cmd.h"

#define CL "~/.local/bin/claude"
#define SID "22222222-2222-4222-8222-222222222222"

void setUp(void) {}
void tearDown(void) {}

static void test_shell_quote_roundtrip(void)
{
    const char *cases[] = { "yes", "it's fine", "$(touch /tmp/cc-pwned) `id` \"q\"",
                            "line1\nline2", "back\\slash" };
    unlink("/tmp/cc-pwned");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char q[256], cmd[400], got[256] = {0};
        TEST_ASSERT_TRUE(cc_shell_quote(cases[i], q, sizeof(q)));
        snprintf(cmd, sizeof(cmd), "printf %%s %s", q);
        FILE *f = popen(cmd, "r"); /* popen runs it through /bin/sh -c */
        TEST_ASSERT_NOT_NULL(f);
        size_t n = fread(got, 1, sizeof(got) - 1, f);
        pclose(f);
        got[n] = 0;
        TEST_ASSERT_EQUAL_STRING(cases[i], got);
        /* the user's login shell runs SSH exec commands: repeat under zsh */
        if (access("/bin/zsh", X_OK) == 0) {
            char zcmd[600], zq[512];
            TEST_ASSERT_TRUE(cc_shell_quote(cmd, zq, sizeof(zq)));
            snprintf(zcmd, sizeof(zcmd), "/bin/zsh -c %s", zq);
            memset(got, 0, sizeof(got));
            f = popen(zcmd, "r");
            TEST_ASSERT_NOT_NULL(f);
            n = fread(got, 1, sizeof(got) - 1, f);
            pclose(f);
            got[n] = 0;
            TEST_ASSERT_EQUAL_STRING(cases[i], got);
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, access("/tmp/cc-pwned", F_OK));
}

static void test_cmd_reply_exact(void)
{
    char o[256];
    TEST_ASSERT_TRUE(cc_cmd_reply(CL, SID, "it's ok", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " -p --resume " SID " -- 'it'\\''s ok'", o);
}

static void test_cmd_reply_leading_dash(void)
{
    char o[256];
    TEST_ASSERT_TRUE(cc_cmd_reply(CL, SID, "--help me", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " -p --resume " SID " -- '--help me'", o);
    TEST_ASSERT_TRUE(cc_cmd_reply(CL, SID, "- fix tests", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " -p --resume " SID " -- '- fix tests'", o);
}

static void test_quote_clears_out_on_failure(void)
{
    char o[8] = "xxxxxxx";
    TEST_ASSERT_FALSE(cc_shell_quote("far too long for the buffer", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("", o);
    char r[80] = "junk";
    TEST_ASSERT_FALSE(cc_cmd_reply(CL, SID, "text that will not fit in eighty bytes of output buffer at all, really not", r, sizeof(r)));
    TEST_ASSERT_EQUAL_STRING("", r);
}

static void test_cmd_exact(void)
{
    char o[256];
    TEST_ASSERT_TRUE(cc_cmd_agents(CL, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " agents --json --all", o);
    TEST_ASSERT_TRUE(cc_cmd_attach(CL, "aaaaaaa2", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " attach aaaaaaa2", o);
    TEST_ASSERT_TRUE(cc_cmd_logs(CL, "aaaaaaa2", o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(CL " logs aaaaaaa2 | tail -n 40", o);
}

static void test_cmd_rejects(void)
{
    char o[256], small[40];
    TEST_ASSERT_FALSE(cc_cmd_reply(CL, "not-a-uuid", "hi", o, sizeof(o)));
    TEST_ASSERT_FALSE(cc_cmd_reply(CL, SID, "", o, sizeof(o)));
    TEST_ASSERT_FALSE(cc_cmd_agents("~/bin/claude;rm", o, sizeof(o)));
    TEST_ASSERT_FALSE(cc_cmd_agents("my claude", o, sizeof(o)));
    TEST_ASSERT_FALSE(cc_cmd_attach(CL, "AAAAAAA2", o, sizeof(o)));
    /* exact-fit boundary: needs strlen+1 bytes; one byte less must fail */
    char want[256];
    cc_cmd_attach(CL, "aaaaaaa2", want, sizeof(want));
    size_t need = strlen(want) + 1;
    char buf[256];
    TEST_ASSERT_TRUE(cc_cmd_attach(CL, "aaaaaaa2", buf, need));
    TEST_ASSERT_FALSE(cc_cmd_attach(CL, "aaaaaaa2", buf, need - 1));
    TEST_ASSERT_FALSE(cc_cmd_attach(CL, "aaaaaaa2", small, 10));
    TEST_ASSERT_FALSE(cc_cmd_reply(CL, SID, "hello world long text", small, sizeof(small)));
}

/* verified against this Mac's /bin/sh (bash 3.2) by firmware/tools/test_watch_loop.py */
#define WATCH_EXACT "exec /bin/sh -c 'stty -echo 2>/dev/null; C=~/.local/bin/claude; I=3; H=60; T=60; L=1800; echo \"@@CC-WATCH 1\"; b() { echo \"@@CC-BYE $1\" & sleep 0.5; kill -- -$$ 2>/dev/null || kill $$; exit; }; { while :; do j=$SECONDS; if read -r -t \"$T\" x; then :; elif [ $((SECONDS-j)) -lt \"$T\" ]; then b eof; else b quiet; fi; done; } <&0 & p=; s=0; while [ \"$SECONDS\" -lt \"$L\" ]; do o=$(\"$C\" agents --json --all </dev/null 2>&1); r=$?; if [ \"$o\" != \"$p\" ]; then echo \"@@CC-BEGIN $r\"; printf \"%s\\n\" \"$o\"; echo \"@@CC-END\"; p=$o; s=$SECONDS; elif [ $((SECONDS-s)) -ge \"$H\" ]; then echo \"@@CC-HB $SECONDS\"; s=$SECONDS; fi; sleep \"$I\"; done; b life'"

static void test_cmd_watch_exact(void)
{
    char o[2048];
    cc_watch_params_t p = { 3, 60, 60, 1800 };
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &p, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING(WATCH_EXACT, o);
}

static void test_cmd_watch_rejects(void)
{
    char o[2048];
    cc_watch_params_t ok = { 3, 60, 60, 1800 };
    TEST_ASSERT_FALSE(cc_cmd_watch("my claude", &ok, o, sizeof(o)));
    TEST_ASSERT_FALSE(cc_cmd_watch(CL, NULL, o, sizeof(o)));
    cc_watch_params_t bad[] = {
        { 0, 60, 60, 1800 }, { 121, 60, 60, 1800 },
        { 3, 0, 60, 1800 }, { 3, 601, 60, 1800 },
        { 3, 60, 1, 1800 }, { 3, 60, 601, 1800 },
        { 3, 60, 60, 4 }, { 3, 60, 60, 7201 },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_FALSE(cc_cmd_watch(CL, &bad[i], o, sizeof(o)));
    /* the range ends are accepted */
    cc_watch_params_t lo = { 1, 1, 2, 5 }, hi = { 120, 600, 600, 7200 };
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &lo, o, sizeof(o)));
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &hi, o, sizeof(o)));
    /* exact fit, then one byte too small */
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &ok, o, sizeof(o)));
    size_t need = strlen(o) + 1;
    char *b = malloc(need);
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &ok, b, need));
    TEST_ASSERT_FALSE(cc_cmd_watch(CL, &ok, b, need - 1));
    free(b);
    /* only the two quotes that delimit the sh -c argument */
    TEST_ASSERT_TRUE(cc_cmd_watch(CL, &ok, o, sizeof(o)));
    int q = 0;
    for (const char *c = o; *c; c++) q += *c == '\'';
    TEST_ASSERT_EQUAL_INT(2, q);
    TEST_ASSERT_EQUAL_CHAR('\'', o[strlen("exec /bin/sh -c ")]);
    TEST_ASSERT_EQUAL_CHAR('\'', o[strlen(o) - 1]);
}

static void test_strip_ansi(void)
{
    char s[] = "\x1b[1;32m\xe2\x9c\x93 done\x1b[0m\x1b]0;title\x07";
    size_t n = cc_strip_ansi(s);
    TEST_ASSERT_EQUAL_STRING("\xe2\x9c\x93 done", s);
    TEST_ASSERT_EQUAL_UINT(strlen(s), n);
}


static void test_log_tail_cursor_moves(void)
{
    /* cursor-forward becomes spaces; cursor positioning starts a new line */
    char s[] = "Hello!\x1b[1CI'm\x1b[3Chere\x1b[2;1Hnext\x1b[Cline";
    int n = cc_log_tail(s, 6);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("Hello! I'm   here\nnext line", s);
    /* real `claude logs`: words placed with cursor-to-column */
    char t[] = "\x1b[H\r\x1b[2C\x1b[8BWhat's\x1b[10Gyour\x1b[15Gfavorite\x1b[24Gcolor?\x1b[50;1H";
    TEST_ASSERT_EQUAL_INT(1, cc_log_tail(t, 6));
    TEST_ASSERT_EQUAL_STRING("What's your favorite color?", t);
}

static void test_log_tail_drops_rules_and_blanks(void)
{
    char s[] = "\x1b[38;5;1mred\x1b[0m\r\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\r\n   \r\n"
               "\xc2\xa0\xe2\x95\xad\xe2\x94\x80\r\n  \xe2\x9c\xbb thinking  \r\x1b]0;t\x07" "done";
    int n = cc_log_tail(s, 6);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("red\n\xe2\x9c\xbb thinking\ndone", s);
}

static void test_log_tail_drops_spinner_fragments(void)
{
    /* spinner redraws leave lines of single letters behind */
    char s[] = "\xc2\xb7 c b\r\ns m\r\n\xe2\x9c\xbb 57\r\nWhat's your favorite color?\r\n\xe2\x9d\xaf\r\n1. Amber\r\n";
    TEST_ASSERT_EQUAL_INT(2, cc_log_tail(s, 6));
    TEST_ASSERT_EQUAL_STRING("What's your favorite color?\n1. Amber", s);
}

static void test_log_tail_keeps_short_words(void)
{
    /* two-letter words are real output; lone letters and bare numbers are spinner leftovers */
    char s[] = "OK\r\nNo\r\n\xe2\x9c\xbb 57\r\nD c\r\nHi there\r\n";
    TEST_ASSERT_EQUAL_INT(3, cc_log_tail(s, 6));
    TEST_ASSERT_EQUAL_STRING("OK\nNo\nHi there", s);
}

static void test_log_tail_keeps_last(void)
{
    char s[] = "l01\nl02\nl03\nl04\nl05\nl06\nl07\nl08\n";
    TEST_ASSERT_EQUAL_INT(6, cc_log_tail(s, 6));
    TEST_ASSERT_EQUAL_STRING("l03\nl04\nl05\nl06\nl07\nl08", s);
    char e[] = "";
    TEST_ASSERT_EQUAL_INT(0, cc_log_tail(e, 6));
    TEST_ASSERT_EQUAL_STRING("", e);
    TEST_ASSERT_EQUAL_INT(0, cc_log_tail(NULL, 6));
}

#define PS "\x1b[200~"
#define PE "\x1b[201~"

static void test_paste_build_exact(void)
{
    char o[64];
    memset(o, 'x', sizeof(o));
    size_t n = cc_paste_build("blue", o, sizeof(o));
    const char want[] = PS "blue" PE "\r";
    TEST_ASSERT_EQUAL_UINT(sizeof(want) - 1, n);
    TEST_ASSERT_EQUAL_MEMORY(want, o, n);
    TEST_ASSERT_EQUAL_UINT8(0, o[n]);
    /* no Ctrl+U or Ctrl+Z: cc_link sends those separately */
    TEST_ASSERT_NULL(memchr(o, 0x15, n));
    TEST_ASSERT_NULL(memchr(o, 0x1a, n));
}

static void test_paste_build_strips_esc(void)
{
    char o[64];
    size_t n = cc_paste_build("a\x1b[201~b", o, sizeof(o));
    const char want[] = PS "a[201~b" PE "\r";
    TEST_ASSERT_EQUAL_UINT(sizeof(want) - 1, n);
    TEST_ASSERT_EQUAL_MEMORY(want, o, n);
    /* the only ESCs are the two paste markers */
    int esc = 0;
    for (size_t i = 0; i < n; i++) esc += (o[i] == 0x1b);
    TEST_ASSERT_EQUAL_INT(2, esc);
}

static void test_paste_build_controls(void)
{
    char o[64];
    size_t n = cc_paste_build("line1\nline2", o, sizeof(o));
    const char want[] = PS "line1\nline2" PE "\r";
    TEST_ASSERT_EQUAL_UINT(sizeof(want) - 1, n);
    TEST_ASSERT_EQUAL_MEMORY(want, o, n);
    /* CR, TAB, Ctrl+C/U/Z, BEL and DEL never reach the TUI; UTF-8 is kept */
    n = cc_paste_build("a\r\nb\tc\x03\x15\x1a\x07\x7f\xc3\xbc", o, sizeof(o));
    const char want2[] = PS "a\nbc\xc3\xbc" PE "\r";
    TEST_ASSERT_EQUAL_UINT(sizeof(want2) - 1, n);
    TEST_ASSERT_EQUAL_MEMORY(want2, o, n);
}

static void test_paste_build_rejects(void)
{
    char o[64];
    TEST_ASSERT_EQUAL_UINT(0, cc_paste_build("", o, sizeof(o)));
    TEST_ASSERT_EQUAL_UINT(0, cc_paste_build(NULL, o, sizeof(o)));
    TEST_ASSERT_EQUAL_UINT(0, cc_paste_build("\x1b\x1b\r", o, sizeof(o))); /* empty once sanitized */
    /* overflow: needs 6 + 4 + 6 + 1 bytes plus the NUL */
    TEST_ASSERT_EQUAL_UINT(17, cc_paste_build("blue", o, 18));
    o[0] = 'x';
    TEST_ASSERT_EQUAL_UINT(0, cc_paste_build("blue", o, 17));
    TEST_ASSERT_EQUAL_UINT8(0, o[0]);
    TEST_ASSERT_EQUAL_UINT(0, cc_paste_build("blue", o, 0));
}

/* Synthetic screens, ANSI already stripped. "\xe2\x9d\xaf" is the TUI's selection/input glyph,
   "\xe2\x94\x80" a frame rule. A dialog ends the screen with a numbered option list plus an esc hint;
   an input prompt ends with an empty "\xe2\x9d\xaf " line inside the input box. */
#define SEL "\xe2\x9d\xaf"
#define RULE "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
static const char PERM_SCREEN[] =
    "Bash command\n\n  curl -sI https://example.com\n  Fetch headers\n\n"
    "Do you want to proceed?\n" SEL " 1. Yes\n  2. Yes, and don't ask again for curl commands\n"
    "  3. No, and tell Claude what to do differently (esc)\n";
/* the same dialog the way the stripped attach stream really looks: no newlines, moved selection */
static const char PERM_SCREEN_FLAT[] =
    RULE " Edit file a.c " RULE "Do you want to make this edit to a.c?  1. Yes" SEL " 2. Yes, allow all edits"
    "  3. No  Esc to cancel \xc2\xb7 Tab to amend";
static const char INPUT_SCREEN[] =
    "\xe2\x8f\xba What's your favorite color?\n\n" RULE "\n" SEL " \n" RULE "\n  ? for shortcuts\n";
/* history that looks like a dialog, then the input box */
static const char INPUT_AFTER_QUESTION[] =
    SEL " Reply with exactly this line and nothing else: Do you want to proceed? 1. Yes"
    "\xe2\x8f\xba Do you want to proceed? 1. Yes  allow: GET, HEAD  (esc)" RULE " companion-test-guard " RULE
    SEL "\xc2\xa0\r" RULE RULE "\r\xe2\x8f\xb5\xe2\x8f\xb5 auto mode on (shift+tab to cycle)";   /* as in the real stream */
/* an answered dialog redrawn earlier in the stream, then the input box */
static const char INPUT_AFTER_DIALOG[] =
    "Do you want to proceed?\n" SEL " 1. Yes\n  2. No (esc)\n" "\xe2\x8f\xba Done.\n" RULE "\n" SEL " \n" RULE "\n";

/* Real attach output (2.1.309, ANSI intact, cwd anonymized) of a session waiting for input
   whose last reply was "Do you want to proceed? 1. Yes 2. No (esc)". */
static void test_screen_real_input_after_question(void)
{
    FILE *f = fopen(FIXTURE_DIR "/screen_input_after_question.bin", "rb");
    TEST_ASSERT_NOT_NULL(f);
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cc_strip_ansi(buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "Do you want to proceed?"));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(buf));
    TEST_ASSERT_TRUE(cc_screen_input_ready(buf));
}

/* The real renderer: lines end in \r and unstyled spaces are cursor moves, so they vanish. */
static const char REAL_DIALOG[] =
    "\xe2\x8f\xba" "Bashcommand\r curl-sIhttps://example.com\rDoyouwanttoproceed?\r"
    SEL "1.Yes\r2.Yes,anddon'taskagain\r3.No,andtellClaudewhattododifferently(esc)\rEsctocancel";
static const char SPINNER[] =
    "\xe2\x8f\xbaWriting the file\r\xe2\x9c\xbb" "Actualizing\xe2\x80\xa6(3s\xc2\xb7" "esctointerrupt)";

static void test_screen_input_ready(void)
{
    /* typing is allowed only when the last selection glyph is an empty input line */
    TEST_ASSERT_TRUE(cc_screen_input_ready(INPUT_SCREEN));
    TEST_ASSERT_TRUE(cc_screen_input_ready(INPUT_AFTER_QUESTION));
    TEST_ASSERT_TRUE(cc_screen_input_ready(INPUT_AFTER_DIALOG));
    TEST_ASSERT_TRUE(cc_screen_input_ready("x " SEL "\xc2\xa0\r" RULE "\r10k tokens"));
    /* fail closed: dialogs in any form, spinners, drafts, nothing */
    TEST_ASSERT_FALSE(cc_screen_input_ready(REAL_DIALOG));
    TEST_ASSERT_FALSE(cc_screen_input_ready(PERM_SCREEN));
    TEST_ASSERT_FALSE(cc_screen_input_ready(PERM_SCREEN_FLAT));
    TEST_ASSERT_FALSE(cc_screen_input_ready(SPINNER));
    TEST_ASSERT_FALSE(cc_screen_input_ready(SEL " half-typed draft\r"));
    TEST_ASSERT_FALSE(cc_screen_input_ready(""));
    TEST_ASSERT_FALSE(cc_screen_input_ready(NULL));
    /* the dialog matcher (secondary) also knows the real form */
    TEST_ASSERT_TRUE(cc_screen_is_permission_prompt(REAL_DIALOG));
}

static void test_screen_permission_prompt(void)
{
    TEST_ASSERT_TRUE(cc_screen_is_permission_prompt(PERM_SCREEN));
    TEST_ASSERT_TRUE(cc_screen_is_permission_prompt(PERM_SCREEN_FLAT));
}

static void test_screen_input_prompt_is_not_permission(void)
{
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(INPUT_SCREEN));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(INPUT_AFTER_QUESTION));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(INPUT_AFTER_DIALOG));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(""));
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(NULL));
    /* words alone are not a dialog */
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt("Do you want to proceed? Allow? 1. Yes 2. No"));
    /* a numbered list without an esc/cancel hint is not a dialog */
    TEST_ASSERT_FALSE(cc_screen_is_permission_prompt(SEL " 1. Apples\n  2. Pears\n"));
}

static void test_same_host(void)
{
    TEST_ASSERT_TRUE(cc_same_host("100.64.0.18", "100.64.0.18"));
    TEST_ASSERT_TRUE(cc_same_host("Mac.Tail1234.ts.net", "mac.tail1234.ts.net."));
    TEST_ASSERT_TRUE(cc_same_host(" mac ", "mac"));
    TEST_ASSERT_TRUE(cc_same_host("mac", "mac.tail1234.ts.net"));   /* MagicDNS short name */
    TEST_ASSERT_TRUE(cc_same_host("mac.tail1234.ts.net", "MAC"));
    TEST_ASSERT_FALSE(cc_same_host("mac", "macmini.tail1234.ts.net"));
    TEST_ASSERT_FALSE(cc_same_host("100", "100.64.0.18"));        /* not an IP's first byte */
    TEST_ASSERT_FALSE(cc_same_host("100.64.0.1", "100.64.0.18"));
    TEST_ASSERT_FALSE(cc_same_host("mac.a", "mac.b"));
    TEST_ASSERT_FALSE(cc_same_host("", "mac"));
    TEST_ASSERT_FALSE(cc_same_host(".", "."));
    TEST_ASSERT_FALSE(cc_same_host(NULL, "mac"));
    TEST_ASSERT_FALSE(cc_same_host("mac", NULL));
}

static void test_is_companion_server(void)
{
    /* by index */
    TEST_ASSERT_TRUE(cc_is_companion_server(2, "10.0.0.5", 2, "100.64.0.18"));
    /* a second entry for the same Mac */
    TEST_ASSERT_TRUE(cc_is_companion_server(0, "100.64.0.18", 2, "100.64.0.18"));
    TEST_ASSERT_TRUE(cc_is_companion_server(0, "mac", 2, "mac.tail1234.ts.net"));
    /* another machine */
    TEST_ASSERT_FALSE(cc_is_companion_server(0, "100.64.0.19", 2, "100.64.0.18"));
    /* no companion configured or its entry is missing */
    TEST_ASSERT_FALSE(cc_is_companion_server(0, "100.64.0.18", -1, NULL));
    TEST_ASSERT_FALSE(cc_is_companion_server(0, "100.64.0.18", -1, ""));
    TEST_ASSERT_FALSE(cc_is_companion_server(-1, "x", -1, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shell_quote_roundtrip);
    RUN_TEST(test_cmd_reply_exact);
    RUN_TEST(test_cmd_reply_leading_dash);
    RUN_TEST(test_quote_clears_out_on_failure);
    RUN_TEST(test_cmd_exact);
    RUN_TEST(test_cmd_rejects);
    RUN_TEST(test_cmd_watch_exact);
    RUN_TEST(test_cmd_watch_rejects);
    RUN_TEST(test_strip_ansi);
    RUN_TEST(test_log_tail_cursor_moves);
    RUN_TEST(test_log_tail_drops_rules_and_blanks);
    RUN_TEST(test_log_tail_drops_spinner_fragments);
    RUN_TEST(test_log_tail_keeps_short_words);
    RUN_TEST(test_log_tail_keeps_last);
    RUN_TEST(test_paste_build_exact);
    RUN_TEST(test_paste_build_strips_esc);
    RUN_TEST(test_paste_build_controls);
    RUN_TEST(test_paste_build_rejects);
    RUN_TEST(test_screen_permission_prompt);
    RUN_TEST(test_screen_input_prompt_is_not_permission);
    RUN_TEST(test_screen_real_input_after_question);
    RUN_TEST(test_screen_input_ready);
    RUN_TEST(test_same_host);
    RUN_TEST(test_is_companion_server);
    return UNITY_END();
}
