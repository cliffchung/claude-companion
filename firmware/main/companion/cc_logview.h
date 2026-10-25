#pragma once
/* The Sessions output pane's text from `claude logs` (output-pane fix, 2026-10-24).
 *
 * `claude logs` returns the session TUI's raw byte stream: a full-screen cell-diff renderer that draws every row
 * at an absolute position below ESC[H on the main screen (no scrolling, no alternate screen). Its last frames
 * redraw only what changed, mostly the footer, the spinner and the status line, so stripping escapes from the
 * stream (cc_log_tail) shows that chrome instead of the conversation. Here the stream is rendered on a virtual
 * screen (term/vt) and the conversation is read off the final screen instead.
 *
 * Pure C (vt and wcwidth only): built by the host tests. The screen lives in PSRAM and only during the call. */
#include <stdbool.h>
#include <stddef.h>

/* The virtual screen a stream needs, from the bytes after its last full clear (ESC[2J), or all of them:
   rows = the lowest row any cursor move reaches (24..160, 60 when none); cols = the longest run of '─' (the
   full-width frame rules) or the furthest column reached, plus 8, in 80..320 (256 with no rule). A screen larger
   than the recording's draws the same, since the renderer never relies on scrolling or the right margin.
   *start is where rendering begins (the last ESC[2J, or 0); *cleared says whether one was found. */
void cc_logview_size(const char *raw, size_t len, int *cols, int *rows, size_t *start, bool *cleared);

/* Renders raw (len bytes; it needn't start or end on a character or sequence boundary) and writes the last
   max_lines lines of the conversation into out (cap bytes, NUL-terminated, lines joined with '\n', no trailing one):
   - from the top it drops the welcome banner (rows starting with block-element logo glyphs);
   - from the bottom everything from the input box's top rule down (the rule, the "❯" input, its bottom rule and the
     footer and status rows under it: tokens, mode, agents, hooks, the update check); with no input box (a dialog
     is up) blank rows and frame rules at the very bottom;
   - in between, frame rules, blank rows, right-aligned rows (starting past column 24 and past the middle or
     reaching the right edge: the effort, usage notices)
     and the spinner's line (a spinner glyph at column 0: "✻ Baked for 5s",
     "· Computing…", with a "⎿" tip under it);
   - with a dialog up, the description rows under its numbered options (indented past the number), so that the
     question and every option fit the pane;
   - a row the TUI wrapped (the previous row full up to its next word) is joined to the previous one with a space,
     so the pane can wrap it to its own width.
   Returns the number of lines written: 0 (out = "") when the screen has no conversation rows, when the stream had
   no full clear and no input box was found (a partial render that may show only chrome), or when memory ran out.
   The caller then falls back to cc_log_tail. */
int cc_logview_tail(const char *raw, size_t len, int max_lines, char *out, size_t cap);
