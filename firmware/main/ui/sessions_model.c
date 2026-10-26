#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/sessions_model.h"
#include "i18n/i18n.h"
#include "term/vt.h"   /* vt_wcwidth (term/wcwidth.c): the terminal's cell widths */

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
        if (cc_session_needs_you(s)) return tr(STR_PILL_NEEDS_YOU);
        return tr(s->background ? STR_PILL_ENDED : STR_PILL_WAITING);
    case CC_WORKING: return tr(STR_PILL_WORKING);
    case CC_IDLE:    return tr(STR_PILL_IDLE);
    case CC_DONE:    return tr(STR_PILL_DONE);
    default:         return tr(STR_PILL_ENDED);
    }
}

void sm_meta(const cc_session_t *s, const char *age, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!s) return;
    bool aged = age && age[0];
    if (s->background) snprintf(out, cap, aged ? "%s \xc2\xb7 %s" : "%s", s->repo, aged ? age : "");
    /* first, so a row too narrow for the whole meta still shows it */
    else if (aged) snprintf(out, cap, "%s \xc2\xb7 %s \xc2\xb7 %s", tr(STR_SES_ON_HOST), s->repo, age);
    else snprintf(out, cap, "%s \xc2\xb7 %s", tr(STR_SES_ON_HOST), s->repo);
}

int sm_hide_finished_tests(cc_session_t *s, int n)
{
    if (!s || n <= 0) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        bool finished = s[i].state == CC_DONE || s[i].state == CC_ENDED;
        if (finished && !strncmp(s[i].name, SM_TEST_PREFIX, strlen(SM_TEST_PREFIX))) continue;
        if (!s[i].background) continue;   /* a plain `claude` in a terminal: the Tab5 can't open it (user, 2026-10-24) */
        if (k != i) s[k] = s[i];
        k++;
    }
    return k;
}

/* ------------------------------------------------------------ pinned (stopped here) */
void sm_pin_add(sm_pins_t *p, const char *key)
{
    if (!p || !key || !key[0]) return;
    int slot = -1;
    for (int i = 0; i < SM_PINS && slot < 0; i++) if (p->key[i][0] && !strcmp(p->key[i], key)) slot = i;
    for (int i = 0; i < SM_PINS && slot < 0; i++) if (!p->key[i][0]) slot = i;
    if (slot < 0) {   /* full: the oldest goes */
        slot = 0;
        for (int i = 1; i < SM_PINS; i++) if (p->used[i] < p->used[slot]) slot = i;
    }
    if (strcmp(p->key[slot], key)) p->seen_stopped[slot] = false;
    snprintf(p->key[slot], sizeof(p->key[slot]), "%s", key);
    p->used[slot] = ++p->clock;
}

bool sm_pinned(const sm_pins_t *p, const char *key)
{
    if (!p || !key || !key[0]) return false;
    for (int i = 0; i < SM_PINS; i++) if (p->key[i][0] && !strcmp(p->key[i], key)) return true;
    return false;
}

void sm_pins_prune(sm_pins_t *p, const cc_session_t *items, int n)
{
    if (!p) return;
    char k[SM_KEY_MAX];
    for (int i = 0; i < SM_PINS; i++) {
        if (!p->key[i][0]) continue;
        bool keep = false;
        for (int j = 0; j < n; j++) {
            sm_key(&items[j], k, sizeof(k));
            if (strcmp(k, p->key[i])) continue;
            if (!items[j].live) p->seen_stopped[i] = true;
            keep = !(items[j].live && p->seen_stopped[i]);
            break;
        }
        if (!keep) { p->key[i][0] = 0; p->used[i] = 0; p->seen_stopped[i] = false; }
    }
}

int sm_visible(cc_session_t *s, int n, int max_finished, sm_pins_t *pins)
{
    if (!s || n <= 0) return 0;
    sm_pins_prune(pins, s, n);
    char k[SM_KEY_MAX];
    /* pass 1: what sm_hide_finished_tests drops, a pinned one excepted */
    int w = 0;
    for (int i = 0; i < n; i++) {
        bool finished = s[i].state == CC_DONE || s[i].state == CC_ENDED;
        sm_key(&s[i], k, sizeof(k));
        bool pinned = s[i].background && sm_pinned(pins, k);
        if (!s[i].background) continue;
        if (!pinned && finished && !strncmp(s[i].name, SM_TEST_PREFIX, strlen(SM_TEST_PREFIX))) continue;
        if (w != i) s[w] = s[i];
        w++;
    }
    n = w;
    /* pass 2: the finished limit over the unpinned finished ones (cc_sessions_visible's ranking) */
    bool *drop = calloc((size_t)n, sizeof(bool));
    if (!drop) return cc_sessions_visible(s, n, max_finished);
    for (int i = 0; i < n; i++) {
        bool fi = s[i].state == CC_DONE || s[i].state == CC_ENDED;
        sm_key(&s[i], k, sizeof(k));
        if (!fi || sm_pinned(pins, k)) continue;
        int newer = 0;
        for (int j = 0; j < n; j++) {
            if (j == i || !(s[j].state == CC_DONE || s[j].state == CC_ENDED)) continue;
            sm_key(&s[j], k, sizeof(k));
            if (sm_pinned(pins, k)) continue;
            if (s[j].started_ms > s[i].started_ms || (s[j].started_ms == s[i].started_ms && j < i)) newer++;
        }
        drop[i] = newer >= max_finished;
    }
    w = 0;
    for (int i = 0; i < n; i++) {
        if (drop[i]) continue;
        if (w != i) s[w] = s[i];
        w++;
    }
    free(drop);
    return w;
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

/* One UTF-8 character at *p (before end): its code point, and *p moves past it. */
static uint32_t utf8_next(const char **p, const char *end)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp = *s++;
    int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (more) cp &= 0x3F >> more;
    while (more-- && (const char *)s < end && is_cont(*s)) cp = (cp << 6) | (*s++ & 0x3F);
    while ((const char *)s < end && is_cont(*s)) s++;   /* stray continuation bytes stay with it */
    *p = (const char *)s;
    return cp;
}

/* Next wrapped row of the line [*p, end), at most `width` cells: returns its byte length and advances
   *p. A wide character (2 cells) that would overflow the row starts the next one; a row always takes at
   least one character, so the wrap ends however narrow it is. cells(cp) is a character's width. */
static int term_cells(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp < 0x20 ? 1 : vt_wcwidth(cp);
}

static size_t next_row(const char **p, const char *end, int width, sm_cells_fn cells_of, void *ctx)
{
    const char *s = *p;
    int cells = 0;
    while (s < end) {
        const char *q = s;
        uint32_t cp = utf8_next(&q, end);
        int w = cells_of(cp, ctx);
        if (cells > 0 && cells + w > width) break;
        cells += w;
        s = q;
    }
    size_t len = (size_t)(s - *p);
    *p = s;
    return len;
}

/* One pass over the rows: counts them when out is NULL, else writes all but the first `skip`.
   Returns the rows seen/written, or -1 when out is too small. */
static int wrap_pass(const char *in, int cols, int skip, char *out, size_t cap, sm_cells_fn fn, void *ctx)
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
            size_t len = next_row(&q, end, first ? cols : cols - 2, fn, ctx);
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
    return sm_wrap_tail_fn(in, cols, rows, out, cap, NULL, NULL);
}

int sm_wrap_tail_fn(const char *in, int cols, int rows, char *out, size_t cap, sm_cells_fn fn, void *ctx)
{
    if (!fn) fn = term_cells;
    if (!cap) return 0;
    out[0] = 0;
    if (!in || rows <= 0) return 0;
    if (cols < 3) cols = 3;
    int total = wrap_pass(in, cols, 0, NULL, 0, fn, ctx);
    int skip = total > rows ? total - rows : 0;
    /* a short buffer drops the oldest rows first, never a row in the middle */
    for (;; skip++) {
        out[0] = 0;
        int n = wrap_pass(in, cols, skip, out, cap, fn, ctx);
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

bool sm_picks_hint(cc_state_t state, int options)
{
    return options >= 2 && (state == CC_NEEDS_YOU || state == CC_IDLE);
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
