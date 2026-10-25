/* See cc_tailbuf.h. */
#include "cc_tailbuf.h"

void cc_tailbuf_init(cc_tailbuf_t *t, char *buf, size_t cap, bool from_clear)
{
    *t = (cc_tailbuf_t){ .buf = buf, .room = cap ? cap - 1 : 0, .from_clear = from_clear };
    if (buf && cap) buf[0] = 0;
}

static void put1(cc_tailbuf_t *t, char c)
{
    if (!t->room) { t->dropped = true; return; }
    if (t->len < t->room) {
        t->buf[(t->head + t->len) % t->room] = c;
        t->len++;
    } else {   /* full: the newest byte replaces the oldest */
        t->buf[t->head] = c;
        t->head = (t->head + 1) % t->room;
        t->dropped = true;
    }
}

void cc_tailbuf_put(cc_tailbuf_t *t, const char *p, size_t n)
{
    static const char clr[4] = { 0x1b, '[', '2', 'J' };
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        put1(t, c);
        if (!t->from_clear) continue;
        if (c == clr[t->match]) {
            if (++t->match == 4) {   /* a full clear: start again from it */
                t->match = 0;
                t->head = t->len = 0;
                t->dropped = false;
                t->cleared = true;
                for (int k = 0; k < 4; k++) put1(t, clr[k]);
            }
        } else {
            t->match = c == clr[0] ? 1 : 0;
        }
    }
}

static void reverse(char *a, size_t i, size_t j)   /* [i, j) */
{
    while (i + 1 < j) { char x = a[i]; a[i] = a[--j]; a[j] = x; i++; }
}

size_t cc_tailbuf_finish(cc_tailbuf_t *t)
{
    if (!t->buf) return 0;
    if (t->head) {   /* rotate left by head: three reversals */
        reverse(t->buf, 0, t->head);
        reverse(t->buf, t->head, t->len);
        reverse(t->buf, 0, t->len);
        t->head = 0;
    }
    t->buf[t->len] = 0;
    return t->len;
}
