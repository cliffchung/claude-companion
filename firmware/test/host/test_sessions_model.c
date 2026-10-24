#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "ui/sessions_model.h"

void setUp(void) {}
void tearDown(void) {}

static cc_session_t bg(const char *id, cc_state_t st, bool live)
{
    cc_session_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.id, sizeof(s.id), "%s", id);
    snprintf(s.name, sizeof(s.name), "n-%s", id);
    s.background = true; s.state = st; s.live = live;
    return s;
}

static cc_session_t inter(const char *name, int64_t started, cc_state_t st)
{
    cc_session_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", name);
    s.started_ms = started; s.state = st;
    return s;
}

static void test_key_background_is_id(void)
{
    cc_session_t s = bg("a1b2c3d4", CC_WORKING, true);
    char k[SM_KEY_MAX];
    sm_key(&s, k, sizeof(k));
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", k);
}

static void test_key_interactive(void)
{
    cc_session_t s = inter("desk", 1700000000123LL, CC_IDLE);
    char k[SM_KEY_MAX];
    sm_key(&s, k, sizeof(k));
    TEST_ASSERT_EQUAL_STRING("i:desk:1700000000123", k);
}

static void test_selection_follows_key_across_resort(void)
{
    cc_session_t before[3] = { bg("A", CC_WORKING, true), bg("B", CC_WORKING, true), bg("C", CC_IDLE, true) };
    char sel[SM_KEY_MAX];
    sm_key(&before[1], sel, sizeof(sel));
    TEST_ASSERT_EQUAL(1, sm_select(before, 3, sel, 1));
    cc_session_t after[3] = { bg("B", CC_NEEDS_YOU, true), bg("C", CC_IDLE, true), bg("A", CC_DONE, false) };
    TEST_ASSERT_EQUAL(0, sm_select(after, 3, sel, 1));   /* index 1 is now C: the key wins, not the position */
    /* a different session that takes the old index never inherits the selection */
    cc_session_t swap[3] = { bg("A", CC_WORKING, true), bg("X", CC_WORKING, true), bg("B", CC_IDLE, true) };
    TEST_ASSERT_EQUAL(2, sm_select(swap, 3, sel, 1));
}

static void test_selection_on_removal_keeps_position(void)
{
    cc_session_t abc[3] = { bg("A", CC_WORKING, true), bg("B", CC_WORKING, true), bg("C", CC_WORKING, true) };
    char sel[SM_KEY_MAX];
    sm_key(&abc[1], sel, sizeof(sel));
    cc_session_t ac[2] = { abc[0], abc[2] };
    TEST_ASSERT_EQUAL(1, sm_select(ac, 2, sel, 1));
    cc_session_t a[1] = { abc[0] };
    TEST_ASSERT_EQUAL(0, sm_select(a, 1, sel, 1));
    TEST_ASSERT_EQUAL(-1, sm_select(a, 0, sel, 1));
}

static void test_draft_follows_key_across_resort(void)
{
    static sm_drafts_t ds;
    memset(&ds, 0, sizeof(ds));
    cc_session_t before[2] = { bg("A", CC_WORKING, true), bg("B", CC_WORKING, true) };
    sm_draft_set(&ds, "B", "hi");
    cc_session_t after[2] = { before[1], before[0] };   /* re-sorted: B is now first */
    sm_drafts_prune(&ds, after, 2);
    TEST_ASSERT_EQUAL_STRING("hi", sm_draft_get(&ds, "B"));
    TEST_ASSERT_EQUAL_STRING("", sm_draft_get(&ds, "A"));
    /* the draft is addressed by the key of whatever row is now at index 0 */
    char k0[SM_KEY_MAX];
    sm_key(&after[0], k0, sizeof(k0));
    TEST_ASSERT_EQUAL_STRING("hi", sm_draft_get(&ds, k0));
}

static void test_draft_cleared_on_send_and_removal(void)
{
    static sm_drafts_t ds;
    memset(&ds, 0, sizeof(ds));
    sm_draft_set(&ds, "B", "x");
    sm_draft_set(&ds, "B", "");
    TEST_ASSERT_EQUAL_STRING("", sm_draft_get(&ds, "B"));
    sm_draft_set(&ds, "C", "y");
    cc_session_t only[1] = { bg("A", CC_WORKING, true) };
    sm_drafts_prune(&ds, only, 1);
    TEST_ASSERT_EQUAL_STRING("", sm_draft_get(&ds, "C"));
}

static void test_drafts_evict_least_recent(void)
{
    static sm_drafts_t ds;
    memset(&ds, 0, sizeof(ds));
    char k[16];
    for (int i = 1; i <= 17; i++) { snprintf(k, sizeof(k), "k%d", i); sm_draft_set(&ds, k, "t"); }
    TEST_ASSERT_EQUAL_STRING("", sm_draft_get(&ds, "k1"));
    for (int i = 2; i <= 17; i++) { snprintf(k, sizeof(k), "k%d", i); TEST_ASSERT_EQUAL_STRING("t", sm_draft_get(&ds, k)); }
    /* updating an entry refreshes it: k2 survives the next insert, k3 goes */
    sm_draft_set(&ds, "k2", "u");
    sm_draft_set(&ds, "k18", "t");
    TEST_ASSERT_EQUAL_STRING("u", sm_draft_get(&ds, "k2"));
    TEST_ASSERT_EQUAL_STRING("", sm_draft_get(&ds, "k3"));
}

static void test_next_needs_you_wraps(void)
{
    cc_session_t a[4] = { bg("1", CC_WORKING, true), bg("2", CC_NEEDS_YOU, true), bg("3", CC_WORKING, true), bg("4", CC_NEEDS_YOU, true) };
    TEST_ASSERT_EQUAL(1, sm_next_needs_you(a, 4, 3));
    cc_session_t b[2] = { bg("1", CC_WORKING, true), bg("2", CC_WORKING, true) };
    TEST_ASSERT_EQUAL(1, sm_next_needs_you(b, 2, 0));
    TEST_ASSERT_EQUAL(-1, sm_next_needs_you(b, 0, 0));
}

static void test_pill_text_rules(void)
{
    cc_session_t s = bg("a", CC_NEEDS_YOU, true);
    TEST_ASSERT_EQUAL_STRING("NEEDS YOU", sm_pill_text(&s));
    s = bg("a", CC_NEEDS_YOU, false);
    TEST_ASSERT_EQUAL_STRING("ENDED", sm_pill_text(&s));
    s = inter("d", 1, CC_NEEDS_YOU);
    TEST_ASSERT_EQUAL_STRING("waiting on Host", sm_pill_text(&s));
    s = bg("a", CC_WORKING, true); TEST_ASSERT_EQUAL_STRING("WORKING", sm_pill_text(&s));
    s.state = CC_IDLE;  TEST_ASSERT_EQUAL_STRING("IDLE", sm_pill_text(&s));
    s.state = CC_DONE;  TEST_ASSERT_EQUAL_STRING("DONE", sm_pill_text(&s));
    s.state = CC_ENDED; TEST_ASSERT_EQUAL_STRING("ENDED", sm_pill_text(&s));
}

static sm_in_t ok(void)
{
    sm_in_t i;
    memset(&i, 0, sizeof(i));
    i.host_set = i.wifi_up = i.link_up = i.have_list = true;
    i.n = 2;
    return i;
}

static void test_banner_precedence(void)
{
    sm_in_t i = ok();
    TEST_ASSERT_EQUAL(SM_B_LIVE, sm_banner(&i));
    i = ok(); i.wifi_up = false; i.host_set = false; i.link_failed = true; i.low_battery_shown = true;
    TEST_ASSERT_EQUAL(SM_B_NO_WIFI, sm_banner(&i));
    i = ok(); i.wifi_up = false; i.wifi_connecting = true;
    TEST_ASSERT_EQUAL(SM_B_NO_WIFI, sm_banner(&i));
    i = ok(); i.host_set = false;
    TEST_ASSERT_EQUAL(SM_B_NO_HOST, sm_banner(&i));
    i = ok(); i.link_up = false; i.link_failed = true;
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, sm_banner(&i));
    i = ok(); i.link_up = false; i.down_ms = 30000;
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, sm_banner(&i));
    i = ok(); i.link_up = false; i.down_ms = 29999; i.have_list = false;
    TEST_ASSERT_EQUAL(SM_B_LOADING, sm_banner(&i));
    i = ok(); i.link_up = false; i.down_ms = 29999;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i = ok(); i.poll_failing = true;
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, sm_banner(&i));
    i = ok(); i.low_battery_shown = true;
    TEST_ASSERT_EQUAL(SM_B_LOW_BATTERY, sm_banner(&i));
    i = ok(); i.low_battery_shown = true; i.link_failed = true;
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, sm_banner(&i));
    i = ok(); i.reconnecting = true;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i = ok(); i.waking = true;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i = ok(); i.have_list = false;
    TEST_ASSERT_EQUAL(SM_B_LOADING, sm_banner(&i));
    i = ok(); i.stale = true;
    TEST_ASSERT_EQUAL(SM_B_STALE, sm_banner(&i));
    i = ok(); i.n = 0;
    TEST_ASSERT_EQUAL(SM_B_EMPTY, sm_banner(&i));
}

static void test_lowbat_once_per_discharge(void)
{
    sm_lowbat_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_FALSE(sm_lowbat_step(&st, 16, false, false));
    TEST_ASSERT_TRUE(sm_lowbat_step(&st, 15, false, false));
    TEST_ASSERT_TRUE(sm_lowbat_step(&st, 14, false, false));
    TEST_ASSERT_FALSE(sm_lowbat_step(&st, 14, false, true));
    TEST_ASSERT_FALSE(sm_lowbat_step(&st, 13, false, false));   /* stays dismissed */
    TEST_ASSERT_FALSE(sm_lowbat_step(&st, 14, true, false));    /* charging re-arms */
    TEST_ASSERT_TRUE(sm_lowbat_step(&st, 15, false, false));
    TEST_ASSERT_FALSE(sm_lowbat_step(&st, -1, false, false));
}

static void test_wrap_hanging_indent(void)
{
    char o[128];
    TEST_ASSERT_EQUAL(4, sm_wrap_tail("abcdefghij", 4, 10, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("abcd\n  ef\n  gh\n  ij", o);
}

static void test_wrap_keeps_newest_rows(void)
{
    const char *in = "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\nl12\n";
    char o[256];
    TEST_ASSERT_EQUAL(10, sm_wrap_tail(in, 20, 10, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("l3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\nl12", o);
}

static void test_wrap_unbroken_line(void)
{
    char in[301], o[1024];
    memset(in, 'x', 300); in[300] = 0;
    int n = sm_wrap_tail(in, 62, 100, o, sizeof(o));
    TEST_ASSERT_TRUE(n > 4);
    int letters = 0, run = 0;
    for (const char *p = o;; p++) {
        if (*p == '\n' || !*p) { TEST_ASSERT_TRUE(run <= 62); run = 0; if (!*p) break; }
        else { run++; if (*p == 'x') letters++; }
    }
    TEST_ASSERT_EQUAL(300, letters);
}

static void test_wrap_utf8_counts_chars(void)
{
    char o[64];
    int n = sm_wrap_tail("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 3, 10, o, sizeof(o));
    TEST_ASSERT_EQUAL(3, n);   /* 3 chars, then 1 + 1 on the indented continuations */
    /* every row is whole characters */
    for (const unsigned char *p = (const unsigned char *)o; *p; p++) {
        if (*p == 0xC3) { TEST_ASSERT_EQUAL_HEX8(0xA9, p[1]); p++; }
        else TEST_ASSERT_TRUE(*p == ' ' || *p == '\n');
    }
    n = sm_wrap_tail("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 4, 10, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\n  \xc3\xa9", o);
}

static void test_tail_options(void)
{
    TEST_ASSERT_EQUAL(3, sm_tail_options("Pick\n1. A\n2. B\n3. C\n"));
    TEST_ASSERT_EQUAL(2, sm_tail_options("Pick\n\xe2\x9d\xaf 1. Yes\n  2. No\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("1. only\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("text\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("1. A\n2. B\nthen text\n"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_background_is_id);
    RUN_TEST(test_key_interactive);
    RUN_TEST(test_selection_follows_key_across_resort);
    RUN_TEST(test_selection_on_removal_keeps_position);
    RUN_TEST(test_draft_follows_key_across_resort);
    RUN_TEST(test_draft_cleared_on_send_and_removal);
    RUN_TEST(test_drafts_evict_least_recent);
    RUN_TEST(test_next_needs_you_wraps);
    RUN_TEST(test_pill_text_rules);
    RUN_TEST(test_banner_precedence);
    RUN_TEST(test_lowbat_once_per_discharge);
    RUN_TEST(test_wrap_hanging_indent);
    RUN_TEST(test_wrap_keeps_newest_rows);
    RUN_TEST(test_wrap_unbroken_line);
    RUN_TEST(test_wrap_utf8_counts_chars);
    RUN_TEST(test_tail_options);
    return UNITY_END();
}
