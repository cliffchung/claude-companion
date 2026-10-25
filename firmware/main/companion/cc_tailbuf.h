#pragma once
/* The newest bytes of an exec's stdout, kept in a ring (no copy per chunk; one rotation at the end).
 *
 * With from_clear set it also keeps them from the stream's latest full clear (ESC[2J, found across chunk
 * boundaries too): a clear empties the buffer and starts it again with the clear, so a `claude logs` stream of any
 * length leaves exactly what its screen needs, when that fits (output-pane fix round 1, I1/I2).
 *
 * Pure C: built by the host tests. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t room;      /* cap - 1: one byte stays for the NUL */
    size_t head;      /* the oldest byte, once the ring has wrapped */
    size_t len;
    bool from_clear;
    bool dropped;     /* bytes lost from the front: since the last clear (from_clear) or since the start */
    bool cleared;     /* a full clear was seen (from_clear) */
    uint8_t match;    /* how much of ESC [ 2 J the last bytes matched */
} cc_tailbuf_t;

void cc_tailbuf_init(cc_tailbuf_t *t, char *buf, size_t cap, bool from_clear);
/* O(n): each byte is stored once; nothing already stored moves. */
void cc_tailbuf_put(cc_tailbuf_t *t, const char *p, size_t n);
/* Rotates the ring into order in place (O(cap), once) and NUL-terminates it; returns the length. The buffer
   then starts at the latest clear (from_clear, when one was seen and nothing since was dropped). */
size_t cc_tailbuf_finish(cc_tailbuf_t *t);
