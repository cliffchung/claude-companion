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

static void test_same_name_interactive_sessions_are_distinct(void)
{
    cc_session_t a[2] = { inter("desk", 2, CC_IDLE), inter("desk", 1, CC_IDLE) };
    char k1[SM_KEY_MAX], k2[SM_KEY_MAX];
    sm_key(&a[0], k1, sizeof(k1)); sm_key(&a[1], k2, sizeof(k2));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(k1, k2));
    TEST_ASSERT_EQUAL(1, sm_select(a, 2, k2, 0));
    static sm_drafts_t ds;
    memset(&ds, 0, sizeof(ds));
    sm_draft_set(&ds, k1, "one");
    sm_draft_set(&ds, k2, "two");
    TEST_ASSERT_EQUAL_STRING("one", sm_draft_get(&ds, k1));
    TEST_ASSERT_EQUAL_STRING("two", sm_draft_get(&ds, k2));
}

static void test_draft_being_typed_survives_eviction(void)
{
    static sm_drafts_t ds;
    memset(&ds, 0, sizeof(ds));
    char k[16];
    sm_draft_set(&ds, "sel", "typing");
    for (int i = 0; i < 40; i++) {   /* other drafts churn; the selected one is re-set as the user types */
        snprintf(k, sizeof(k), "o%d", i);
        sm_draft_set(&ds, k, "t");
        sm_draft_set(&ds, "sel", "typing");
    }
    TEST_ASSERT_EQUAL_STRING("typing", sm_draft_get(&ds, "sel"));
}

static void test_draft_cut_keeps_utf8_whole(void)
{
    static sm_drafts_t ds;
    static char big[SM_ANSWER_MAX + 8];
    memset(&ds, 0, sizeof(ds));
    memset(big, 'a', SM_ANSWER_MAX - 1);
    memcpy(big + SM_ANSWER_MAX - 1, "\xe3\x81\x82\xe3\x81\x82", 6);   /* a 3-byte char straddles the cap */
    big[SM_ANSWER_MAX + 5] = 0;
    sm_draft_set(&ds, "k", big);
    const char *t = sm_draft_get(&ds, "k");
    TEST_ASSERT_EQUAL(SM_ANSWER_MAX - 1, strlen(t));
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

static sm_banner_t with(void (*mod)(sm_in_t *))
{
    sm_in_t i = ok();
    mod(&i);
    return sm_banner(&i);
}
static void f_nowifi(sm_in_t *i) { i->wifi_up = false; }
static void f_connecting(sm_in_t *i) { i->wifi_up = false; i->wifi_connecting = true; }
static void f_nohost(sm_in_t *i) { i->host_set = false; }
static void f_failed(sm_in_t *i) { i->link_failed = true; }
static void f_pollfail(sm_in_t *i) { i->poll_failing = true; }
static void f_down30(sm_in_t *i) { i->link_up = false; i->down_ms = SM_UNREACHABLE_MS; }
static void f_lowbat(sm_in_t *i) { i->low_battery_shown = true; }
static void f_reconn(sm_in_t *i) { i->reconnecting = true; }
static void f_waking(sm_in_t *i) { i->waking = true; }
static void f_nolist(sm_in_t *i) { i->have_list = false; }
static void f_down(sm_in_t *i) { i->link_up = false; i->down_ms = 1000; }   /* down within the grace */
static void f_stale(sm_in_t *i) { i->stale = true; }
static void f_empty(sm_in_t *i) { i->n = 0; }

/* each row of the table against every row below it: the higher one must win */
static void test_banner_precedence(void)
{
    sm_in_t i = ok();
    TEST_ASSERT_EQUAL(SM_B_LIVE, sm_banner(&i));
    typedef struct { void (*set)(sm_in_t *); sm_banner_t want; } row_t;
    const row_t t[] = {
        { f_nowifi, SM_B_NO_WIFI }, { f_nohost, SM_B_NO_HOST }, { f_failed, SM_B_UNREACHABLE },
        { f_lowbat, SM_B_LOW_BATTERY }, { f_reconn, SM_B_RECONNECTING }, { f_nolist, SM_B_LOADING },
        { f_down, SM_B_RECONNECTING }, { f_stale, SM_B_STALE }, { f_empty, SM_B_EMPTY },
    };
    int n = (int)(sizeof(t) / sizeof(t[0]));
    for (int a = 0; a < n; a++) {
        TEST_ASSERT_EQUAL_MESSAGE(t[a].want, with(t[a].set), "alone");
        for (int b = a + 1; b < n; b++) {
            sm_in_t x = ok();
            t[a].set(&x); t[b].set(&x);
            TEST_ASSERT_EQUAL_MESSAGE(t[a].want, sm_banner(&x), "pair");
        }
    }
    /* the other triggers of the same rows */
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, with(f_pollfail));
    TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, with(f_down30));
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, with(f_waking));
    { sm_in_t x = ok(); f_down30(&x); f_lowbat(&x); f_nowifi(&x); f_connecting(&x);
      TEST_ASSERT_EQUAL(SM_B_UNREACHABLE, sm_banner(&x)); }
}

/* T5 re-review N1: a down link outranks a stale list (and an empty one): Reconnecting, not Updating. */
static void test_banner_down_beats_stale(void)
{
    sm_in_t i = ok(); i.link_up = false; i.stale = true;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i.n = 0;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
}

static void test_banner_wifi_connecting_is_not_no_wifi(void)
{
    sm_in_t i = ok(); f_connecting(&i);
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));   /* with a list */
    i.have_list = false;
    TEST_ASSERT_EQUAL(SM_B_LOADING, sm_banner(&i));
    i.have_list = true; i.waking = true;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i = ok(); f_connecting(&i); f_nohost(&i);
    TEST_ASSERT_EQUAL(SM_B_NO_HOST, sm_banner(&i));
}

static void test_banner_down_within_grace(void)
{
    sm_in_t i = ok(); i.link_up = false; i.down_ms = 29999;
    TEST_ASSERT_EQUAL(SM_B_RECONNECTING, sm_banner(&i));
    i.have_list = false;
    TEST_ASSERT_EQUAL(SM_B_LOADING, sm_banner(&i));
    i.low_battery_shown = true;
    TEST_ASSERT_EQUAL(SM_B_LOW_BATTERY, sm_banner(&i));   /* low battery outranks both */
    i = ok(); i.low_battery_shown = true; i.link_up = false; i.waking = true;
    TEST_ASSERT_EQUAL(SM_B_LOW_BATTERY, sm_banner(&i));
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

/* Review Focus 3 / T5 carry: CJK advances two 12 px cells in FONT_MONO (measured in the preview's
   `sessions long`), so the wrap counts cells as the terminal does (vt_wcwidth), not characters. */
static void test_wrap_cjk_counts_two_cells(void)
{
    char o[128];
    const char *in = "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a";   /* 5 chars, 10 cells */
    TEST_ASSERT_EQUAL(2, sm_wrap_tail(in, 6, 10, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\n  \xe3\x81\x88\xe3\x81\x8a", o);
    TEST_ASSERT_EQUAL(4, sm_wrap_tail(in, 5, 10, o, sizeof(o)));   /* 2 + 1 + 1 + 1: a wide char never splits */
    TEST_ASSERT_EQUAL_STRING("\xe3\x81\x82\xe3\x81\x84\n  \xe3\x81\x86\n  \xe3\x81\x88\n  \xe3\x81\x8a", o);
    /* narrow then wide: the wide one moves to the next row rather than overflow it */
    TEST_ASSERT_EQUAL(2, sm_wrap_tail("ab\xe6\x97\xa5", 3, 10, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("ab\n  \xe6\x97\xa5", o);
    /* a row narrower than one wide char still takes one, so the wrap always ends */
    TEST_ASSERT_EQUAL(3, sm_wrap_tail("\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5", 3, 10, o, sizeof(o)));
}

/* The Sessions log is an LVGL label, not the terminal grid: its glyph advances set the cells (device:
   MaruMinya draws "\xe2\x96\x88" and "\xe2\x80\xa6" 24 px wide), so the screen passes its own width function. */
static int three_for_w(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp == 'w' ? 3 : 1;
}

static void test_wrap_cell_function(void)
{
    char o[64];
    TEST_ASSERT_EQUAL(3, sm_wrap_tail_fn("wwww", 6, 10, o, sizeof(o), three_for_w, NULL));
    TEST_ASSERT_EQUAL_STRING("ww\n  w\n  w", o);
    TEST_ASSERT_EQUAL(1, sm_wrap_tail_fn("abcdef", 6, 10, o, sizeof(o), three_for_w, NULL));
    /* NULL is the terminal's rule */
    TEST_ASSERT_EQUAL(2, sm_wrap_tail_fn("ab\xe6\x97\xa5", 3, 10, o, sizeof(o), NULL, NULL));
}

static void test_wrap_short_buffer_drops_oldest(void)
{
    char o[12];
    int n = sm_wrap_tail("one\ntwo\nthree\nfour\n", 20, 10, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("three\nfour", o);   /* 10 bytes + NUL fits; adding "two\n" would not */
    TEST_ASSERT_EQUAL(2, n);
    char tiny[4];
    TEST_ASSERT_EQUAL(0, sm_wrap_tail("abcdefgh\n", 20, 10, tiny, sizeof(tiny)));
}

static void test_wrap_crlf(void)
{
    char o[64];
    TEST_ASSERT_EQUAL(2, sm_wrap_tail("ab\r\ncd\r\n", 4, 10, o, sizeof(o)));
    TEST_ASSERT_EQUAL_STRING("ab\ncd", o);
}

/* The picks hint is for a session that can take an answer: NEEDS YOU or IDLE, never WORKING (a numbered paragraph
   list in a busy session's output is not a question), DONE or ENDED. */
static void test_picks_hint_state(void)
{
    TEST_ASSERT_TRUE(sm_picks_hint(CC_NEEDS_YOU, 3));
    TEST_ASSERT_TRUE(sm_picks_hint(CC_IDLE, 2));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_WORKING, 3));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_WORKING, sm_tail_options("Plan\n1. Read the code\n2. Fix it\n3. Test it\n")));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_DONE, 3));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_ENDED, 3));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_NEEDS_YOU, 1));
    TEST_ASSERT_FALSE(sm_picks_hint(CC_IDLE, 0));
}

static void test_tail_options(void)
{
    TEST_ASSERT_EQUAL(3, sm_tail_options("Pick\n1. A\n2. B\n3. C\n"));
    TEST_ASSERT_EQUAL(2, sm_tail_options("Pick\n\xe2\x9d\xaf 1. Yes\n  2. No\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("1. only\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("text\n"));
    TEST_ASSERT_EQUAL(0, sm_tail_options("1. A\n2. B\nthen text\n"));
}

/* P4C-11 (b): finished companion-test-* sessions leave the list; live ones and every other session stay */
static void test_hide_finished_tests(void)
{
    cc_session_t a[6];
    a[0] = bg("t1", CC_WORKING, true);   snprintf(a[0].name, sizeof(a[0].name), "companion-test-live");
    a[1] = bg("t2", CC_DONE, false);     snprintf(a[1].name, sizeof(a[1].name), "companion-test-done");
    a[2] = bg("u1", CC_DONE, false);     /* someone else's finished session */
    a[3] = bg("t3", CC_ENDED, false);    snprintf(a[3].name, sizeof(a[3].name), "companion-test-ended");
    a[4] = bg("t4", CC_NEEDS_YOU, true); snprintf(a[4].name, sizeof(a[4].name), "companion-test-ask");
    a[5] = bg("t5", CC_IDLE, true);      snprintf(a[5].name, sizeof(a[5].name), "companion-tests");   /* no dash: not one */
    int n = sm_hide_finished_tests(a, 6);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_STRING("companion-test-live", a[0].name);
    TEST_ASSERT_EQUAL_STRING("n-u1", a[1].name);
    TEST_ASSERT_EQUAL_STRING("companion-test-ask", a[2].name);
    TEST_ASSERT_EQUAL_STRING("companion-tests", a[3].name);
    TEST_ASSERT_EQUAL(0, sm_hide_finished_tests(a, 0));
    TEST_ASSERT_EQUAL(0, sm_hide_finished_tests(NULL, 3));
}

/* interactive sessions (a plain `claude` in a terminal) can't be opened from the Tab5, so they aren't listed */
static void test_hide_interactive(void)
{
    cc_session_t a[3];
    a[0] = bg("b1", CC_WORKING, true);
    a[1] = bg("i1", CC_WORKING, true); a[1].background = false; a[1].id[0] = 0;
    a[2] = bg("b2", CC_IDLE, true);
    int n = sm_hide_finished_tests(a, 3);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("n-b1", a[0].name);
    TEST_ASSERT_EQUAL_STRING("n-b2", a[1].name);
}

/* P4C-11 (a): an interactive session's row meta says it is on the Host (it can't be opened here) */
static void test_meta_marks_interactive(void)
{
    char m[96];
    cc_session_t s = bg("a1", CC_WORKING, true);
    snprintf(s.repo, sizeof(s.repo), "api");
    sm_meta(&s, "7m", m, sizeof(m));
    TEST_ASSERT_EQUAL_STRING("api \xc2\xb7 7m", m);
    sm_meta(&s, "", m, sizeof(m));
    TEST_ASSERT_EQUAL_STRING("api", m);
    cc_session_t i = inter("x", 0, CC_WORKING);
    snprintf(i.repo, sizeof(i.repo), "web");
    sm_meta(&i, "2h", m, sizeof(m));
    TEST_ASSERT_EQUAL_STRING("on Host \xc2\xb7 web \xc2\xb7 2h", m);
    sm_meta(&i, NULL, m, sizeof(m));
    TEST_ASSERT_EQUAL_STRING("on Host \xc2\xb7 web", m);
}

/* Stop review I2: a session stopped here stays listed past the finished limit and the test filter, until it runs
   again after a poll showed it stopped, or leaves the Host's list. */
static cc_session_t fin(const char *id, cc_state_t st, int64_t started, const char *name)
{
    cc_session_t s = bg(id, st, false);
    s.started_ms = started;
    if (name) snprintf(s.name, sizeof(s.name), "%s", name);
    return s;
}

static void test_pins_keep_a_stopped_session(void)
{
    sm_pins_t p;
    memset(&p, 0, sizeof(p));
    /* five finished, the oldest (a0000000) is the one just stopped here, plus a live one */
    cc_session_t a[8] = { bg("c0000000", CC_WORKING, true),
                          fin("a0000004", CC_DONE, 50, NULL), fin("a0000003", CC_DONE, 40, NULL),
                          fin("a0000002", CC_ENDED, 30, NULL), fin("a0000001", CC_ENDED, 20, NULL),
                          fin("a0000000", CC_ENDED, 10, NULL) };
    cc_session_t w[8];
    memcpy(w, a, sizeof(a));
    TEST_ASSERT_EQUAL(5, sm_visible(w, 6, 4, &p));   /* unpinned: the oldest finished one goes */
    TEST_ASSERT_EQUAL_STRING("a0000001", w[4].id);
    sm_pin_add(&p, "a0000000");
    TEST_ASSERT_TRUE(sm_pinned(&p, "a0000000"));
    memcpy(w, a, sizeof(a));
    TEST_ASSERT_EQUAL(6, sm_visible(w, 6, 4, &p));   /* pinned: listed too, and the 4 newest still are */
    TEST_ASSERT_EQUAL_STRING("a0000000", w[5].id);
    TEST_ASSERT_EQUAL_STRING("a0000001", w[4].id);
    /* NULL pins: as cc_sessions_visible */
    memcpy(w, a, sizeof(a));
    TEST_ASSERT_EQUAL(5, sm_visible(w, 6, 4, NULL));
    /* a pinned companion-test-* session isn't hidden; an unpinned finished one is */
    cc_session_t t[3] = { fin("b0000001", CC_ENDED, 5, "companion-test-stop"), fin("b0000002", CC_ENDED, 6, "companion-test-x"),
                          inter("desk", 1, CC_IDLE) };
    sm_pin_add(&p, "b0000001");
    TEST_ASSERT_EQUAL(1, sm_visible(t, 3, 4, &p));
    TEST_ASSERT_EQUAL_STRING("b0000001", t[0].id);
}

static void test_pins_until_running_again(void)
{
    sm_pins_t p;
    memset(&p, 0, sizeof(p));
    sm_pin_add(&p, "a0000000");
    /* the list still shows the process right after the stop: the pin stays */
    cc_session_t s = bg("a0000000", CC_IDLE, true);
    sm_pins_prune(&p, &s, 1);
    TEST_ASSERT_TRUE(sm_pinned(&p, "a0000000"));
    /* stopped, then opened and running again: the pin goes */
    s.live = false; s.state = CC_ENDED;
    sm_pins_prune(&p, &s, 1);
    TEST_ASSERT_TRUE(sm_pinned(&p, "a0000000"));
    s.live = true; s.state = CC_IDLE;
    sm_pins_prune(&p, &s, 1);
    TEST_ASSERT_FALSE(sm_pinned(&p, "a0000000"));
    /* gone from the Host's list: the pin goes */
    sm_pin_add(&p, "a0000009");
    sm_pins_prune(&p, &s, 1);
    TEST_ASSERT_FALSE(sm_pinned(&p, "a0000009"));
}

static void test_pins_hold_four(void)
{
    sm_pins_t p;
    memset(&p, 0, sizeof(p));
    const char *k[5] = { "a0000001", "a0000002", "a0000003", "a0000004", "a0000005" };
    for (int i = 0; i < 4; i++) sm_pin_add(&p, k[i]);
    sm_pin_add(&p, k[0]);   /* again: refreshed, no duplicate */
    sm_pin_add(&p, k[4]);   /* full: the least recently pinned (k[1]) goes */
    TEST_ASSERT_TRUE(sm_pinned(&p, k[0]));
    TEST_ASSERT_FALSE(sm_pinned(&p, k[1]));
    for (int i = 2; i < 5; i++) TEST_ASSERT_TRUE(sm_pinned(&p, k[i]));
    TEST_ASSERT_FALSE(sm_pinned(&p, ""));
    TEST_ASSERT_FALSE(sm_pinned(NULL, k[0]));
    sm_pin_add(&p, "");   /* ignored */
    TEST_ASSERT_TRUE(sm_pinned(&p, k[0]));
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
    RUN_TEST(test_same_name_interactive_sessions_are_distinct);
    RUN_TEST(test_draft_being_typed_survives_eviction);
    RUN_TEST(test_draft_cut_keeps_utf8_whole);
    RUN_TEST(test_next_needs_you_wraps);
    RUN_TEST(test_pill_text_rules);
    RUN_TEST(test_banner_precedence);
    RUN_TEST(test_banner_down_beats_stale);
    RUN_TEST(test_banner_wifi_connecting_is_not_no_wifi);
    RUN_TEST(test_banner_down_within_grace);
    RUN_TEST(test_lowbat_once_per_discharge);
    RUN_TEST(test_wrap_hanging_indent);
    RUN_TEST(test_wrap_keeps_newest_rows);
    RUN_TEST(test_wrap_unbroken_line);
    RUN_TEST(test_wrap_utf8_counts_chars);
    RUN_TEST(test_wrap_cjk_counts_two_cells);
    RUN_TEST(test_wrap_cell_function);
    RUN_TEST(test_wrap_short_buffer_drops_oldest);
    RUN_TEST(test_wrap_crlf);
    RUN_TEST(test_tail_options);
    RUN_TEST(test_picks_hint_state);
    RUN_TEST(test_hide_finished_tests);
    RUN_TEST(test_hide_interactive);
    RUN_TEST(test_meta_marks_interactive);
    RUN_TEST(test_pins_keep_a_stopped_session);
    RUN_TEST(test_pins_until_running_again);
    RUN_TEST(test_pins_hold_four);
    return UNITY_END();
}
