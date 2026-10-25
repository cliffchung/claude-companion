/* The Sessions output pane's text, read off a rendered screen of `claude logs` (see cc_logview.h). */
#include "cc_logview.h"
#include "term/vt.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ROWS_MIN     24
#define ROWS_MAX     160
#define ROWS_DEFAULT 60
#define COLS_MIN     80
#define COLS_MAX     320
#define COLS_NO_RULE 256
#define COLS_MARGIN  8
#define RULE_MIN     40    /* a '─' run this long is a frame rule */
#define RULE_CELLS   10    /* a row that starts with this many box-drawing cells is a rule */
#define RIGHT_MIN_X  24    /* a row starting past this column and reaching the right edge is right-aligned status */
#define WRAP_SLACK   4     /* a row this close to the width, with the next word not fitting, was wrapped */
#define RULE_NEAR    8     /* with no input box or dialog, a rule this close to the last drawn row ends the view */
#define HINT_SPAN    40    /* a dialog's options lie this close above its "Esc to cancel" hint */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool cc_logview_size(const char *raw, size_t len, int *cols, int *rows, size_t *start, bool *cleared)
{
    const unsigned char *b = (const unsigned char *)raw;
    size_t from = 0;
    bool clr = false;
    for (size_t i = 0; b && i + 3 < len; i++)
        if (b[i] == 0x1b && b[i + 1] == '[' && b[i + 2] == '2' && b[i + 3] == 'J') { from = i; clr = true; }
    int max_row = 0, max_col = 0, run = 0, best_run = 0;
    for (size_t i = from; b && i < len;) {
        if (b[i] == 0x1b && i + 1 < len && b[i + 1] == '[') {
            int p[2] = { 0, 0 }, np = 0;
            size_t j = i + 2;
            while (j < len && (b[j] < 0x40 || b[j] > 0x7e)) {
                if (b[j] >= '0' && b[j] <= '9') { if (np < 2 && p[np] < 10000) p[np] = p[np] * 10 + (b[j] - '0'); }
                else if (b[j] == ';' && np < 2) np++;
                j++;
            }
            if (j < len) {
                if (b[j] == 'H' || b[j] == 'f') {
                    if (p[0] > max_row) max_row = p[0];
                    if (p[1] > max_col) max_col = p[1];
                } else if (b[j] == 'B' || b[j] == 'E') {   /* moves down from home, in this renderer */
                    if (p[0] + 1 > max_row) max_row = p[0] + 1;
                } else if (b[j] == 'G') {
                    if (p[0] > max_col) max_col = p[0];
                }
                j++;
            }
            i = j;
            continue;   /* an escape between rule glyphs doesn't end the run */
        }
        if (i + 2 < len && b[i] == 0xe2 && b[i + 1] == 0x94 && b[i + 2] == 0x80) {
            if (++run > best_run) best_run = run;
            i += 3;
            continue;
        }
        run = 0;
        i++;
    }
    int c = best_run >= RULE_MIN ? best_run : COLS_NO_RULE;
    if (max_col > c) c = max_col;
    bool fits = c + COLS_MARGIN <= COLS_MAX && max_row <= ROWS_MAX;
    if (cols) *cols = clampi(c + COLS_MARGIN, COLS_MIN, COLS_MAX);
    if (rows) *rows = max_row ? clampi(max_row, ROWS_MIN, ROWS_MAX) : ROWS_DEFAULT;
    if (start) *start = from;
    if (cleared) *cleared = clr;
    return fits;
}

/* ------------------------------------------------------------ row classes */
typedef struct {
    vt_t *vt;
    int cols, rows;
    char *buf;            /* one row's text */
    size_t buf_cap;
} scr_t;

static const vt_cell_t *row_at(const scr_t *s, int y) { return vt_view_row(s->vt, y); }

/* a blank cell: a space, NBSP or never written; a wide character's second cell is not blank */
static bool cell_blank(const vt_cell_t *c)
{
    if (c->attr & VT_ATTR_WIDE_CONT) return false;
    return c->cp == ' ' || c->cp == 0 || c->cp == 0xa0;
}

static int first_x(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    for (int x = 0; x < s->cols; x++)
        if (!cell_blank(&r[x])) return x;
    return -1;
}

/* cells up to the last drawn one */
static int row_width(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    for (int x = s->cols - 1; x >= 0; x--)
        if (!cell_blank(&r[x])) return x + 1;
    return 0;
}

static const char *row_text(const scr_t *s, int y)
{
    vt_row_text(s->vt, y, s->buf, s->buf_cap);
    return s->buf;
}

static bool box_cp(uint32_t cp) { return cp >= 0x2500 && cp <= 0x257f; }

static bool is_rule(const scr_t *s, int y)
{
    int x0 = first_x(s, y);
    if (x0 < 0 || x0 > 1 || x0 + RULE_CELLS > s->cols) return false;
    const vt_cell_t *r = row_at(s, y);
    for (int x = x0; x < x0 + RULE_CELLS; x++)
        if (!box_cp(r[x].cp)) return false;
    return true;
}

static bool is_prompt(const scr_t *s, int y) { return row_at(s, y)[0].cp == 0x276f; }   /* ❯ at column 0 */

static bool is_banner(const scr_t *s, int y)   /* the welcome logo: block elements */
{
    int x0 = first_x(s, y);
    if (x0 < 0 || x0 > 2) return false;
    uint32_t cp = row_at(s, y)[x0].cp;
    return cp >= 0x2580 && cp <= 0x259f;
}

/* Claude Code's spinner line, at column 0: a spinner glyph, a space, a verb, then either "…" and an optional
   "(…)" tail ("✽ Pollinating… (2s · thinking)", "· Computing…") or " for " and a time ("✻ Baked for 5s · done").
   A reply's own "* Item" or "· note" doesn't match. */
static bool is_spinner(const scr_t *s, int y)
{
    uint32_t c = row_at(s, y)[0].cp;
    if (!(c == 0xb7 || c == 0x2722 || c == 0x2733 || c == 0x2736 || c == 0x273b || c == 0x273d || c == '*')) return false;
    const unsigned char *t = (const unsigned char *)row_text(s, y);
    t += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;   /* the glyph */
    if (*t++ != ' ') return false;
    const unsigned char *w = t;
    for (;;) {
        if ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z')) { t++; continue; }
        if (t[0] >= 0xc3 && t[0] <= 0xc5 && (t[1] & 0xc0) == 0x80) { t += 2; continue; }   /* é and other Latin letters */
        break;
    }
    if (t == w) return false;
    if (t[0] == 0xe2 && t[1] == 0x80 && t[2] == 0xa6) {   /* … */
        t += 3;
        while (*t == ' ') t++;
        if (!*t) return true;
        if (*t != '(') return false;
        const unsigned char *close = (const unsigned char *)strrchr((const char *)t, ')');
        if (!close) return false;
        for (t = close + 1; *t == ' '; t++) {}
        return !*t;
    }
    return !strncmp((const char *)t, " for ", 5) && t[5] >= '0' && t[5] <= '9';
}

static bool is_tip(const scr_t *s, int y)   /* "  ⎿  Tip: …" under the spinner */
{
    int x0 = first_x(s, y);
    return x0 >= 0 && x0 <= 4 && row_at(s, y)[x0].cp == 0x23bf;
}

static int first_word_width(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    int x0 = first_x(s, y), x = x0;
    if (x0 < 0) return 0;
    while (x < s->cols && !cell_blank(&r[x])) x++;
    return x - x0;
}

/* A numbered option or list item ("1. Yes", "❯ 2. No"): the column of its number, else -1. */
static int option_x(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    int x = first_x(s, y);
    if (x < 0) return -1;
    if (r[x].cp == 0x276f) { x++; while (x < s->cols && r[x].cp == ' ') x++; }
    int d = x;
    while (x < s->cols && x - d < 3 && r[x].cp >= '0' && r[x].cp <= '9') x++;
    return x > d && x < s->cols && r[x].cp == '.' ? d : -1;
}

/* A row that starts a new item, never a wrapped continuation: a numbered item, a bullet, a marker. */
static bool starts_item(const scr_t *s, int y)
{
    if (option_x(s, y) >= 0) return true;
    int x0 = first_x(s, y);
    if (x0 < 0) return false;
    const vt_cell_t *r = row_at(s, y);
    uint32_t c = r[x0].cp;
    bool spaced = x0 + 1 < s->cols && r[x0 + 1].cp == ' ';
    if (c == 0x23fa || c == 0x276f || c == 0x23bf) return true;   /* ⏺ ❯ ⎿ */
    return spaced && (c == '-' || c == '*' || c == '+' || c == '>' || c == 0x2022 || c == 0xb7 || c == 0x25cf);
}

static bool wide_at(const scr_t *s, int y, int x)
{
    if (x < 0 || x >= s->cols) return false;
    return (row_at(s, y)[x].attr & (VT_ATTR_WIDE | VT_ATTR_WIDE_CONT)) != 0;
}

static int last_drawn(const scr_t *s)
{
    int last = s->rows - 1;
    while (last >= 0 && first_x(s, last) < 0) last--;
    return last;
}

/* The input box's top rule: a rule, the "❯" input line under it (a draft may start "1."), and the box's bottom rule
   further down within RULE_NEAR rows of the last drawn row (only the footer is under it; a draft may be tall).
   Asked only when no dialog is up (dialog_hint), so a selector's "❯ 1." under its frame isn't taken for it. */
static int input_box_top(const scr_t *s)
{
    int last = last_drawn(s), bottom = -1;
    for (int k = last; k >= 0 && k >= last - RULE_NEAR; k--)
        if (is_rule(s, k)) { bottom = k; break; }
    for (int y = bottom - 2; y >= 0; y--)
        if (is_rule(s, y) && is_prompt(s, y + 1)) return y;
    return -1;
}

/* A dialog: the lowest drawn row (below it only blanks and rules) is its "Esc to cancel" hint, with at least two
   numbered options, one selected with ❯, above it. Returns the hint's row, else -1. */
static int dialog_hint(const scr_t *s)
{
    int y = s->rows - 1;
    while (y >= 0 && (first_x(s, y) < 0 || is_rule(s, y))) y--;
    if (y < 0) return -1;
    const char *t = row_text(s, y);
    if (!strstr(t, "Esc to cancel") && !strstr(t, "esc to cancel")) return -1;
    int options = 0;
    bool selected = false;
    for (int k = y - 1; k >= 0 && k >= y - HINT_SPAN; k--) {
        if (option_x(s, k) < 0) continue;
        options++;
        if (row_at(s, k)[first_x(s, k)].cp == 0x276f) selected = true;
    }
    return options >= 2 && selected ? y : -1;
}

/* The lowest rule within RULE_NEAR rows of the last drawn row, else -1. */
static int rule_near_bottom(const scr_t *s)
{
    int last = last_drawn(s);
    for (int y = last; y >= 0 && y >= last - RULE_NEAR; y--)
        if (is_rule(s, y)) return y;
    return -1;
}

/* ------------------------------------------------------------ the tail */
static int replay(vt_t *vt, const char *p, size_t n)
{
    vt_set_work_limit(vt, CC_LOGVIEW_WORK_MAX);   /* checked per byte inside vt_feed */
    vt_feed(vt, (const uint8_t *)p, n);
    return vt_work(vt) > CC_LOGVIEW_WORK_MAX ? CC_LV_TOO_COSTLY : 0;
}

int cc_logview_tail(const char *raw, size_t len, bool complete, int max_lines, char *out, size_t cap)
{
    if (!out || !cap) return CC_LV_NONE;
    out[0] = 0;
    if (!raw || !len || max_lines <= 0) return CC_LV_NONE;
    int cols, rows;
    size_t start;
    bool cleared;
    if (!cc_logview_size(raw, len, &cols, &rows, &start, &cleared)) return CC_LV_TOO_BIG;
    if (!cleared && !complete) return CC_LV_PARTIAL;   /* the front is gone and no clear since: sparse */
    size_t row_cap = (size_t)cols * 4 + 2, all_cap = (size_t)rows * (row_cap + 1) + 1;
    scr_t s = { vt_create(cols, rows, 0), cols, rows, heap_caps_malloc(row_cap, MALLOC_CAP_SPIRAM), row_cap };
    char *all = heap_caps_malloc(all_cap, MALLOC_CAP_SPIRAM);
    int kept = CC_LV_NO_MEMORY;
    if (!s.vt || !s.buf || !all) goto done;
    kept = replay(s.vt, raw + start, len - start);
    if (kept) goto done;

    /* where the conversation ends: a dialog's hint (first: a numbered draft is no dialog, a selector's "❯ 1." under
       its frame is no input box), else the input box, else a rule near the bottom */
    int top = 0, hint = dialog_hint(&s), end;
    bool dialog = hint >= 0;
    if (dialog) end = hint + 1;
    else if ((end = input_box_top(&s)) < 0) end = rule_near_bottom(&s);
    kept = CC_LV_NO_LAYOUT;
    if (end < 0) goto done;
    while (top < end && top < 4 && first_x(&s, top) < 0) top++;
    while (top < end && is_banner(&s, top)) top++;
    /* the screen's width, for telling a wrapped row: the widest frame rule, else the virtual width */
    int width = 0;
    for (int y = 0; y < rows; y++)
        if (is_rule(&s, y)) { int w = row_width(&s, y); if (w > width) width = w; }
    if (!width) width = cols;

    size_t o = 0;
    int prev = -2;            /* the last kept row */
    bool after_spinner = false;
    int opt = -1;             /* a dialog's last option: the column of its number */
    kept = 0;
    for (int y = top; y < end; y++) {
        int x0 = first_x(&s, y);
        if (x0 < 0 || is_rule(&s, y)) { after_spinner = false; continue; }
        if (x0 > RIGHT_MIN_X && row_width(&s, y) >= width - WRAP_SLACK)
            continue;   /* right-aligned status, nothing at its left: the effort, a usage notice, the update check */
        if (is_spinner(&s, y)) { after_spinner = true; continue; }
        if (after_spinner && is_tip(&s, y)) continue;
        after_spinner = false;
        bool join = prev == y - 1 && x0 >= 1 && !starts_item(&s, y) &&
                    row_width(&s, prev) + 1 + first_word_width(&s, y) > width - WRAP_SLACK;
        int ox = option_x(&s, y);
        if (dialog && y != hint && !join && ox < 0 && opt >= 0 && x0 > opt)
            continue;   /* an option's description: the question and options fit the pane */
        if (!join) opt = ox;
        bool space = !join || (!wide_at(&s, prev, row_width(&s, prev) - 1) && !wide_at(&s, y, x0));   /* CJK: none */
        size_t n = strlen(row_text(&s, y));
        const char *t = s.buf;
        if (join) {
            while (*t == ' ') t++;
            n -= (size_t)(t - s.buf);
        }
        if (o + n + 2 >= all_cap) break;
        if (join) { if (space) all[o++] = ' '; }
        else if (kept++) all[o++] = '\n';
        memcpy(all + o, t, n);
        o += n;
        prev = y;
    }
    all[o] = 0;
    /* the last max_lines lines, cut to cap from a line start */
    const char *p = all + o;
    int lines = 0;
    while (p > all && lines < max_lines) {
        p--;
        if (*p == '\n' && ++lines == max_lines) { p++; break; }
    }
    if (lines < max_lines) lines = kept;   /* all of them fit max_lines */
    else lines = max_lines;
    size_t rest = o - (size_t)(p - all);
    while (rest + 1 > cap) {
        const char *nl = memchr(p, '\n', rest);
        if (!nl) {   /* one line longer than cap: its end, from a character start */
            p += rest - (cap - 1);
            while (((unsigned char)*p & 0xc0) == 0x80) p++;
            rest = o - (size_t)(p - all);
            break;
        }
        rest -= (size_t)(nl + 1 - p);
        p = nl + 1;
        lines--;
    }
    memcpy(out, p, rest);
    out[rest] = 0;
    kept = rest ? lines : CC_LV_NONE;
done:
    vt_destroy(s.vt);
    heap_caps_free(s.buf);
    heap_caps_free(all);
    return kept;
}
