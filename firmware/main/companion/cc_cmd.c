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

bool cc_shell_quote(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap == 0) return false;
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
    if (!put(out, cap, &n, "%s -p --resume %s ", claude, sid)) return false;
    return cc_shell_quote(text, out + n, cap - n);
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
