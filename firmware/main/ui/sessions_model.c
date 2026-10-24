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
    size_t n = strlen(d->text);
    if (n == sizeof(d->text) - 1 && strlen(text) > n) {   /* a cut must not leave half a UTF-8 sequence */
        size_t k = n;
        while (k > 0 && (((unsigned char)d->text[k - 1]) & 0xC0) == 0x80) k--;
        if (k > 0) {
            unsigned char lead = (unsigned char)d->text[k - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if ((k - 1) + need > n) d->text[k - 1] = 0;
        }
    }
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
    /* §5.1 order. While Wi-Fi is still associating (a wake) it is not "No Wi-Fi": the link counts as down. */
    if (!in->wifi_up && !in->wifi_connecting) return SM_B_NO_WIFI;
    if (!in->host_set) return SM_B_NO_HOST;
    if (in->link_failed || in->poll_failing || in->down_ms >= SM_UNREACHABLE_MS) return SM_B_UNREACHABLE;
    if (in->low_battery_shown) return SM_B_LOW_BATTERY;
    bool down = !in->wifi_up || !in->link_up || in->down_ms > 0;
    if (in->reconnecting || in->waking) return SM_B_RECONNECTING;
    if (!in->have_list) return SM_B_LOADING;
    if (down) return SM_B_RECONNECTING;
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

/* One pass over the rows: counts them when out is NULL, else writes all but the first `skip`.
   Returns the rows seen/written, or -1 when out is too small. */
static int wrap_pass(const char *in, int cols, int skip, char *out, size_t cap)
{
    const char *p = in, *stop = in + strlen(in);
    int count = 0, emitted = 0;
    size_t o = 0;
    while (p < stop) {
        const char *nl = memchr(p, '\n', (size_t)(stop - p));
        const char *end = nl ? nl : stop;
        const char *next = nl ? nl + 1 : stop;
        if (end > p && end[-1] == '\r') end--;   /* CRLF */
        const char *q = p;
        bool first = true;
        do {
            size_t len = next_row(&q, end, first ? cols : cols - 2);
            count++;
            if (out && count > skip) {
                size_t ind = first ? 0 : 2;
                if (o + (emitted ? 1 : 0) + ind + len + 1 > cap) return -1;
                if (emitted) out[o++] = '\n';
                memset(out + o, ' ', ind); o += ind;
                memcpy(out + o, q - len, len); o += len;
                out[o] = 0;
                emitted++;
            }
            first = false;
        } while (q < end);
        p = next;
    }
    return out ? emitted : count;
}

int sm_wrap_tail(const char *in, int cols, int rows, char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = 0;
    if (!in || rows <= 0) return 0;
    if (cols < 3) cols = 3;
    int total = wrap_pass(in, cols, 0, NULL, 0);
    int skip = total > rows ? total - rows : 0;
    /* a short buffer drops the oldest rows first, never a row in the middle */
    for (;; skip++) {
        out[0] = 0;
        int n = wrap_pass(in, cols, skip, out, cap);
        if (n >= 0) return n;
    }
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
