#include <stdio.h>
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

static void test_strip_ansi(void)
{
    char s[] = "\x1b[1;32m\xe2\x9c\x93 done\x1b[0m\x1b]0;title\x07";
    size_t n = cc_strip_ansi(s);
    TEST_ASSERT_EQUAL_STRING("\xe2\x9c\x93 done", s);
    TEST_ASSERT_EQUAL_UINT(strlen(s), n);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shell_quote_roundtrip);
    RUN_TEST(test_cmd_reply_exact);
    RUN_TEST(test_cmd_reply_leading_dash);
    RUN_TEST(test_quote_clears_out_on_failure);
    RUN_TEST(test_cmd_exact);
    RUN_TEST(test_cmd_rejects);
    RUN_TEST(test_strip_ansi);
    RUN_TEST(test_paste_build_exact);
    RUN_TEST(test_paste_build_strips_esc);
    RUN_TEST(test_paste_build_controls);
    RUN_TEST(test_paste_build_rejects);
    return UNITY_END();
}
