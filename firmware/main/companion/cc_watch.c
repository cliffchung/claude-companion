#include "cc_watch.h"

#include <string.h>

/* Line-by-line, byte at a time, so a feed can stop at any byte and a body line of any length
 * goes straight into body. Only a line's first CC_WATCH_LINE_MAX - 1 bytes are kept apart for
 * marker matching; longer lines are never markers. Inside a block every byte also goes into
 * body; the END line's bytes are taken back off when it turns out to be END. */

void cc_watch_init(cc_watch_t *w, char *body, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->body = body;
    w->cap = cap;
    if (body && cap) body[0] = 0;
}

static bool digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

/* The current line, if it is short enough to be a marker; NULL otherwise. */
static const char *short_line(cc_watch_t *w)
{
    if (w->line_len >= CC_WATCH_LINE_MAX) return NULL;
    w->line[w->line_len] = 0;
    return w->line;
}

static cc_watch_ev_t end_line_in_block(cc_watch_t *w)
{
    const char *l = short_line(w);
    if (l && strcmp(l, "@@CC-END") == 0) {
        w->in_block = false;
        if (w->over) return CC_WATCH_OVERFLOW;
        w->len = w->line_start;   /* drop the END line's bytes */
        w->body[w->len] = 0;
        return CC_WATCH_BLOCK;
    }
    /* a body line: its bytes are in body already (when they fit); add the \n */
    if (w->over || w->line_start + w->line_len + 1 > w->cap - 1) {
        w->over = true;
    } else {
        w->len = w->line_start + w->line_len;
        w->body[w->len++] = '\n';
        w->line_start = w->len;
    }
    return CC_WATCH_NONE;
}

static cc_watch_ev_t end_line_outside(cc_watch_t *w)
{
    const char *l = short_line(w);
    if (!l) return CC_WATCH_NONE;
    if (strcmp(l, "@@CC-WATCH 1") == 0) return CC_WATCH_BANNER;
    if (strncmp(l, "@@CC-BEGIN ", 11) == 0 && digits(l + 11) && strlen(l + 11) <= 3) {
        int rc = 0;
        for (const char *p = l + 11; *p; p++) rc = rc * 10 + (*p - '0');
        w->rc = rc;
        w->in_block = true;
        w->over = false;
        w->len = w->line_start = 0;
        w->body[0] = 0;
        return CC_WATCH_NONE;
    }
    if (strncmp(l, "@@CC-HB ", 8) == 0 && digits(l + 8)) return CC_WATCH_HEARTBEAT;
    if (strncmp(l, "@@CC-BYE ", 9) == 0) {
        const char *why = l + 9;
        if (strcmp(why, "eof") == 0 || strcmp(why, "quiet") == 0 || strcmp(why, "life") == 0) {
            strlcpy(w->bye, why, sizeof(w->bye));
            return CC_WATCH_BYE;
        }
    }
    return CC_WATCH_NONE;   /* pty echo, blanks, anything else */
}

cc_watch_ev_t cc_watch_feed(cc_watch_t *w, const char *data, size_t n, size_t *used)
{
    size_t i = 0;
    cc_watch_ev_t ev = CC_WATCH_NONE;
    if (!w || !w->body || w->cap == 0 || !data) {
        if (used) *used = n;
        return CC_WATCH_NONE;
    }
    while (i < n && ev == CC_WATCH_NONE) {
        char c = data[i++];
        if (c == '\r') continue;
        if (c == '\n') {
            ev = w->in_block ? end_line_in_block(w) : end_line_outside(w);
            w->line_len = 0;
            continue;
        }
        if (w->line_len < CC_WATCH_LINE_MAX - 1) w->line[w->line_len] = c;
        if (w->in_block && !w->over) {
            size_t at = w->line_start + w->line_len;
            if (at < w->cap - 1) w->body[at] = c;
        }
        w->line_len++;
    }
    if (used) *used = i;
    return ev;
}
