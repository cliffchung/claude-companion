/* The Sessions output pane's text, read off a rendered screen of `claude logs` (see cc_logview.h). */
#include "cc_logview.h"
#include "companion/cc_cmd.h"
#include "term/vt.h"
#include <limits.h>
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
#define SPIN_NEAR    40    /* on a sparse screen, the spinner lies this close above the cursor or the last drawn row
                              (a todo list or an agent tree may sit between it and the input box) */
#define JOIN_MIN     80    /* on a sparse screen, rows narrower than this were never wrapped by an 80+ column TUI */

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

/* ------------------------------------------------------------ chrome lines (busy-pane fix rounds 1, 2) */
/* Every rule matches the whole line, or a shape anchored at its start, as Claude Code draws it. Anything unclear is
   kept: a stray chrome line is better than a lost sentence. A line is (l, n): no copy, no NUL needed at l[n], never
   read past n. Nothing here recurses; the frames are small (C1, round 2). */
#define ELL "\xe2\x80\xa6"   /* … */
#define MID "\xc2\xb7"       /* · */

static bool eq(const char *l, size_t n, const char *lit) { size_t k = strlen(lit); return n == k && !memcmp(l, lit, k); }
static bool pre(const char *l, size_t n, const char *lit) { size_t k = strlen(lit); return n >= k && !memcmp(l, lit, k); }
static bool post(const char *l, size_t n, const char *lit) { size_t k = strlen(lit); return n >= k && !memcmp(l + n - k, lit, k); }
static bool has(const char *l, size_t n, const char *lit)
{
    size_t k = strlen(lit);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(l + i, lit, k)) return true;
    return false;
}
static bool has_c(const char *l, size_t n, char c) { return memchr(l, c, n) != NULL; }

/* a spinner glyph (· ✢ ✳ ✶ ✻ ✽ or *): its length, else 0 */
static size_t spin_glyph(const char *l, size_t n)
{
    const unsigned char *u = (const unsigned char *)l;
    if (n >= 2 && u[0] == 0xc2 && u[1] == 0xb7) return 2;
    if (n >= 3 && u[0] == 0xe2 && u[1] == 0x9c && (u[2] == 0xa2 || u[2] == 0xb3 || u[2] == 0xb6 || u[2] == 0xbb || u[2] == 0xbd))
        return 3;
    return n && u[0] == '*' ? 1 : 0;
}

/* a spinner verb: a capital, then letters ("Beboppin'", "Sautéed"): its length, else 0 */
static size_t verb_len(const char *l, size_t n)
{
    const unsigned char *t = (const unsigned char *)l;
    if (!n || !(t[0] >= 'A' && t[0] <= 'Z')) return 0;
    size_t i = 1;
    for (;;) {
        if (i < n && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '\'')) i++;
        else if (i + 1 < n && t[i] >= 0xc3 && t[i] <= 0xc5 && (t[i + 1] & 0xc0) == 0x80) i += 2;
        else break;
    }
    return i;
}

/* a duration ("5s", "2m 6s", "1h 3m"): its length, else 0 */
static size_t duration_len(const char *l, size_t n)
{
    size_t i = 0, ok = 0;
    for (;;) {
        size_t d = i;
        while (i < n && l[i] >= '0' && l[i] <= '9') i++;
        if (i == d || i >= n || (l[i] != 's' && l[i] != 'm' && l[i] != 'h')) return ok;
        ok = ++i;
        if (i + 1 >= n || l[i] != ' ' || !(l[i + 1] >= '0' && l[i + 1] <= '9')) return ok;
        i++;
    }
}

/* one word of a spinner's counter: "12s", "3.2k", "·", "↓", "tokens", "thinking", "medium", "effort", ... */
static bool counter_word(const char *w, size_t n, bool *marker, bool *amount)
{
    static const char *const words[] = { "tokens", "thinking", "thought", "for", "with", "low", "medium", "high", "xhigh",
                                         "max", "effort", "lines", "words", "esc", "to", "interrupt" };
    if (n && w[0] == '(') { w++; n--; }
    if (n && w[n - 1] == ')') n--;
    if (!n) return true;
    if (eq(w, n, MID) || eq(w, n, "\xe2\x86\x93") || eq(w, n, "\xe2\x86\x91")) { *marker = true; return true; }
    size_t d = 0;
    while (d < n && ((w[d] >= '0' && w[d] <= '9') || w[d] == '.' || w[d] == ',')) d++;
    if (d && (d == n || (d + 1 == n && strchr("kMsmh", w[d])))) { *amount = true; return true; }
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (eq(w, n, words[i])) {
            if (i <= 2) *amount = true;   /* tokens, thinking, thought */
            return true;
        }
    return false;
}

#define CNT_GLYPH 1   /* after a spinner glyph: any counter words will do */
#define CNT_BARE  2   /* only bare integers ("Honking… 6 8") */

/* A spinner's counter, whole or a stale piece of it: every word a counter word (or a lone letter a redraw left),
   with an amount, and the counter's frame: "(2s · thinking with medium effort)" (parenthesised, with a "·" or an
   arrow), a stale tail whose "(" was never redrawn ("10s · 11 lines)", "5 thinking)"), or "… with medium effort".
   "0.8s · 1,024 tokens" and "(12 lines)" are not. */
static bool counter_line(const char *l, size_t n, int how)
{
    bool marker = false, amount = false, with = false, effort = false, bare = true;
    size_t i = 0;
    if (!n) return false;
    while (i < n) {
        while (i < n && l[i] == ' ') i++;
        size_t w = i;
        while (i < n && l[i] != ' ') i++;
        size_t k = i - w;
        if (!k) break;
        if (k == 1 && ((l[w] | 32) >= 'a' && (l[w] | 32) <= 'z')) { bare = false; continue; }
        if (!counter_word(l + w, k, &marker, &amount)) return false;
        for (size_t j = w; j < i; j++) bare &= l[j] >= '0' && l[j] <= '9';
        with |= eq(l + w, k, "with");
        effort |= pre(l + w, k, "effort");
    }
    if (!amount) return false;
    if (how == CNT_GLYPH) return true;
    if (how == CNT_BARE) return bare;
    bool open = l[0] == '(', close = l[n - 1] == ')', inner = has_c(l + 1, n - 1, '(');
    if (open && close && !inner) return marker;                /* "(12s · ↓ 3k tokens)", not "(12 lines)" */
    if (!open && close && !inner && !has_c(l, n, '(')) return true;   /* a stale tail */
    return with && effort;
}

/* the hints the TUI puts on a row of their own */
static bool hint_line(const char *l, size_t n)
{
    static const char *const hints[] = { "(ctrl+b to run in background)", "(ctrl+o to expand)", "(ctrl+r to expand)",
                                         "(esc to interrupt)", "(shift+tab to cycle)" };
    for (size_t h = 0; h < sizeof(hints) / sizeof(hints[0]); h++) {
        const char *b = hints[h];
        size_t i = 0;
        while (i < n && *b) {   /* a redraw's skipped cell may double a space */
            if (l[i] == ' ' && *b == ' ') { while (i < n && l[i] == ' ') i++; b++; continue; }
            if (l[i] != *b) break;
            i++, b++;
        }
        if (i == n && !*b) return true;
    }
    return false;
}

static const char *const s_events[] = { "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
                                        "Stop", "SubagentStop", "PreCompact", "Notification" };

/* a hook event's name at l: its length, else 0 */
static size_t event_len(const char *l, size_t n)
{
    for (size_t i = 0; i < sizeof(s_events) / sizeof(s_events[0]); i++) {
        size_t k = strlen(s_events[i]);
        if (n >= k && !memcmp(l, s_events[i], k) && (k == n || l[k] == ' ' || l[k] == ':')) return k;
    }
    return 0;
}

/* The rules for a line with no spinner glyph in front (l trimmed). */
static bool chrome_plain(const char *l, size_t n)
{
    const unsigned char *u = (const unsigned char *)l;
    /* a bare verb with its counter: "Swirling… (12s · ↓ 3k tokens)", "Honking… 39 thinking with medium effort",
       "Beboppin'… 1"; not "Waiting… 10s" */
    size_t v = verb_len(l, n);
    if (v && pre(l + v, n - v, ELL " ")) {
        const char *r = l + v + 4;
        size_t rn = n - v - 4;
        if (counter_line(r, rn, 0) || counter_line(r, rn, CNT_BARE)) return true;
    }
    if (counter_line(l, n, 0) || hint_line(l, n)) return true;
    /* a status line's stale tail, its "(" never redrawn: "running Stop h ok · 17s · ↓ 1.1k tokens)", or a broken
       counter word before its time: "ought for 1s)" */
    if (l[n - 1] == ')' && !has_c(l, n, '(')) {
        for (size_t i = 0; i + 4 <= n; i++)
            if (!memcmp(l + i, " " MID " ", 4)) {
                if (counter_line(l + i + 1, n - i - 1, 0)) return true;
                break;
            }
        size_t w = 0;
        while (w < n && l[w] >= 'a' && l[w] <= 'z') w++;
        if (w && pre(l + w, n - w, " for ")) {
            size_t d = duration_len(l + w + 5, n - w - 5);
            if (d && w + 5 + d + 1 == n) return true;
        }
    }
    /* the footer's "N agents" or "N shells", whole */
    {
        size_t d = 0;
        while (d < n && l[d] >= '0' && l[d] <= '9') d++;
        const char *r = l + d;
        size_t rn = n - d;
        if (d && (eq(r, rn, " agent") || eq(r, rn, " agents") || eq(r, rn, " shell") || eq(r, rn, " shells"))) return true;
    }
    /* the footer's meter: "12k tokens | Opus 5.5", "46k tokens | [██░░] 7% reset | …" */
    {
        size_t d = 0;
        while (d < n && ((l[d] >= '0' && l[d] <= '9') || l[d] == '.')) d++;
        if (d && d < n && (l[d] == 'k' || l[d] == 'M')) d++;
        if (d && pre(l + d, n - d, " tokens |")) return true;
    }
    /* the mode line: ⏵⏵ or ⏸ and its text, or the text alone */
    if (n >= 3 && u[0] == 0xe2 && u[1] == 0x8f && (u[2] == 0xb5 || u[2] == 0xb8)) return true;
    static const char *const modes[] = { "auto mode on", "plan mode on", "manual mode on", "accept edits on",
                                         "bypass permissions on" };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        if (pre(l, n, modes[i])) {
            size_t k = strlen(modes[i]);
            if (k == n || pre(l + k, n - k, " (shift+tab") || pre(l + k, n - k, " " MID " ")) return true;
        }
    if (pre(l, n, "? for shortcuts")) return true;
    if (eq(l, n, "Checking for updates") || eq(l, n, "Checking for updates" ELL)) return true;
    if (eq(l, n, "/rc") || eq(l, n, "/rc connecting" ELL)) return true;
    /* hooks, in the TUI's shapes: "(running SessionStart hooks… 0/2 · 0s)", "running Stop hook · 5s · …",
       "UserPromptSubmit hook · 0s)", "SessionStart:startup hook succeeded"; never a sentence about a hook */
    {
        size_t i = l[0] == '(';
        if (pre(l + i, n - i, "running ") || pre(l + i, n - i, "Running ")) i += 8;
        size_t e = event_len(l + i, n - i);
        if (e) {
            i += e;
            if (i < n && l[i] == ':') while (i < n && l[i] != ' ') i++;
            if (pre(l + i, n - i, " hooks")) i += 6;
            else if (pre(l + i, n - i, " hook")) i += 5;
            else i = 0;
            if (i) {
                const char *r = l + i;
                size_t rn = n - i;
                if (!rn || pre(r, rn, ELL) || pre(r, rn, " " MID " ") || eq(r, rn, ")") || eq(r, rn, " succeeded") ||
                    eq(r, rn, " failed") || pre(r, rn, " succeeded ") || pre(r, rn, " failed "))
                    return true;
            }
        }
    }
    /* a tip under the spinner: "⎿  Tip: …" */
    if (n >= 3 && u[0] == 0xe2 && u[1] == 0x8e && u[2] == 0xbf) {
        size_t i = 3;
        while (i < n && l[i] == ' ') i++;
        if (pre(l + i, n - i, "Tip:")) return true;
    }
    /* the effort: "◐ medium · /effort"; usage notices: "You've used 89% of your weekly limit · resets Oct 27 at 7pm" */
    if (n >= 3 && u[0] == 0xe2 && u[1] == 0x97 && u[2] >= 0x90 && u[2] <= 0x93 && has(l, n, "/effort")) return true;
    if ((pre(l, n, "You've used ") || pre(l, n, "Approaching ")) && has(l, n, " limit " MID " resets ")) return true;
    /* the banner: two logo glyphs (block elements), then the version, the model or the folder line */
    if (n >= 6 && u[0] == 0xe2 && u[1] == 0x96 && u[2] >= 0x80 && u[2] <= 0x9f && u[3] == 0xe2 && u[4] == 0x96 &&
        u[5] >= 0x80 && u[5] <= 0x9f && (has(l, n, "Claude Code v") || has(l, n, " " MID " "))) return true;
    /* the frame rule with the session's name: a long run of "─", the name, " ─" ("─── Summary ───" stays) */
    if (n >= 30 && post(l, n, " \xe2\x94\x80")) {
        size_t r = 0;
        while (r + 3 <= n && !memcmp(l + r, "\xe2\x94\x80", 3)) r += 3;
        if (r >= 30 && r + 4 < n) return true;
    }
    return false;
}

bool cc_logview_chrome_line(const char *line)
{
    if (!line) return false;
    const char *l = line;
    while (*l == ' ') l++;
    size_t n = strlen(l);
    while (n && l[n - 1] == ' ') n--;
    if (!n) return false;
    /* the spinner's glyph, as often as a row repeats it ("· · · the end" is no spinner: the rules see "the end") */
    size_t g = spin_glyph(l, n);
    bool star = g == 1;
    if (!g || g >= n || l[g] != ' ') return chrome_plain(l, n);
    const char *r = l;
    size_t rn = n;
    while ((g = spin_glyph(r, rn)) && g < rn && r[g] == ' ') {
        r += g + 1;
        rn -= g + 1;
        while (rn && *r == ' ') r++, rn--;
    }
    if (!rn) return false;
    /* a verb and "…" ("✽ Swirling… (12s · ↓ 3k tokens)"; "*" only with nothing or a counter after it), or, never
       after "*", "<verb> for <time>" ("✻ Baked for 5s · 1 shell still running"), a counter ("✢ 5 thinking", "✳ n (2s
       · thinking with medium effort)"), a bare number left by a redraw ("✻ 313"), or a plain rule's shape */
    size_t v = verb_len(r, rn);
    if (v && pre(r + v, rn - v, ELL)) {
        size_t t = v + 3;
        while (t < rn && r[t] == ' ') t++;
        if (!star || t == rn || counter_line(r + t, rn - t, 0)) return true;
    }
    if (star) return false;
    if (v && pre(r + v, rn - v, " for ")) {
        size_t d = duration_len(r + v + 5, rn - v - 5);
        if (d && (v + 5 + d == rn || pre(r + v + 5 + d, rn - v - 5 - d, " " MID " "))) return true;
    }
    if (counter_line(r, rn, CNT_GLYPH)) return true;
    bool digits = true;
    for (size_t i = 0; i < rn && digits; i++) digits = (r[i] >= '0' && r[i] <= '9') || r[i] == ' ';
    return digits || chrome_plain(r, rn);
}

/* A stale fragment of a redraw: no word (two letters in a row, or a CJK character) and a "…" or "·" ("3  12s…",
   "· 4"), or a bare ⏺ or ⎿ far from the digit after it (a dot blinking over an old counter). "⏺ 42", "↑ 1.2%" and
   "});" stay. */
static bool fragment_line(const char *l)
{
    const unsigned char *u = (const unsigned char *)l;
    for (size_t i = 0; u[i]; i++) {
        if (((u[i] | 32) >= 'a' && (u[i] | 32) <= 'z') && ((u[i + 1] | 32) >= 'a' && (u[i + 1] | 32) <= 'z')) return false;
        if (u[i] >= 0xe3) return false;   /* CJK and the rest past U+3000 */
    }
    if (!strncmp(l, "\xe2\x8f\xba", 3) || !strncmp(l, "\xe2\x8e\xbf", 3)) {
        size_t sp = 0;
        while (l[3 + sp] == ' ') sp++;
        return sp > 2 || !l[3 + sp];
    }
    return strstr(l, ELL) || strstr(l, MID);
}

/* ------------------------------------------------------------ the pane's fallback */
#define DROP_MARK 0x01   /* a dropped line's first byte while sorting (cc_log_tail leaves no control bytes) */
#define DROP_VERB 0x02   /* the same, for a spinner verb (pass 2): not a neighbour's evidence */
#define VERBS     24     /* the spinner verbs remembered (distinct, newest kept) */

typedef struct {
    uint32_t at;          /* the verb in s */
    uint16_t n;
    uint8_t seen, glyph;  /* how often bare; seen after a glyph */
    char c0;              /* its first byte: a bare verb's line may be marked dropped since */
} verb_t;

static char verb_at(const verb_t *v, const char *s, size_t i) { return i ? s[v->at + i] : v->c0; }

/* the spinner's verb in a line: "✽ Swirling… …" (glyph) or "Swirling…" (bare, nothing after): its offset, else -1 */
static int line_verb(const char *l, size_t n, size_t *vn, bool *glyph)
{
    size_t g = spin_glyph(l, n), at = 0;
    *glyph = g > 1 && g < n && l[g] == ' ';
    if (*glyph) at = g + 1;
    size_t v = verb_len(l + at, n - at);
    if (v < 3 || !pre(l + at + v, n - at - v, ELL)) return -1;
    if (!*glyph && at + v + 3 != n) return -1;
    *vn = v;
    return (int)at;
}

static int verb_find(const verb_t *vs, int nv, const char *s, const char *w, size_t n)
{
    for (int k = 0; k < nv; k++)
        if (vs[k].n == n && vs[k].c0 == w[0] && !memcmp(s + vs[k].at + 1, w + 1, n - 1)) return k;
    return -1;
}

/* A spinner verb broken by redraws: "Galliva ting…" (a skipped cell over a seen verb, or, unseen, a capitalised
   piece and a short gerund piece: "Keep going…" goes too, "Please wait…" and "Still thinking…" stay), or a short
   piece of a seen verb with no spaces ("ify", "Honk", 3-8 bytes). */
static bool broken_verb(const verb_t *vs, int nv, const char *s, const char *l, size_t n)
{
    bool ell = post(l, n, ELL), space = has_c(l, n, ' ');
    size_t body = ell ? n - 3 : n;
    if (ell && space) {
        size_t a = verb_len(l, body), b = a + 1, c = b;
        while (c < body && l[c] >= 'a' && l[c] <= 'z') c++;
        size_t tail = c - b;
        if (a >= 3 && l[a] == ' ' && c == body && tail >= 2 && tail <= 6 && a + tail >= 8 &&
            (post(l + b, tail, "ing") || post(l + b, tail, "ed")))
            return true;
    }
    for (int k = 0; k < nv; k++) {
        if (ell && space && vs[k].n == body) {   /* the same letters, but a space where a cell was skipped */
            size_t i = 0;
            while (i < body && (l[i] == verb_at(&vs[k], s, i) || l[i] == ' ')) i++;
            if (i == body) return true;
        }
        if (!ell && !space && n >= 3 && n <= 8 && n < vs[k].n)
            for (size_t i = 0; i + n <= vs[k].n; i++) {
                size_t j = 0;
                while (j < n && l[j] == verb_at(&vs[k], s, i + j)) j++;
                if (j == n) return true;
            }
    }
    return false;
}

int cc_logview_fallback(char *s, int max_lines)
{
    if (!s) return 0;
    cc_log_tail(s, INT_MAX);
    verb_t vs[VERBS];
    int nv = 0, next = 0;
    /* pass 1: mark the chrome and the fragments; remember the spinner's verbs */
    for (size_t r = 0; s[r];) {
        size_t a = r;
        while (s[r] && s[r] != '\n') r++;
        size_t e = r;
        if (s[r]) r++;
        size_t vn;
        bool glyph;
        int at = line_verb(s + a, e - a, &vn, &glyph);
        if (at >= 0) {
            int k = verb_find(vs, nv, s, s + a + at, vn);
            if (k < 0) {
                k = nv < VERBS ? nv++ : next;
                next = (k + 1) % VERBS;
                vs[k] = (verb_t){ (uint32_t)(a + (size_t)at), (uint16_t)vn, 0, 0, s[a + (size_t)at] };
            }
            if (glyph) vs[k].glyph = 1;
            else if (vs[k].seen < 255) vs[k].seen++;
        }
        char save = s[e];
        s[e] = 0;
        bool drop = cc_logview_chrome_line(s + a) || fragment_line(s + a);
        s[e] = save;
        if (drop) s[a] = DROP_MARK;
    }
    /* pass 2: a bare verb the spinner drew (seen twice, or also after its glyph, or once between chrome lines), and
       broken pieces of its verbs. Only a line's first byte is ever marked; each verb keeps its own (c0). */
    bool prev_chrome = false;
    for (size_t r = 0; s[r];) {
        size_t a = r;
        while (s[r] && s[r] != '\n') r++;
        size_t e = r;
        if (s[r]) r++;
        unsigned char m = (unsigned char)s[a];
        if (m == DROP_MARK || e == a) { prev_chrome = m == DROP_MARK; continue; }
        size_t vn;
        bool glyph;
        int at = line_verb(s + a, e - a, &vn, &glyph);
        int k = at >= 0 ? verb_find(vs, nv, s, s + a + at, vn) : -1;
        bool next_chrome = (unsigned char)s[r] == DROP_MARK;
        if ((k >= 0 && (vs[k].seen >= 2 || vs[k].glyph || prev_chrome || next_chrome)) || broken_verb(vs, nv, s, s + a, e - a))
            s[a] = DROP_VERB;
        prev_chrome = false;
    }
    /* pass 3: keep the rest, in order */
    size_t w = 0;
    int kept = 0;
    for (size_t r = 0; s[r];) {
        size_t a = r;
        while (s[r] && s[r] != '\n') r++;
        size_t e = r;
        if (s[r]) r++;
        if ((unsigned char)s[a] == DROP_MARK || (unsigned char)s[a] == DROP_VERB || e == a) continue;
        if (kept++) s[w++] = '\n';
        memmove(s + w, s + a, e - a);
        w += e - a;
    }
    s[w] = 0;
    if (max_lines < 0) max_lines = 0;
    if (kept > max_lines) {
        int skip = kept - max_lines;
        size_t p = 0;
        while (skip && p < w) { if (s[p++] == '\n') skip--; }
        memmove(s, s + p, w - p + 1);
        kept = max_lines;
    }
    return kept;
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
   A reply's own "* Item" or "· note" doesn't match. loose: anything after the "…" (a sparse screen, whose "(" was
   drawn before the stream's front and never again). */
static bool is_spinner(const scr_t *s, int y, bool loose)
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
        if (!*t || loose) return true;
        if (*t != '(') return false;
        const unsigned char *close = (const unsigned char *)strrchr((const char *)t, ')');
        if (!close) return false;
        for (t = close + 1; *t == ' '; t++) {}
        return !*t;
    }
    if (c == '*' || strncmp((const char *)t, " for ", 5)) return false;   /* not "* Ran for 12m" */
    size_t n = strlen((const char *)t + 5), d = duration_len((const char *)t + 5, n);   /* not "· Pay for 3 items" */
    return d && (d == n || !strncmp((const char *)t + 5 + d, " \xc2\xb7 ", 4));
}

/* A whole row that is only one of the TUI's key hints: "(ctrl+b to run in background)" under a running command. */
static bool is_hint(const scr_t *s, int y)
{
    const char *t = row_text(s, y);
    while (*t == ' ') t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') n--;
    return n && hint_line(t, n);
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

/* A busy session's sparse screen (no full clear in the stream: `claude logs` keeps only its recent output, and a
   turn's spinner redraws fill that): no rules, no input box, only the rows redrawn since. The spinner is the
   anchor: the lowest spinner line within SPIN_NEAR rows above the parked cursor (the input line) or above the last
   drawn row, whichever is lower (a stream may end mid-frame with the cursor above the spinner). Returns its row, else
   -1. Under it lie only a todo list or an agent tree, the tip, the effort, the box and the footer. */
static int sparse_spinner(const scr_t *s)
{
    int cx, cy, best = -1;
    bool visible;
    vt_cursor(s->vt, &cx, &cy, &visible);
    int refs[2] = { visible && cy > 0 && cy < s->rows ? cy : -1, last_drawn(s) };
    for (int i = 0; i < 2; i++)
        for (int y = refs[i]; y > best && y >= refs[i] - SPIN_NEAR; y--)
            if (is_spinner(s, y, true)) { best = y; break; }
    return best;
}

/* A row's text, from its text column, looks like code or a listing, not prose: a column gap of two or more spaces
   ("drwxr-xr-x  5 …"), a call ("add_argument("), or a path, flag, prompt or comment at its start. */
static bool code_like(const scr_t *s, int y, int x0)
{
    const vt_cell_t *r = row_at(s, y);
    int w = row_width(s, y);
    uint32_t c = r[x0].cp;
    if (c == '/' || c == '-' || c == '~' || c == '.' || c == '$' || c == '#' || c == '`' || c == '>') return true;
    for (int x = x0 + 1; x < w; x++) {
        uint32_t a = r[x - 1].cp, b = r[x].cp;
        if (b == ' ' && a == ' ') return true;
        if (b == '(' && (((a | 32) >= 'a' && (a | 32) <= 'z') || a == '_' || (a >= '0' && a <= '9'))) return true;
    }
    return false;
}

/* Sparse screens only: the row y continues the wrapped row above it. The width is a guess (no rule drawn), so only
   prose: the row above ran to within WRAP_SLACK of the widest row (JOIN_MIN or more) and doesn't end a sentence or a
   code line (. ! ? : ; brackets, quotes, a box character), neither row looks like code or a listing (code_like, " = "),
   and this row starts lowercase or with a CJK character at that row's text column. */
static bool sparse_join(const scr_t *s, int prev, int y, int width)
{
    if (width < JOIN_MIN || row_width(s, prev) + 1 + first_word_width(s, y) <= width - WRAP_SLACK) return false;
    const vt_cell_t *p = row_at(s, prev), *r = row_at(s, y);
    int px = first_x(s, prev), x0 = first_x(s, y);
    uint32_t pc = p[px].cp;
    int text_x = pc == 0x23bf ? px + 3 : (pc == 0x23fa || pc == 0x276f) ? px + 2 : px;   /* "⎿  ", "⏺ ", "❯ " */
    if (x0 != text_x || starts_item(s, y)) return false;
    uint32_t c = r[x0].cp, last = p[row_width(s, prev) - 1].cp;
    if (!((c >= 'a' && c <= 'z') || c >= 0x2e80)) return false;
    if ((last < 0x80 && last && strchr(".!?:;)]}{([|\"'`", (int)last)) || box_cp(last)) return false;
    if (code_like(s, prev, text_x) || code_like(s, y, x0)) return false;
    return !strstr(row_text(s, prev), " = ") && !strstr(row_text(s, y), " = ");
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
       its frame is no input box), else the input box, else a rule near the bottom, else a sparse screen's spinner */
    int top = 0, hint = dialog_hint(&s), end;
    bool dialog = hint >= 0, sparse = false;
    if (dialog) end = hint + 1;
    else if ((end = input_box_top(&s)) < 0 && (end = rule_near_bottom(&s)) < 0 && !cleared) {
        /* a busy session's sparse screen: up to its spinner, which goes too */
        end = sparse_spinner(&s);
        if (end >= 0) { end++; sparse = true; }
    }
    kept = CC_LV_NO_LAYOUT;
    if (end < 0) goto done;
    while (top < end && top < 4 && first_x(&s, top) < 0) top++;
    while (top < end && is_banner(&s, top)) top++;
    /* the screen's width, for telling a wrapped row: the widest frame rule, else the virtual width */
    int width = 0;
    for (int y = 0; y < rows; y++)
        if (is_rule(&s, y)) { int w = row_width(&s, y); if (w > width) width = w; }
    if (!width && sparse)   /* no rule drawn: the widest row is the closest to the width */
        for (int y = top; y < end; y++) { int w = row_width(&s, y) + 1; if (w > width) width = w; }
    if (!width) width = cols;

    size_t o = 0;
    int prev = -2;            /* the last kept row */
    bool after_spinner = false;
    int opt = -1;             /* a dialog's last option: the column of its number */
    kept = 0;
    for (int y = top; y < end; y++) {
        int x0 = first_x(&s, y);
        if (x0 < 0 || is_rule(&s, y)) { after_spinner = false; continue; }
        if (!sparse && x0 > RIGHT_MIN_X && row_width(&s, y) >= width - WRAP_SLACK)
            continue;   /* right-aligned status, nothing at its left: the effort, a usage notice, the update check */
        if (is_spinner(&s, y, sparse)) { after_spinner = true; continue; }
        if (after_spinner && is_tip(&s, y)) continue;
        after_spinner = false;
        if (is_hint(&s, y)) continue;
        if (sparse) {   /* chrome (the status, a counter) and stale fragments of a redraw, judged row by row */
            const char *t = row_text(&s, y);
            while (*t == ' ') t++;
            if (cc_logview_chrome_line(t) || fragment_line(t)) continue;
        }
        bool join = prev == y - 1 && x0 >= 1 && !starts_item(&s, y) &&
                    (sparse ? sparse_join(&s, prev, y, width)
                            : row_width(&s, prev) + 1 + first_word_width(&s, y) > width - WRAP_SLACK);
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
