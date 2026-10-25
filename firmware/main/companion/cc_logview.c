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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cc_logview_size(const char *raw, size_t len, int *cols, int *rows, size_t *start, bool *cleared)
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
    if (cols) *cols = clampi(c + COLS_MARGIN, COLS_MIN, COLS_MAX);
    if (rows) *rows = max_row ? clampi(max_row, ROWS_MIN, ROWS_MAX) : ROWS_DEFAULT;
    if (start) *start = from;
    if (cleared) *cleared = clr;
}

/* ------------------------------------------------------------ row classes */
typedef struct {
    vt_t *vt;
    int cols, rows;
} scr_t;

static const vt_cell_t *row_at(const scr_t *s, int y) { return vt_view_row(s->vt, y); }
static bool blank_cp(uint32_t cp) { return cp == ' ' || cp == 0 || cp == 0xa0; }

static int first_x(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    for (int x = 0; x < s->cols; x++)
        if (!blank_cp(r[x].cp) && !(r[x].attr & VT_ATTR_WIDE_CONT)) return x;
    return -1;
}

/* cells up to the last drawn one */
static int row_width(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    for (int x = s->cols - 1; x >= 0; x--)
        if (!blank_cp(r[x].cp)) return x + 1;
    return 0;
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

static bool is_spinner(const scr_t *s, int y)
{
    const vt_cell_t *r = row_at(s, y);
    uint32_t c = r[0].cp, l = s->cols > 2 ? r[2].cp : 0;
    bool glyph = c == 0xb7 || c == 0x2722 || c == 0x2733 || c == 0x2736 || c == 0x273b || c == 0x273d || c == '*';
    return glyph && r[1].cp == ' ' && ((l >= 'A' && l <= 'Z') || (l >= 'a' && l <= 'z'));
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
    while (x < s->cols && !blank_cp(r[x].cp)) x++;
    return x - x0;
}

/* A numbered option ("1. Yes", "❯ 2. No"): the column of its number, else -1. */
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

/* The input box's top rule: a rule, the "❯" input line under it, and another rule a few rows further down. */
static int input_box_top(const scr_t *s)
{
    for (int y = s->rows - 2; y >= 0; y--) {
        if (!is_rule(s, y) || !is_prompt(s, y + 1)) continue;
        for (int k = y + 2; k < s->rows && k <= y + 16; k++)
            if (is_rule(s, k)) return y;
    }
    return -1;
}

/* ------------------------------------------------------------ the tail */
static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p;
}

int cc_logview_tail(const char *raw, size_t len, int max_lines, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!raw || !len || max_lines <= 0) return 0;
    int cols, rows;
    size_t start;
    bool cleared;
    cc_logview_size(raw, len, &cols, &rows, &start, &cleared);
    scr_t s = { vt_create(cols, rows, 0), cols, rows };
    size_t row_cap = (size_t)cols * 4 + 2, all_cap = (size_t)rows * (row_cap + 1) + 1;
    char *rowbuf = ps_alloc(row_cap), *all = ps_alloc(all_cap);
    int kept = 0;
    if (!s.vt || !rowbuf || !all) goto done;
    vt_feed(s.vt, (const uint8_t *)raw + start, len - start);

    int top = 0, end = input_box_top(&s);
    bool dialog = end < 0;
    if (dialog) {
        if (!cleared) goto done;   /* a partial render with no input box: maybe only chrome */
        end = rows;
        while (end > 0 && (first_x(&s, end - 1) < 0 || is_rule(&s, end - 1))) end--;
    }
    while (top < end && top < 4 && first_x(&s, top) < 0) top++;
    if (top < end && is_banner(&s, top)) {
        while (top < end && is_banner(&s, top)) top++;
    }
    /* the screen's width, for telling a wrapped row: the widest frame rule, else the virtual width */
    int width = 0;
    for (int y = 0; y < rows; y++)
        if (is_rule(&s, y)) { int w = row_width(&s, y); if (w > width) width = w; }
    if (!width) width = cols;

    size_t o = 0;
    int prev = -2;            /* the last kept row */
    bool after_spinner = false;
    int opt = -1;             /* a dialog's last option: the column of its number */
    for (int y = top; y < end; y++) {
        int x0 = first_x(&s, y);
        if (x0 < 0 || is_rule(&s, y)) { after_spinner = false; continue; }
        if (x0 > RIGHT_MIN_X && (x0 > width / 2 || row_width(&s, y) >= width - WRAP_SLACK))
            continue;   /* right-aligned status: the effort, a usage notice, the update check */
        if (is_spinner(&s, y)) { after_spinner = true; continue; }
        if (after_spinner && is_tip(&s, y)) continue;
        after_spinner = false;
        size_t n = vt_row_text(s.vt, y, rowbuf, row_cap);
        bool join = prev == y - 1 && x0 >= 1 && !is_tip(&s, y) &&
                    row_width(&s, prev) + 1 + first_word_width(&s, y) > width - WRAP_SLACK;
        int ox = option_x(&s, y);
        if (dialog && !join && ox < 0 && opt >= 0 && x0 > opt) continue;   /* an option's description: the options fit */
        if (!join) opt = ox;
        const char *t = rowbuf;
        if (join) {
            while (*t == ' ') t++;
            n -= (size_t)(t - rowbuf);
        }
        if (o + n + 2 >= all_cap) break;
        if (join) all[o++] = ' ';
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
    kept = rest ? lines : 0;
done:
    vt_destroy(s.vt);
    heap_caps_free(rowbuf);
    heap_caps_free(all);
    return kept;
}
