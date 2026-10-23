#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Parser for the long-poll watch loop's output (cc_cmd_watch). Pure: no ESP-IDF headers.
 * The loop prints, one per line (the pty turns \n into \r\n):
 *   @@CC-WATCH 1            banner, once at start
 *   @@CC-BEGIN <rc>         a block: `agents` output follows, then
 *   @@CC-END
 *   @@CC-HB <seconds>       heartbeat when nothing was sent for H seconds
 *   @@CC-BYE eof|quiet|life the loop is exiting, and why */

typedef enum { CC_WATCH_NONE, CC_WATCH_BANNER, CC_WATCH_BLOCK, CC_WATCH_HEARTBEAT,
               CC_WATCH_BYE, CC_WATCH_OVERFLOW } cc_watch_ev_t;

#define CC_WATCH_LINE_MAX 32   /* marker lines are shorter; longer lines are never markers */

typedef struct {
    char *body; size_t cap, len;   /* the watch's own PSRAM buffer of JSON_CAP; NUL-terminated at BLOCK */
    int rc;                        /* BLOCK: the loop's exit status for that run */
    char bye[16];                  /* BYE: "eof" | "quiet" | "life" */
    /* private line state follows */
    bool in_block;                 /* between BEGIN and END */
    bool over;                     /* this block's body went over cap */
    char line[CC_WATCH_LINE_MAX];  /* the current line's first bytes, for marker matching */
    size_t line_len;               /* the current line's full length so far (may exceed the buffer) */
    size_t line_start;             /* in a block: where the current line starts in body */
} cc_watch_t;

void cc_watch_init(cc_watch_t *w, char *body, size_t cap);
/* Consumes bytes up to and including the first complete event and returns it (*used = bytes
   consumed); NONE when data ran out first. \r is dropped. Marker lines must match exactly.
   Lines outside a block that aren't markers (pty echo, blanks) are ignored. A body over cap
   gives OVERFLOW at its END line instead of BLOCK. A body line of any length works. */
cc_watch_ev_t cc_watch_feed(cc_watch_t *w, const char *data, size_t n, size_t *used);
#ifdef __cplusplus
}
#endif
