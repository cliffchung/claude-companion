#include "term_scroll.h"
#include <stdio.h>
#include <string.h>

size_t term_scroll_page(bool back, char *out, size_t cap)
{
    const char *s = back ? "\x1b[5~" : "\x1b[6~";
    size_t n = strlen(s);
    if (!out || cap <= n) return 0;
    memcpy(out, s, n + 1);
    return n;
}

size_t term_scroll_wheel(bool back, int notches, int col, int row, char *out, size_t cap, int *sent)
{
    if (sent) *sent = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (col < 1) col = 1;
    if (row < 1) row = 1;
    char one[32];
    int m = snprintf(one, sizeof(one), "\x1b[<%d;%d;%dM", back ? 64 : 65, col, row);
    if (m <= 0 || (size_t)m >= sizeof(one)) return 0;
    size_t n = 0;
    int k = 0;
    while (k < notches && n + (size_t)m < cap) { memcpy(out + n, one, (size_t)m); n += (size_t)m; k++; }
    out[n] = 0;
    if (sent) *sent = k;
    return n;
}
