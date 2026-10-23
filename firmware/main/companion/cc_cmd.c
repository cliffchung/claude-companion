#include "cc_cmd.h"

#include <stdio.h>
#include <string.h>

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool cc_path_valid(const char *p)
{
    if (!p) return false;
    size_t n = strlen(p);
    if (n < 1 || n > 63) return false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '/' || c == '~' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool cc_id_valid(const char *id)
{
    if (!id || strlen(id) != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (!is_hex(id[i])) return false;
    }
    return true;
}

bool cc_session_id_valid(const char *sid)
{
    if (!sid || strlen(sid) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (sid[i] != '-') return false;
        } else if (!is_hex(sid[i])) {
            return false;
        }
    }
    return true;
}

static bool quote_impl(const char *in, char *out, size_t cap)
{
    if (!in) return false;
    size_t o = 0;
    if (o + 1 >= cap) return false;
    out[o++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            if (o + 4 >= cap) return false;
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else {
            if (o + 1 >= cap) return false;
            out[o++] = *p;
        }
    }
    if (o + 2 > cap) return false;
    out[o++] = '\'';
    out[o] = 0;
    return true;
}

bool cc_shell_quote(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    if (quote_impl(in, out, cap)) return true;
    out[0] = 0;
    return false;
}

/* Formats into out; false if it doesn't fit. Returns the length through *len. */
static bool put(char *out, size_t cap, size_t *len, const char *fmt, const char *a, const char *b)
{
    int r = snprintf(out, cap, fmt, a, b);
    if (r < 0 || (size_t)r >= cap) return false;
    *len = (size_t)r;
    return true;
}

bool cc_cmd_agents(const char *claude, char *out, size_t cap)
{
    size_t n;
    if (!out || !cc_path_valid(claude)) return false;
    return put(out, cap, &n, "%s agents --json --all%s", claude, "");
}

bool cc_cmd_logs(const char *claude, const char *id, char *out, size_t cap)
{
    size_t n;
    if (!out || !cc_path_valid(claude) || !cc_id_valid(id)) return false;
    return put(out, cap, &n, "%s logs %s | tail -n 40", claude, id);
}

bool cc_cmd_attach(const char *claude, const char *id, char *out, size_t cap)
{
    size_t n;
    if (!out || !cc_path_valid(claude) || !cc_id_valid(id)) return false;
    return put(out, cap, &n, "%s attach %s", claude, id);
}

bool cc_cmd_reply(const char *claude, const char *sid, const char *text, char *out, size_t cap)
{
    size_t n;
    if (!out || !text || !text[0] || !cc_path_valid(claude) || !cc_session_id_valid(sid)) return false;
    if (!put(out, cap, &n, "%s -p --resume %s -- ", claude, sid)) return false;
    if (cc_shell_quote(text, out + n, cap - n)) return true;
    out[0] = 0;
    return false;
}

size_t cc_strip_ansi(char *s)
{
    size_t r = 0, w = 0;
    while (s[r]) {
        unsigned char c = (unsigned char)s[r];
        if (c != 0x1b) {
            s[w++] = s[r++];
            continue;
        }
        r++;
        if (s[r] == '[') { /* CSI: params, intermediates, final byte */
            r++;
            while (s[r] && ((unsigned char)s[r] < 0x40 || (unsigned char)s[r] > 0x7e)) r++;
            if (s[r]) r++;
        } else if (s[r] == ']') { /* OSC: until BEL or ESC \ */
            r++;
            while (s[r] && s[r] != 0x07 && !(s[r] == 0x1b && s[r + 1] == '\\')) r++;
            if (s[r] == 0x07) r++;
            else if (s[r] == 0x1b) r += 2;
        } else if (s[r]) { /* two-byte escape */
            r++;
        }
    }
    s[w] = 0;
    return w;
}

/* Box drawing (U+2500-257F) and block elements (U+2580-259F): E2 94..96 xx. */
static size_t rule_char(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] == 0xe2 && u[1] >= 0x94 && u[1] <= 0x96 && u[2] >= 0x80 && u[2] <= 0xbf) return 3;
    if (u[0] == 0xc2 && u[1] == 0xa0) return 2;   /* NBSP */
    if (u[0] == ' ') return 1;
    return 0;
}

static bool blank_line(const char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t k = rule_char(p + i);
        if (!k) return false;
        i += k;
    }
    return true;
}

int cc_log_tail(char *s, int max_lines)
{
    if (!s) return 0;
    /* 1. escapes: cursor-forward -> spaces (never more than the sequence's own bytes, so
       the text can't grow), cursor-to-column -> one space, cursor positioning -> new
       line, everything else dropped */
    size_t r = 0, w = 0;
    while (s[r]) {
        unsigned char c = (unsigned char)s[r];
        if (c == 0x1b) {
            size_t start = r++;
            if (s[r] == '[') {
                r++;
                int num = 0;
                bool has = false;
                while (s[r] && ((unsigned char)s[r] < 0x40 || (unsigned char)s[r] > 0x7e)) {
                    if (s[r] >= '0' && s[r] <= '9' && num < 1000) { num = num * 10 + (s[r] - '0'); has = true; }
                    r++;
                }
                char fin = s[r];
                if (fin) r++;
                if (fin == 'C') {
                    size_t sp = has ? (size_t)num : 1;
                    if (sp < 1) sp = 1;
                    if (sp > r - start) sp = r - start;
                    while (sp--) s[w++] = ' ';
                } else if (fin == 'G') {
                    s[w++] = ' ';   /* cursor to a column: the TUI's way of spacing words */
                } else if (fin == 'H' || fin == 'f' || fin == 'B' || fin == 'E' || fin == 'd') {
                    s[w++] = '\n';
                }
            } else if (s[r] == ']') {
                r++;
                while (s[r] && s[r] != 0x07 && !(s[r] == 0x1b && s[r + 1] == '\\')) r++;
                if (s[r] == 0x07) r++;
                else if (s[r] == 0x1b) r += 2;
            } else if (s[r]) {
                r++;
            }
            continue;
        }
        if (c == '\t') { s[w++] = ' '; r++; continue; }
        if ((c < 0x20 && c != '\r' && c != '\n') || c == 0x7f) { r++; continue; }
        s[w++] = s[r++];
    }
    s[w] = 0;
    /* 2. split on \r and \n, trim, drop blank and rule-only lines; compact to the front */
    size_t len = w, out = 0;
    int kept = 0;
    r = 0;
    while (r < len) {
        size_t a = r;
        while (r < len && s[r] != '\r' && s[r] != '\n') r++;
        size_t b = r;
        if (r < len) r++;
        size_t k;
        while (a < b && (k = rule_char(s + a)) != 0 && k != 3) a += k;   /* leading blanks */
        while (b > a && s[b - 1] == ' ') b--;
        while (b >= a + 2 && (unsigned char)s[b - 2] == 0xc2 && (unsigned char)s[b - 1] == 0xa0) b -= 2;
        if (b == a || blank_line(s + a, b - a)) continue;
        memmove(s + out, s + a, b - a);
        out += b - a;
        s[out++] = '\n';
        kept++;
    }
    if (out) out--;   /* no trailing newline */
    s[out] = 0;
    /* 3. keep the last max_lines */
    if (max_lines < 0) max_lines = 0;
    if (kept > max_lines) {
        int skip = kept - max_lines;
        size_t p = 0;
        while (skip && p < out) { if (s[p++] == '\n') skip--; }
        memmove(s, s + p, out - p + 1);
        kept = max_lines;
    }
    return kept;
}

size_t cc_paste_build(const char *text, char *out, size_t cap)
{
    static const char start[] = "\x1b[200~";
    static const char end[] = "\x1b[201~\r";
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!text) return 0;
    size_t o = sizeof(start) - 1, kept = 0;
    if (o >= cap) return 0;
    memcpy(out, start, o);
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\n') || c == 0x7f) continue;
        if (o + 1 >= cap) {
            out[0] = 0;
            return 0;
        }
        out[o++] = (char)c;
        kept++;
    }
    if (kept == 0 || o + sizeof(end) > cap) { /* sizeof(end) counts the NUL */
        out[0] = 0;
        return 0;
    }
    memcpy(out + o, end, sizeof(end));
    return o + sizeof(end) - 1;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_word(char c)
{
    c = lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* Case-insensitive search for needle (lowercase) at s; whole word if word is set. */
static bool has_ci(const char *s, const char *needle, bool word)
{
    size_t n = strlen(needle);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && lower(p[i]) == needle[i]) i++;
        if (i < n) continue;
        if (!word) return true;
        if ((p == s || !is_word(p[-1])) && !is_word(p[n])) return true;
    }
    return false;
}

#define SEL_GLYPH "\xe2\x9d\xaf"   /* the TUI's selection / input marker */
#define RULE_GLYPH "\xe2\x94\x80"  /* frame rule */

static const char *skip_blank(const char *p)
{
    for (;;) {
        if (*p == ' ') p++;
        else if ((unsigned char)p[0] == 0xc2 && (unsigned char)p[1] == 0xa0) p += 2;   /* NBSP */
        else return p;
    }
}

static bool is_option(const char *p)   /* "<digit>." */
{
    return p[0] >= '1' && p[0] <= '9' && p[1] == '.';
}

bool cc_screen_is_permission_prompt(const char *plain)
{
    if (!plain) return false;
    /* Only the bottom of the screen counts: the last selected option ("❯ 2.") must come
       after the last empty input line ("❯ " then a rule or line end). */
    const char *sel = NULL, *empty = NULL;
    for (const char *p = strstr(plain, SEL_GLYPH); p; p = strstr(p + 3, SEL_GLYPH)) {
        const char *q = skip_blank(p + 3);
        if (is_option(q)) sel = p;
        else if (!*q || *q == '\n' || *q == '\r' || strncmp(q, RULE_GLYPH, 3) == 0) empty = p;
    }
    if (!sel || (empty && empty > sel)) return false;
    /* Dialog structure: at least two numbered options around the selection... */
    const char *from = sel - plain > 400 ? sel - 400 : plain;
    int options = 0;
    for (const char *p = from; *p; p++) {
        /* the real stream drops spaces ("1.Yes2.No"), so any non-digit may precede */
        if (is_option(p) && (p == plain || p[-1] < '0' || p[-1] > '9')) options++;
    }
    /* ...and an esc / cancel hint after it (glued to words too: "Esctocancel"). */
    return options >= 2 && (has_ci(sel, "esc", false) || has_ci(sel, "cancel", false));
}

bool cc_screen_input_ready(const char *plain)
{
    if (!plain) return false;
    const char *last = NULL;
    for (const char *p = strstr(plain, SEL_GLYPH); p; p = strstr(p + 3, SEL_GLYPH)) last = p;
    if (!last) return false;
    const unsigned char *q = (const unsigned char *)last + 3;
    while (*q && *q != '\r' && *q != '\n') {
        if (*q == ' ' || *q == '\t') q++;
        else if (q[0] == 0xc2 && q[1] == 0xa0) q += 2;                          /* NBSP */
        else if (q[0] == 0xe2 && (q[1] == 0x94 || q[1] == 0x95) && q[2]) q += 3; /* U+2500-257F */
        else return false;
    }
    return true;
}
