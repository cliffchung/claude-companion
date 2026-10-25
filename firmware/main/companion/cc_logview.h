#pragma once
/* The Sessions output pane's text from `claude logs` (output-pane fix, 2026-10-24).
 *
 * `claude logs` returns the session TUI's raw byte stream: a full-screen cell-diff renderer that draws every row
 * at an absolute position below ESC[H on the main screen (no scrolling, no alternate screen). Its last frames
 * redraw only what changed, mostly the footer, the spinner and the status line, so stripping escapes from the
 * stream (cc_log_tail) shows that chrome instead of the conversation. Here the stream is rendered on a virtual
 * screen (term/vt) and the conversation is read off the final screen instead.
 *
 * The render needs every byte since the stream's latest full clear (ESC[2J): a diff renderer never redraws an
 * unchanged row, so a stream cut after its clear leaves a sparse screen. cc_link_exec_screen keeps exactly those
 * bytes (cc_tailbuf); a cut stream is refused here and the caller falls back to cc_log_tail.
 *
 * A busy session (busy-pane fix, 2026-10-25): `claude logs` itself keeps only the session's recent output (32-72 KB
 * measured, trimmed at a frame start), so a long turn's spinner redraws push the clear and the first full frame out
 * of it. Its screen is then sparse: no rules, no input box, no footer, only the rows redrawn since (the conversation
 * where the screen moved, the spinner, stale counter fragments). It is read up to the spinner, the lowest one within
 * 12 rows above the parked cursor. A row redrawn by cell diff can miss the cells that didn't change (a letter in the
 * same place on the row before); only a repaint (attach, resize) would bring those back.
 *
 * The limit (accepted, fix round 2): the bytes since the latest clear must fit 512 KB (LOGS_CAP in ui_sessions.c).
 * Claude Code clears on start, attach and resize, so a long-running session that nobody attaches to can pass that;
 * its pane then shows the old escape-stripped tail. A stateful streaming replay (a persistent vt per session fed
 * only the new bytes, `claude logs <id> | tail -c +<offset>`) would lift the limit; it isn't built.
 *
 * Pure C (vt and wcwidth only): built by the host tests. The screen lives in PSRAM and only during the call. */
#include <stdbool.h>
#include <stddef.h>

/* Results of cc_logview_tail besides a line count (> 0). The caller falls back on any of them. */
#define CC_LV_NONE        0    /* no conversation rows (or bad arguments) */
#define CC_LV_PARTIAL    (-1)  /* no full clear in the bytes and their front was cut: the screen would be sparse */
#define CC_LV_TOO_BIG    (-2)  /* the recording is wider than 312 columns or taller than 160 rows */
#define CC_LV_NO_MEMORY  (-3)
#define CC_LV_TOO_COSTLY (-4)  /* the replay passed CC_LOGVIEW_WORK_MAX */
#define CC_LV_NO_LAYOUT  (-5)  /* no input box, no dialog, no rule near the bottom, no busy spinner: an unknown screen */

/* The replay's budget in vt_work units: one per input byte plus every cell written, cleared, moved or stepped over.
   Claude Code's own streams cost 2-5 units a byte (measured on 21 live streams), so a full 512 KB buffer needs at
   most about 2.6 M. vt_feed stops once past the budget, so a stream of scrolls, erases, DECALN or tab moves ends
   within one sequence of it: at most about 4.2 M cells (a full-screen sequence is ≤ 320 x 160 = 51 K cells, or
   320 x 320 steps for a clamped tab move), i.e. about 67 MB of 16-byte cell writes, twice that for a move. */
#define CC_LOGVIEW_WORK_MAX ((size_t)4 * 1024 * 1024)

/* The virtual screen a stream needs, from the bytes after its last full clear (ESC[2J), or all of them:
   rows = the lowest row any cursor move reaches (24..160, 60 when none); cols = the longest run of '─' (the
   full-width frame rules) or the furthest column reached, plus 8, in 80..320 (256 with no rule). A screen larger
   than the recording's draws the same, since the renderer never relies on scrolling or the right margin.
   *start is where rendering begins (the last ESC[2J, or 0); *cleared says whether one was found.
   Returns false when the recording doesn't fit the largest screen (the values are then clamped). */
bool cc_logview_size(const char *raw, size_t len, int *cols, int *rows, size_t *start, bool *cleared);

/* Renders raw (len bytes; it needn't start or end on a character or sequence boundary) and writes the last
   max_lines lines of the conversation into out (cap bytes, NUL-terminated, lines joined with '\n', no trailing one).
   complete: raw holds the stream from its start, or from a full clear in it (cc_link_exec_screen returned ESP_OK);
   with complete false only a clear inside raw makes it renderable.
   - from the top it drops the welcome banner (rows starting with block-element logo glyphs);
   - the conversation ends at a dialog's "Esc to cancel" hint when that is the lowest drawn row with two or more
     numbered options and a ❯ above it (the hint stays); else at the input box's top rule (a rule with "❯" at
     column 0 under it, above the lowest rule within 8 rows of the last drawn row: the input, however tall or
     numbered, its bottom rule and the footer and status rows under it go: tokens, mode, agents, hooks); else at a
     rule within 8 rows of the last drawn row; else, with no clear in raw (a busy session's sparse screen, above),
     at its spinner, which goes too; else CC_LV_NO_LAYOUT;
   - in between it drops frame rules, blank rows, right-aligned status (starting past column 24 and ending at the
     right edge: the effort, usage notices, the update check), the spinner's line (a spinner glyph at column 0, a
     verb and "…" with an optional "(…)", or "<verb> for <time>": "✻ Baked for 5s", "· Computing…") with a "⎿" tip
     under it, and, with a dialog up, the description rows under its numbered options (indented past the number),
     so that the question and every option fit the pane; a row that is only a key hint ("(ctrl+b to run in
     background)"); on a sparse screen also rows starting past column 24 and rows without a word (two letters in
     a row, or a CJK character): a moved spinner's stale time and counters. Tool calls stay as the TUI draws them,
     already compact: "⏺ Running 1 shell command · 44s…", its "⎿  $ command (42s · 43 lines)", "Listed 1 directory";
   - a row the TUI wrapped (the previous row full up to its next word, and the row not starting an item: a number
     and ".", a bullet, ⏺, ❯, ⎿) is joined to the previous one, with a space unless either side is a wide (CJK)
     character, so the pane can wrap it to its own width.
   Returns the number of lines written, or one of the CC_LV_ results above (out = "" then). */
int cc_logview_tail(const char *raw, size_t len, bool complete, int max_lines, char *out, size_t cap);
