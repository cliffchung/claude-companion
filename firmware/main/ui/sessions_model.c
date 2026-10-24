#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/sessions_model.h"

void sm_key(const cc_session_t *s, char *out, size_t cap)
{
    if (!cap) return;
    if (s->background && s->id[0]) snprintf(out, cap, "%s", s->id);
    else snprintf(out, cap, "i:%s:%lld", s->name, (long long)s->started_ms);
}

int sm_select(const cc_session_t *items, int n, const char *sel_key, int prev_index)
{
    if (n <= 0) return -1;
    if (sel_key && sel_key[0]) {
        char k[SM_KEY_MAX];
        for (int i = 0; i < n; i++) {
            sm_key(&items[i], k, sizeof(k));
            if (!strcmp(k, sel_key)) return i;
        }
    }
    if (prev_index < 0) return 0;
    return prev_index >= n ? n - 1 : prev_index;
}

int sm_next_needs_you(const cc_session_t *items, int n, int sel)
{
    if (n <= 0) return -1;
    for (int step = 1; step <= n; step++) {
        int i = ((sel + step) % n + n) % n;
        if (cc_session_needs_you(&items[i])) return i;
    }
    return ((sel + 1) % n + n) % n;
}

/* ------------------------------------------------------------ drafts */
static sm_draft_t *find(sm_drafts_t *ds, const char *key)
{
    for (int i = 0; i < SM_DRAFTS; i++)
        if (ds->d[i].key[0] && !strcmp(ds->d[i].key, key)) return &ds->d[i];
    return NULL;
}

const char *sm_draft_get(const sm_drafts_t *ds, const char *key)
{
    if (!ds || !key || !key[0]) return "";
    const sm_draft_t *d = find((sm_drafts_t *)ds, key);
    return d ? d->text : "";
}

void sm_draft_set(sm_drafts_t *ds, const char *key, const char *text)
{
    if (!ds || !key || !key[0]) return;
    sm_draft_t *d = find(ds, key);
    if (!text || !text[0]) {
        if (d) memset(d, 0, sizeof(*d));
        return;
    }
    if (!d) {
        for (int i = 0; i < SM_DRAFTS && !d; i++)
            if (!ds->d[i].key[0]) d = &ds->d[i];
        if (!d) {
            d = &ds->d[0];
            for (int i = 1; i < SM_DRAFTS; i++)
                if (ds->d[i].used < d->used) d = &ds->d[i];
        }
        memset(d, 0, sizeof(*d));
        snprintf(d->key, sizeof(d->key), "%s", key);
    }
    snprintf(d->text, sizeof(d->text), "%s", text);
    d->used = ++ds->clock;
}

void sm_drafts_prune(sm_drafts_t *ds, const cc_session_t *items, int n)
{
    char k[SM_KEY_MAX];
    for (int i = 0; i < SM_DRAFTS; i++) {
        if (!ds->d[i].key[0]) continue;
        bool keep = false;
        for (int j = 0; j < n && !keep; j++) {
            sm_key(&items[j], k, sizeof(k));
            keep = !strcmp(k, ds->d[i].key);
        }
        if (!keep) memset(&ds->d[i], 0, sizeof(ds->d[i]));
    }
}

/* ------------------------------------------------------------ pill, banner, battery */
const char *sm_pill_text(const cc_session_t *s)
{
    switch (s->state) {
    case CC_NEEDS_YOU:
        if (cc_session_needs_you(s)) return "NEEDS YOU";
        return s->background ? "ENDED" : "waiting on Host";
    case CC_WORKING: return "WORKING";
    case CC_IDLE:    return "IDLE";
    case CC_DONE:    return "DONE";
    default:         return "ENDED";
    }
}

sm_banner_t sm_banner(const sm_in_t *in)
{
    if (!in->wifi_up) return SM_B_NO_WIFI;
    if (!in->host_set) return SM_B_NO_HOST;
    if (in->link_failed || in->poll_failing || in->down_ms >= SM_UNREACHABLE_MS) return SM_B_UNREACHABLE;
    bool down = !in->link_up || in->down_ms > 0;
    if (in->low_battery_shown && !down) return SM_B_LOW_BATTERY;
    if (in->reconnecting || in->waking) return SM_B_RECONNECTING;
    if (down) return in->have_list ? SM_B_RECONNECTING : SM_B_LOADING;
    if (!in->have_list) return SM_B_LOADING;
    if (in->stale) return SM_B_STALE;
    if (in->n == 0) return SM_B_EMPTY;
    return SM_B_LIVE;
}

bool sm_lowbat_step(sm_lowbat_t *st, int pct, bool charging, bool dismiss)
{
    if (pct < 0) { st->shown = false; return false; }
    if (charging || pct > 15) {   /* re-arm */
        st->armed = true; st->shown = false; st->dismissed = false;
        return false;
    }
    if (dismiss) st->dismissed = true;
    st->shown = !st->dismissed;
    st->armed = !st->dismissed;
    return st->shown;
}

/* ------------------------------------------------------------ wrap */
static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* Next wrapped row of the line [*p, end): returns its byte length and advances *p; *first is
   true for a line's first row. Continuations carry a 2-cell indent (emitted by the caller). */
static size_t next_row(const char **p, const char *end, int width)
{
    const char *s = *p;
    int chars = 0;
    while (s < end && chars < width) {
        s++;
        while (s < end && is_cont((unsigned char)*s)) s++;
        chars++;
    }
    size_t len = (size_t)(s - *p);
    *p = s;
    return len;
}

int sm_wrap_tail(const char *in, int cols, int rows, char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = 0;
    if (!in || rows <= 0) return 0;
    if (cols < 3) cols = 3;
    size_t total = strlen(in);
    /* pass 0 counts the rows, pass 1 emits all but the oldest */
    int count = 0, skip = 0, emitted = 0;
    size_t o = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = in, *stop = in + total;
        while (p < stop) {
            const char *nl = memchr(p, '\n', (size_t)(stop - p));
            const char *end = nl ? nl : stop;
            const char *q = p;
            bool first = true;
            do {
                int w = first ? cols : cols - 2;
                size_t len = next_row(&q, end, w);
                if (pass == 0) count++;
                else if (skip > 0) skip--;
                else {
                    size_t ind = first ? 0 : 2;
                    if (o + (emitted ? 1 : 0) + ind + len + 1 <= cap) {
                        if (emitted) out[o++] = '\n';
                        memset(out + o, ' ', ind); o += ind;
                        memcpy(out + o, q - len, len); o += len;
                        out[o] = 0;
                        emitted++;
                    }
                }
                first = false;
            } while (q < end);
            p = nl ? nl + 1 : stop;
        }
        if (pass == 0) skip = count > rows ? count - rows : 0;
    }
    return emitted;
}

/* ------------------------------------------------------------ options */
/* Parses one line; returns the list number, or 0 when the line isn't a numbered item. */
static int line_number(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (end - s >= 3 && !memcmp(s, "\xe2\x9d\xaf", 3)) {
        s += 3;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
    }
    int v = 0, d = 0;
    while (s < end && *s >= '0' && *s <= '9' && d < 4) { v = v * 10 + (*s - '0'); s++; d++; }
    return (d && s < end && *s == '.') ? v : 0;
}

int sm_tail_options(const char *tail)
{
    if (!tail) return 0;
    const char *end = tail + strlen(tail);
    int items = 0, hi = 0;
    while (end > tail) {
        const char *e = end;
        if (e[-1] == '\n') e--;
        const char *b = e;
        while (b > tail && b[-1] != '\n') b--;
        bool blank = true;
        for (const char *c = b; c < e; c++) if (*c != ' ' && *c != '\t' && *c != '\r') blank = false;
        if (blank) {
            if (items) break;
        } else {
            int v = line_number(b, e);
            if (!v) break;
            items++;
            if (v > hi) hi = v;
        }
        end = b;
    }
    return items >= 2 ? hi : 0;
}
