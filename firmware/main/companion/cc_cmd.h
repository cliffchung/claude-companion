#pragma once
#include <stdbool.h>
#include <stddef.h>

bool cc_path_valid(const char *p);          /* only [A-Za-z0-9_./~-], 1-63 chars */
bool cc_id_valid(const char *id);           /* ^[0-9a-f]{8}$ */
bool cc_session_id_valid(const char *sid);  /* lowercase 8-4-4-4-12 hex UUID */
bool cc_shell_quote(const char *in, char *out, size_t cap);   /* '...' with ' written as '\'' */
bool cc_cmd_agents(const char *claude, char *out, size_t cap);
bool cc_cmd_logs(const char *claude, const char *id, char *out, size_t cap);
bool cc_cmd_reply(const char *claude, const char *sid, const char *text, char *out, size_t cap);
bool cc_cmd_attach(const char *claude, const char *id, char *out, size_t cap);
/* Removes CSI and OSC escape sequences in place; returns the new length. */
size_t cc_strip_ansi(char *s);
/* Bracketed paste for a TUI input box: ESC[200~ + text + ESC[201~ + CR, NUL-terminated.
   Drops ESC, DEL and every C0 control byte except '\n' from text, so it can't end the paste
   early or inject keys. Returns the byte count (without the NUL), or 0 for empty text
   (also after sanitizing) or when it doesn't fit cap. */
size_t cc_paste_build(const char *text, char *out, size_t cap);
/* True when ANSI-stripped TUI output shows a permission dialog (case-insensitive:
   "do you want to", "1. yes", "don't ask again", or the word "allow"). A CR there would
   confirm the dialog's default option, so quick answers must not be typed. */
bool cc_screen_is_permission_prompt(const char *plain);
/* Every builder returns false for an invalid path, id or sid, empty text, or output that doesn't fit cap. */
