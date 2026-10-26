#pragma once
/* Scrolling a full-screen app on the Host from the Tab5 (follow-up 2026-10-25). Claude Code draws on the alternate
 * screen and scrolls its conversation itself, so the terminal's own scrollback stays empty in a take-over: the scroll
 * keys and a drag send it what a desktop terminal would. Measured against Claude Code 2.1.310 (`claude attach`, 106 x 40):
 *   PgUp ESC[5~ / PgDn ESC[6~                  scroll the conversation 16 rows back / forward;
 *   SGR wheel ESC[<64;x;yM / ESC[<65;x;yM      1 row back / forward each, at any x;y (it turns on ?1000/1002/1003/1006);
 *   Ctrl+Up/Down, Alt+Up/Down                  don't scroll it. Pure C, host-tested in test/host/test_term_scroll.c. */
#include <stdbool.h>
#include <stddef.h>

/* PgUp (back = older) or PgDn into out, NUL-terminated. Returns the length, 0 when cap is too small. */
size_t term_scroll_page(bool back, char *out, size_t cap);
/* Up to `notches` SGR mouse-wheel reports (wheel up = back) at cell col;row (1-based; anything below 1 counts as 1),
   as many whole reports as fit in cap with the NUL. Returns the length; *sent (if not NULL) gets the reports written. */
size_t term_scroll_wheel(bool back, int notches, int col, int row, char *out, size_t cap, int *sent);
