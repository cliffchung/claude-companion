#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { CC_NEEDS_YOU, CC_WORKING, CC_IDLE, CC_DONE, CC_ENDED } cc_state_t;

typedef struct {
    char id[16];            /* short id: attach / logs; "" unless background */
    char session_id[40];    /* UUID: -p --resume; "" unless background */
    char name[96];          /* display key: "name", else the repo */
    char repo[64];          /* basename of cwd, trailing slash ignored */
    cc_state_t state;
    bool background;        /* kind == "background": can be answered and opened */
    bool live;              /* has a pid: the process runs (a stopped entry has none) */
    int64_t started_ms;
} cc_session_t;

/* Returns the count (<= cap), or -1 if the input isn't a JSON array.
   status "waiting" or a non-empty "waitingFor" (a question on screen) -> NEEDS_YOU, whatever "state" says.
   Then the live process's status wins over a stale state: busy -> WORKING; idle -> DONE if state is "done",
   else IDLE. Otherwise (no status: no live process) state comes from "state" if present, else idle:
     waiting -> NEEDS_YOU;  blocked -> IDLE with a pid (the turn ended, waiting for your next message),
     ENDED without one (it stopped while it waited);
     working / busy -> WORKING;  idle -> IDLE;
     done -> DONE;  failed / stopped -> ENDED;  anything else -> IDLE.
   Background entries whose id or sessionId fails validation are skipped; other kinds
   (interactive) are kept with empty ids and background = false.
   Order: NEEDS_YOU, WORKING, IDLE, DONE, ENDED; newest startedAt first within each state. */
int cc_sessions_parse(const char *json, cc_session_t *out, int cap);
/* The one definition of "needs you": a background session with a live process that is waiting on a question.
   Used by cc_summary, the dashboard's pill and amber styling, and the keyboard light. A stopped
   or interactive session that is waiting does not count. */
bool cc_session_needs_you(const cc_session_t *s);
/* Compacts in place: every active session plus the newest max_finished DONE/ENDED; keeps order. */
int cc_sessions_visible(cc_session_t *s, int n, int max_finished);
/* "2 need you · 1 working", "1 needs you", "3 working", or "All quiet" */
size_t cc_summary(const cc_session_t *s, int n, char *out, size_t cap);
/* under 60 s "45s", under 60 min "12m", under 24 h "3h", otherwise "2d" */
size_t cc_age(int64_t now_ms, int64_t then_ms, char *out, size_t cap);
