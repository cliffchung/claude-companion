#include <string.h>
#include "unity.h"
#include "sys/link_guard.h"
#include "companion/cc_park.h"

/* A fake clock: sleeping advances it. */
static int64_t s_now;
static int64_t fake_now(void) { return s_now; }
static void fake_sleep(int ms) { s_now += ms; }
static const lg_clock_t s_clk = { fake_now, fake_sleep };

/* Every step appends one letter to the log, so the tests read the order. */
static char s_log[128];
static void logc(char c) { size_t n = strlen(s_log); if (n + 1 < sizeof(s_log)) { s_log[n] = c; s_log[n + 1] = 0; } }

/* A fake SSH owner: closing takes close_ms of fake time to become idle. */
typedef struct {
    char tag;            /* upper case: close, lower case: reopen */
    bool open;           /* has a session (or a connect in flight) */
    bool tailnet;        /* that session is over a tailnet address */
    int close_ms;        /* how long its close takes; < 0: never idle */
    int64_t closing_at;  /* -1: not closing */
    bool parks;          /* close takes a park that reopen undoes */
    int reopened;
} fake_t;

static bool f_close(bool tailnet_only, void *ctx)
{
    fake_t *f = ctx;
    logc(f->tag);
    if (tailnet_only && !f->tailnet) return false;
    if (f->open && f->closing_at < 0) f->closing_at = s_now;
    return f->parks;
}
static bool f_idle(bool tailnet_only, void *ctx)
{
    fake_t *f = ctx;
    if (tailnet_only && !f->tailnet) return true;
    if (!f->open) return true;
    if (f->close_ms < 0 || f->closing_at < 0) return false;
    if (s_now - f->closing_at >= f->close_ms) { f->open = false; return true; }
    return false;
}
static void f_reopen(void *ctx) { fake_t *f = ctx; f->reopened++; logc((char)(f->tag + 32)); }

static fake_t s_link, s_term;
/* the down step records whether every session was closed when it ran */
static bool s_open_at_down;
static void down(void *ctx) { (void)ctx; logc('D'); s_open_at_down = s_link.open || s_term.open; }

void setUp(void)
{
    lg_reset();
    lg_set_clock(&s_clk);
    s_now = 1000;
    s_log[0] = 0;
    s_open_at_down = false;
    s_link = (fake_t){ .tag = 'L', .open = true, .close_ms = 30, .closing_at = -1, .parks = true };
    s_term = (fake_t){ .tag = 'T', .open = true, .close_ms = 50, .closing_at = -1, .parks = false };
    lg_closer_t a = { "link", f_close, f_idle, f_reopen, &s_link };
    lg_closer_t b = { "term", f_close, f_idle, NULL, &s_term };
    TEST_ASSERT_TRUE(lg_register(&a));
    TEST_ASSERT_TRUE(lg_register(&b));
}
void tearDown(void) {}

/* C1: the sessions are closed before the down step (Wi-Fi leave, network switch, Tailscale off, restart). */
static void test_close_runs_before_down_and_waits_for_idle(void)
{
    TEST_ASSERT_TRUE(lg_around(false, LG_CLOSE_MS, down, NULL, true));
    TEST_ASSERT_EQUAL_STRING("LTDl", s_log);   /* close both, wait, down, reopen the park */
    TEST_ASSERT_FALSE(s_open_at_down);
    TEST_ASSERT_EQUAL(1, s_link.reopened);
}

/* A restart or a deep sleep: no reopen. */
static void test_no_reopen_when_going_down_for_good(void)
{
    TEST_ASSERT_TRUE(lg_around(false, LG_CLOSE_MS, down, NULL, false));
    TEST_ASSERT_EQUAL_STRING("LTD", s_log);
    TEST_ASSERT_EQUAL(0, s_link.reopened);
}

/* The wait is bounded at about 2 s, and the down step still runs when a close hangs. */
static void test_wait_is_bounded_and_down_still_runs(void)
{
    s_term.close_ms = -1;   /* a hostkey prompt, a stuck connect */
    int64_t t0 = s_now;
    TEST_ASSERT_FALSE(lg_around(false, LG_CLOSE_MS, down, NULL, true));
    TEST_ASSERT_EQUAL_STRING("LTDl", s_log);
    TEST_ASSERT_TRUE(s_now - t0 >= LG_CLOSE_MS);
    TEST_ASSERT_TRUE(s_now - t0 <= LG_CLOSE_MS + 20);
}

/* Nothing open: no wait at all. */
static void test_nothing_open_returns_at_once(void)
{
    s_link.open = false; s_term.open = false;
    int64_t t0 = s_now;
    lg_token_t t = lg_close(false, LG_CLOSE_MS);
    TEST_ASSERT_TRUE(t.idle);
    TEST_ASSERT_EQUAL(0, (int)(s_now - t0));
    TEST_ASSERT_EQUAL(0, t.waited_ms);
}

/* The wait ends as soon as the slowest close is done, not at the bound. */
static void test_wait_ends_when_the_last_is_idle(void)
{
    lg_token_t t = lg_close(false, LG_CLOSE_MS);
    TEST_ASSERT_TRUE(t.idle);
    TEST_ASSERT_TRUE(t.waited_ms >= 50 && t.waited_ms < 100);
}

/* Tailscale off: only sessions over a tailnet address are closed; a LAN link stays up. */
static void test_tailnet_only_leaves_lan_sessions(void)
{
    s_term.tailnet = true;   /* a terminal to a tailnet host; the link is on the LAN */
    lg_token_t t = lg_close(true, LG_CLOSE_MS);
    TEST_ASSERT_TRUE(t.idle);
    TEST_ASSERT_TRUE(s_link.open);
    TEST_ASSERT_FALSE(s_term.open);
    TEST_ASSERT_EQUAL(0, t.took);   /* the link was not parked, so nothing to reopen */
    lg_reopen(t);
    TEST_ASSERT_EQUAL(0, s_link.reopened);
}

/* Reopen undoes only the parks this close took. */
static void test_reopen_only_what_was_taken(void)
{
    lg_token_t t = lg_close(false, LG_CLOSE_MS);
    TEST_ASSERT_EQUAL(1, t.took);   /* bit 0: the link */
    lg_reopen(t);
    lg_reopen((lg_token_t){ 0 });
    TEST_ASSERT_EQUAL(1, s_link.reopened);
}

static void test_register_is_bounded(void)
{
    lg_closer_t c = { "x", f_close, f_idle, NULL, &s_link };
    TEST_ASSERT_TRUE(lg_register(&c));
    TEST_ASSERT_TRUE(lg_register(&c));
    TEST_ASSERT_FALSE(lg_register(&c));   /* LG_MAX 4 */
}

/* No clock set (too early in boot): close asks once, never spins. */
static void test_no_clock_never_spins(void)
{
    lg_set_clock(NULL);
    lg_token_t t = lg_close(false, LG_CLOSE_MS);
    TEST_ASSERT_FALSE(t.idle);
    TEST_ASSERT_EQUAL_STRING("LT", s_log);
}

/* ---- the link's park state (cc_park) ---- */

static void test_park_owners_are_independent(void)
{
    cc_park_t p = { 0 };
    TEST_ASSERT_FALSE(cc_park_on(&p));
    cc_park_set_radio(&p, true);
    cc_park_guard_take(&p);
    cc_park_guard_drop(&p);
    TEST_ASSERT_TRUE(cc_park_on(&p));    /* a Wi-Fi leave's reopen never unparks Radio off */
    cc_park_set_radio(&p, false);
    TEST_ASSERT_FALSE(cc_park_on(&p));
    cc_park_guard_take(&p);
    cc_park_set_radio(&p, true);
    cc_park_set_radio(&p, false);
    TEST_ASSERT_TRUE(cc_park_on(&p));    /* nor a radio unpark a guard's close */
    cc_park_guard_drop(&p);
    TEST_ASSERT_FALSE(cc_park_on(&p));
}

static void test_park_guard_nests_and_never_underflows(void)
{
    cc_park_t p = { 0 };
    cc_park_guard_take(&p);
    cc_park_guard_take(&p);   /* a console restart while a Wi-Fi leave closes */
    cc_park_guard_drop(&p);
    TEST_ASSERT_TRUE(cc_park_on(&p));
    cc_park_guard_drop(&p);
    cc_park_guard_drop(&p);
    TEST_ASSERT_FALSE(cc_park_on(&p));
    cc_park_guard_take(&p);
    TEST_ASSERT_TRUE(cc_park_on(&p));
}

/* I1: a connect that finishes auth after the park is closed, not kept; work in flight gives up. */
static void test_park_after_auth_drops_the_new_session(void)
{
    cc_park_t p = { 0 };
    TEST_ASSERT_TRUE(cc_park_keep_new_session(&p));
    TEST_ASSERT_FALSE(cc_park_abort_work(&p));
    cc_park_set_radio(&p, true);
    TEST_ASSERT_FALSE(cc_park_keep_new_session(&p));
    TEST_ASSERT_TRUE(cc_park_abort_work(&p));
    cc_park_set_radio(&p, false);
    cc_park_guard_take(&p);
    TEST_ASSERT_FALSE(cc_park_keep_new_session(&p));
    TEST_ASSERT_TRUE(cc_park_abort_work(&p));
}

/* I1: idle means no session and no connect in flight (a connect counts even before it has a session). */
static void test_idle_counts_a_connect_in_flight(void)
{
    TEST_ASSERT_TRUE(cc_park_idle(false, false));
    TEST_ASSERT_FALSE(cc_park_idle(false, true));
    TEST_ASSERT_FALSE(cc_park_idle(true, false));
    TEST_ASSERT_FALSE(cc_park_idle(true, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_close_runs_before_down_and_waits_for_idle);
    RUN_TEST(test_no_reopen_when_going_down_for_good);
    RUN_TEST(test_wait_is_bounded_and_down_still_runs);
    RUN_TEST(test_nothing_open_returns_at_once);
    RUN_TEST(test_wait_ends_when_the_last_is_idle);
    RUN_TEST(test_tailnet_only_leaves_lan_sessions);
    RUN_TEST(test_reopen_only_what_was_taken);
    RUN_TEST(test_register_is_bounded);
    RUN_TEST(test_no_clock_never_spins);
    RUN_TEST(test_park_owners_are_independent);
    RUN_TEST(test_park_guard_nests_and_never_underflows);
    RUN_TEST(test_park_after_auth_drops_the_new_session);
    RUN_TEST(test_idle_counts_a_connect_in_flight);
    return UNITY_END();
}
