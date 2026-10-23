#pragma once
#include <stdbool.h>
#include <stddef.h>

bool cc_path_valid(const char *p);          /* only [A-Za-z0-9_./~-], 1-63 chars */
bool cc_id_valid(const char *id);           /* ^[0-9a-f]{8}$ */
bool cc_session_id_valid(const char *sid);  /* lowercase 8-4-4-4-12 hex UUID */
bool cc_shell_quote(const char *in, char *out, size_t cap);   /* '...' with ' written as '\'' */
bool cc_cmd_agents(const char *claude, char *out, size_t cap);
bool cc_cmd_logs(const char *claude, const char *id, char *out, size_t cap);
/* Reserved: not called by the product. The device only answers live sessions, by bracketed
   paste over a pty (cc_link_paste, no shell involved). Kept, with its D4 quoting tests, for a
   later reply to a stopped session (`claude -p --resume <sid> -- '<text>'`). */
bool cc_cmd_reply(const char *claude, const char *sid, const char *text, char *out, size_t cap);
bool cc_cmd_attach(const char *claude, const char *id, char *out, size_t cap);
/* Removes CSI and OSC escape sequences in place; returns the new length. */
size_t cc_strip_ansi(char *s);
/* Turns raw TUI output (`claude logs`) into its last max_lines readable lines, in place:
   cursor-forward and cursor-to-column become spaces, cursor positioning starts a new line,
   other escapes go,
   lines are split on \r and \n and trimmed, and lines of only blanks or box drawing are
   dropped, and so are lines without a word (2+ letters or 3+ characters: spinner leftovers). The kept lines are joined with \n. Returns how many were kept. */
int cc_log_tail(char *s, int max_lines);
/* Bracketed paste for a TUI input box: ESC[200~ + text + ESC[201~ + CR, NUL-terminated.
   Drops ESC, DEL and every C0 control byte except '\n' from text, so it can't end the paste
   early or inject keys. Returns the byte count (without the NUL), or 0 for empty text
   (also after sanitizing) or when it doesn't fit cap. */
size_t cc_paste_build(const char *text, char *out, size_t cap);
/* True when ANSI-stripped TUI output ends in a permission dialog: the last selected numbered
   option ("❯ 1.") comes after the last empty input line ("❯ " + rule or line end), with at
   least two numbered options and an esc/cancel hint. History text doesn't count. A CR there
   would confirm the dialog's default option, so quick answers must not be typed. */
bool cc_screen_is_permission_prompt(const char *plain);
/* Fail-closed gate for quick answers: true only when the last "❯" in the ANSI-stripped screen
   is an empty input line, i.e. only spaces, NBSP or box-drawing follow it up to the next \r,
   \n or the end. Dialogs, spinners without an input line, drafts and unknown screens: false. */
bool cc_screen_input_ready(const char *plain);
/* Every builder returns false for an invalid path, id or sid, empty text, or output that doesn't fit cap. */
/* True when two host strings name the same machine: equal ignoring case, surrounding blanks
   and one trailing dot, or one is a bare name (with a letter, no dot) that is the other's first
   DNS label (MagicDNS "mac" and "mac.tail1234.ts.net"). Empty or NULL never matches. */
bool cc_same_host(const char *a, const char *b);
/* The SSH Terminal's guard (no interactive shell on the companion's Mac): true when server idx
   with host is the companion server, by index (comp_idx >= 0) or by cc_same_host(host,
   comp_host). comp_host may be NULL or "" when the companion entry is missing. */
bool cc_is_companion_server(int idx, const char *host, int comp_idx, const char *comp_host);
