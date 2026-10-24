#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cc_sessions.h"

void setUp(void) {}
void tearDown(void) {}

static char *slurp(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    TEST_ASSERT_EQUAL(n, (long)fread(b, 1, n, f));
    b[n] = 0;
    fclose(f);
    return b;
}

static const cc_session_t *find(const cc_session_t *s, int n, const char *id)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].id, id)) return &s[i];
    }
    return NULL;
}

static const cc_session_t *find_name(const cc_session_t *s, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].name, name)) return &s[i];
    }
    return NULL;
}

static void test_parse_synthetic_order(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[16];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    TEST_ASSERT_EQUAL(5, n);
    /* the interactive entry (real 2.1.309 shape: no "id") has an empty id */
    const char *want[] = { "aaaaaaa2", "aaaaaaa3", "", "aaaaaaa6", "aaaaaaa1" };
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_STRING(want[i], s[i].id);
    TEST_ASSERT_EQUAL_STRING("desk session", s[2].name);
}

static void test_parse_flags(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[16];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    /* interactive: kept, never actionable, no ids even though the entry has a sessionId */
    const cc_session_t *desk = find_name(s, n, "desk session");
    TEST_ASSERT_NOT_NULL(desk);
    TEST_ASSERT_FALSE(desk->background);
    TEST_ASSERT_EQUAL_STRING("", desk->id);
    TEST_ASSERT_EQUAL_STRING("", desk->session_id);
    TEST_ASSERT_EQUAL_STRING("delta", desk->repo);
    TEST_ASSERT_TRUE(desk->live);
    TEST_ASSERT_TRUE(find(s, n, "aaaaaaa2")->background);
    TEST_ASSERT_EQUAL_STRING("gamma", find(s, n, "aaaaaaa3")->repo);
    TEST_ASSERT_EQUAL_STRING("needs input", find(s, n, "aaaaaaa2")->name);
    TEST_ASSERT_EQUAL_STRING("22222222-2222-4222-8222-222222222222", find(s, n, "aaaaaaa2")->session_id);
    TEST_ASSERT_EQUAL_INT64(3000, find(s, n, "aaaaaaa2")->started_ms);
    TEST_ASSERT_EQUAL(CC_WORKING, desk->state);
    /* an unnamed interactive entry is shown by its repo; junk ids don't matter for it */
    const char *j2 = "[{\"pid\":1,\"cwd\":\"/Users/me/code/zeta/\",\"kind\":\"interactive\",\"startedAt\":1,"
                     "\"sessionId\":\"not-a-uuid\",\"name\":\"\",\"status\":\"idle\"},"
                     "{\"pid\":2,\"cwd\":\"/Users/me/code/eta\",\"kind\":\"interactive\",\"startedAt\":2,\"status\":\"idle\"}]";
    cc_session_t t[4];
    int m = cc_sessions_parse(j2, t, 4);
    TEST_ASSERT_EQUAL(2, m);
    TEST_ASSERT_NOT_NULL(find_name(t, m, "zeta"));
    TEST_ASSERT_NOT_NULL(find_name(t, m, "eta"));
    TEST_ASSERT_EQUAL_STRING("", find_name(t, m, "zeta")->session_id);
}

static void test_parse_unknown_state_is_idle(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[16];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    TEST_ASSERT_EQUAL(CC_IDLE, find(s, n, "aaaaaaa6")->state);
}

static void test_parse_invalid_json(void)
{
    cc_session_t s[4];
    TEST_ASSERT_EQUAL(-1, cc_sessions_parse("not json", s, 4));
    TEST_ASSERT_EQUAL(-1, cc_sessions_parse("{}", s, 4));
    TEST_ASSERT_EQUAL(0, cc_sessions_parse("[]", s, 4));
    TEST_ASSERT_EQUAL(-1, cc_sessions_parse(NULL, s, 4));
}

static void test_parse_cap(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[2];
    int n = cc_sessions_parse(j, s, 2);
    free(j);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("aaaaaaa2", s[0].id);
    TEST_ASSERT_EQUAL_STRING("aaaaaaa3", s[1].id);
}

static void test_parse_real_capture(void)
{
    char *j = slurp("agents_captured.json");
    cc_session_t s[16];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    TEST_ASSERT_GREATER_OR_EQUAL(2, n);
    /* companion-test-input was blocked/idle at capture time (its turn had ended, no question on screen: IDLE);
       companion-test-perm had finished (the real Claude Code never produced a permission prompt for it) */
    for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(s[i].background);
    const cc_session_t *in = NULL, *perm = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].name, "companion-test-input")) in = &s[i];
        if (!strcmp(s[i].name, "companion-test-perm")) perm = &s[i];
    }
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(perm);
    TEST_ASSERT_EQUAL(CC_IDLE, in->state);
    TEST_ASSERT_EQUAL(CC_DONE, perm->state);
    for (int i = 0; i < n; i++) TEST_ASSERT_NOT_EQUAL(CC_NEEDS_YOU, s[i].state);
}

/* Claude Code 2.1.310, captured from companion-test sessions: only a question on screen needs you */
static void test_parse_waiting_is_the_only_needs_you(void)
{
    char *j = slurp("agents_waiting.json");
    cc_session_t s[8];
    int n = cc_sessions_parse(j, s, 8);
    free(j);
    TEST_ASSERT_EQUAL(6, n);
    const cc_session_t *by[7] = { 0 };
    for (int i = 0; i < n; i++) by[s[i].id[7] - '0'] = &s[i];
    TEST_ASSERT_EQUAL(CC_NEEDS_YOU, by[1]->state);   /* status waiting, waitingFor "input needed" */
    TEST_ASSERT_TRUE(cc_session_needs_you(by[1]));
    TEST_ASSERT_EQUAL(CC_IDLE, by[2]->state);        /* blocked + idle: the turn ended, no question */
    TEST_ASSERT_FALSE(cc_session_needs_you(by[2]));
    TEST_ASSERT_EQUAL(CC_DONE, by[3]->state);
    TEST_ASSERT_EQUAL(CC_IDLE, by[4]->state);        /* blocked, no pid, no status: stopped mid-wait */
    TEST_ASSERT_EQUAL(CC_WORKING, by[5]->state);
    TEST_ASSERT_EQUAL(CC_IDLE, by[6]->state);        /* state working, status idle: the process is idle */
    char out[64];
    cc_summary(s, n, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1 needs you \xC2\xB7 1 working", out);
    TEST_ASSERT_EQUAL_STRING("aaaaaaa1", s[0].id);   /* NEEDS_YOU sorts first */
}

static void test_visible_limits_finished(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[16];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    /* three more done sessions, newer than aaaaaaa1; list order keeps newest first in DONE */
    for (int k = 0; k < 3; k++) {
        cc_session_t d = {0};
        snprintf(d.id, sizeof(d.id), "bbbbbbb%d", k);
        d.state = CC_DONE;
        d.background = true;
        d.started_ms = 7000 + k * 1000;
        s[n++] = d;
    }
    /* re-sort DONE tail newest first: bbbbbbb2, bbbbbbb1, bbbbbbb0 inserted before aaaaaaa1 */
    cc_session_t tail[4] = { s[n - 1], s[n - 2], s[n - 3], s[n - 4] };
    for (int i = 0; i < 4; i++) s[n - 4 + i] = tail[i];
    n = cc_sessions_visible(s, n, 2);
    TEST_ASSERT_EQUAL(6, n);
    TEST_ASSERT_EQUAL_STRING("aaaaaaa2", s[0].id);
    TEST_ASSERT_EQUAL_STRING("aaaaaaa6", s[3].id);
    TEST_ASSERT_EQUAL_STRING("bbbbbbb2", s[4].id);
    TEST_ASSERT_EQUAL_STRING("bbbbbbb1", s[5].id);
}

static void test_summary_text(void)
{
    char *j = slurp("agents_synthetic.json");
    cc_session_t s[16];
    char out[64];
    int n = cc_sessions_parse(j, s, 16);
    free(j);
    /* the fixture's blocked sessions have no pid (stopped): give them one, a stopped one is not counted */
    for (int i = 0; i < n; i++) if (s[i].state == CC_NEEDS_YOU) s[i].live = true;
    cc_summary(s, n, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2 need you \xc2\xb7 1 working", out);
    cc_summary(s, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1 needs you", out);
    cc_summary(s + 2, 1, out, sizeof(out)); /* aaaaaaa4: working */
    TEST_ASSERT_EQUAL_STRING("1 working", out);
    cc_summary(s + 4, 1, out, sizeof(out)); /* done */
    TEST_ASSERT_EQUAL_STRING("All quiet", out);
}

static void test_summary_stopped_blocked_not_counted(void)
{
    cc_session_t s[3];
    char out[64];
    memset(s, 0, sizeof(s));
    s[0].state = CC_NEEDS_YOU; s[0].background = true; s[0].live = false;   /* stopped */
    s[1].state = CC_NEEDS_YOU; s[1].background = false; s[1].live = true;   /* interactive */
    s[2].state = CC_WORKING; s[2].background = true; s[2].live = true;
    TEST_ASSERT_FALSE(cc_session_needs_you(&s[0]));
    TEST_ASSERT_FALSE(cc_session_needs_you(&s[1]));
    cc_summary(s, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1 working", out);
    cc_summary(s, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("All quiet", out);
    s[0].live = true;
    TEST_ASSERT_TRUE(cc_session_needs_you(&s[0]));
    cc_summary(s, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1 needs you", out);
}

static void test_age_format(void)
{
    char o[16];
    cc_age(45000, 0, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("45s", o);
    cc_age(720000, 0, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("12m", o);
    cc_age(10800000, 0, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("3h", o);
    cc_age(172800000, 0, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("2d", o);
}

/* `claude stop` leaves the entry listed, often still "state":"done", but without a pid. */
static void test_parse_live_from_pid(void)
{
    const char *j = "[{\"id\":\"aaaaaaa1\",\"sessionId\":\"11111111-1111-4111-8111-111111111111\",\"kind\":\"background\","
                    "\"state\":\"done\",\"status\":\"idle\",\"pid\":4242,\"startedAt\":2},"
                    "{\"id\":\"aaaaaaa2\",\"sessionId\":\"22222222-2222-4222-8222-222222222222\",\"kind\":\"background\","
                    "\"state\":\"done\",\"startedAt\":1}]";
    cc_session_t s[4];
    int n = cc_sessions_parse(j, s, 4);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_TRUE(find(s, n, "aaaaaaa1")->live);
    TEST_ASSERT_FALSE(find(s, n, "aaaaaaa2")->live);
    char *real = slurp("agents_captured.json");
    cc_session_t r[8];
    int m = cc_sessions_parse(real, r, 8);
    free(real);
    TEST_ASSERT_TRUE(find(r, m, "d0000001")->live);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_synthetic_order);
    RUN_TEST(test_parse_flags);
    RUN_TEST(test_parse_unknown_state_is_idle);
    RUN_TEST(test_parse_invalid_json);
    RUN_TEST(test_parse_cap);
    RUN_TEST(test_parse_real_capture);
    RUN_TEST(test_parse_waiting_is_the_only_needs_you);
    RUN_TEST(test_visible_limits_finished);
    RUN_TEST(test_summary_text);
    RUN_TEST(test_summary_stopped_blocked_not_counted);
    RUN_TEST(test_age_format);
    RUN_TEST(test_parse_live_from_pid);
    return UNITY_END();
}
