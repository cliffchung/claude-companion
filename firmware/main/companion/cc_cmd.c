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

bool cc_screen_is_permission_prompt(const char *plain)
{
    if (!plain) return false;
    return has_ci(plain, "do you want to", false) || has_ci(plain, "1. yes", false) ||
           has_ci(plain, "don't ask again", false) || has_ci(plain, "allow", true);
}
