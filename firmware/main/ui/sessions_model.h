#pragma once
/* Sessions screen logic, pure (companion/cc_sessions.h and term/vt.h's vt_wcwidth only): row keys, selection and drafts that
 * follow a session across polls, the banner order, the low-battery rule and the log wrap.
 * Host-tested in firmware/test/host/test_sessions_model.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "companion/cc_sessions.h"

#define SM_KEY_MAX 128
#define SM_DRAFTS 16
#define SM_ANSWER_MAX 480
#define SM_UNREACHABLE_MS 30000

/* The row key: a background session's id; otherwise "i:" + name + ":" + started_ms (B3). */
void sm_key(const cc_session_t *s, char *out, size_t cap);
/* Index whose key equals sel_key; else prev_index clamped to [0, n-1]; -1 when n == 0. */
int sm_select(const cc_session_t *items, int n, const char *sel_key, int prev_index);
/* Next index after sel (wrapping) that needs you; none -> (sel + 1) % n; -1 when n == 0. */
int sm_next_needs_you(const cc_session_t *items, int n, int sel);

typedef struct { char key[SM_KEY_MAX]; char text[SM_ANSWER_MAX + 1]; uint32_t used; } sm_draft_t;
typedef struct { sm_draft_t d[SM_DRAFTS]; uint32_t clock; } sm_drafts_t;   /* the caller allocates it in PSRAM */
const char *sm_draft_get(const sm_drafts_t *ds, const char *key);          /* "" when none */
void sm_draft_set(sm_drafts_t *ds, const char *key, const char *text);     /* "" clears; full -> evict the least recently set */
void sm_drafts_prune(sm_drafts_t *ds, const cc_session_t *items, int n);   /* drop drafts whose key matches no item */

const char *sm_pill_text(const cc_session_t *s);
/* The row meta: "repo · age" (age NULL or "": "repo"); an interactive session, which runs in a terminal on the
   Host and can't be opened from the Tab5 (P4C-11), leads with "on Host · " so a narrow row still shows it. */
void sm_meta(const cc_session_t *s, const char *age, char *out, size_t cap);
/* Compacts in place, dropping what the list doesn't show: interactive sessions (a plain `claude` in a terminal,
   which the Tab5 can't open), and (P4C-11) the DONE and ENDED sessions named "companion-test-*" (the agents' test
   sessions). Every other session, live test sessions included, stays in order. Returns the new count. */
#define SM_TEST_PREFIX "companion-test-"
int sm_hide_finished_tests(cc_session_t *s, int n);

typedef enum { SM_B_NO_WIFI, SM_B_NO_HOST, SM_B_UNREACHABLE, SM_B_LOW_BATTERY, SM_B_RECONNECTING,
               SM_B_LOADING, SM_B_STALE, SM_B_EMPTY, SM_B_LIVE } sm_banner_t;
typedef struct {
    bool host_set, wifi_up, wifi_connecting, link_up, link_failed, poll_failing, reconnecting, waking,
         stale, have_list, low_battery_shown;
    int64_t down_ms;
    int n;
} sm_in_t;
sm_banner_t sm_banner(const sm_in_t *in);

typedef struct { bool armed, shown, dismissed; } sm_lowbat_t;   /* zero-initialised is the start state */
/* True while the low-battery banner shows: once per discharge at or below 15 % while not charging,
   until a dismiss tap. Charging or above 15 % re-arms. pct < 0 never shows. */
bool sm_lowbat_step(sm_lowbat_t *st, int pct, bool charging, bool dismiss);

/* Wrap `in` to `cols` 12 px cells per row (continuations indented 2 cells), keep the newest `rows`
   rows, joined by '\n' into out. Returns the row count. A cell is the terminal's: vt_wcwidth gives 2
   for CJK and other wide characters (FONT_MONO draws them 24 px wide), 0 for combining marks. */
int sm_wrap_tail(const char *in, int cols, int rows, char *out, size_t cap);
/* The same with each character's width in cells from fn(cp, ctx) (at least 0; NULL: the terminal's
   rule above). Sessions passes FONT_MONO's own advances, since a label draws some TUI symbols wider
   than the terminal's grid does. */
typedef int (*sm_cells_fn)(uint32_t cp, void *ctx);
int sm_wrap_tail_fn(const char *in, int cols, int rows, char *out, size_t cap, sm_cells_fn fn, void *ctx);
/* The highest number of a trailing numbered list of at least 2 items (^\s*(❯\s*)?\d+\.), else 0. */
int sm_tail_options(const char *tail);
