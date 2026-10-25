#pragma once
#include <stdbool.h>
#include <stddef.h>

bool cc_path_valid(const char *p);          /* only [A-Za-z0-9_./~-], 1-63 chars */
bool cc_id_valid(const char *id);           /* ^[0-9a-f]{8}$ */
bool cc_session_id_valid(const char *sid);  /* lowercase 8-4-4-4-12 hex UUID */
bool cc_shell_quote(const char *in, char *out, size_t cap);   /* '...' with ' written as '\'' */
bool cc_cmd_agents(const char *claude, char *out, size_t cap);
/* `<claude> logs <id> | tail -c CC_LOGS_TAIL_BYTES` */
#define CC_LOGS_TAIL_BYTES 524288
bool cc_cmd_logs(const char *claude, const char *id, char *out, size_t cap);
/* Reserved: not called by the product. The device only answers live sessions, by bracketed
   paste over a pty (cc_link_paste, no shell involved). Kept, with its D4 quoting tests, for a
   later reply to a stopped session (`claude -p --resume <sid> -- '<text>'`). */
bool cc_cmd_reply(const char *claude, const char *sid, const char *text, char *out, size_t cap);
bool cc_cmd_attach(const char *claude, const char *id, char *out, size_t cap);
/* The long-poll loop (spec "Long poll"): `exec /bin/sh -c '<loop>'` with fixed loop text; only
   the Claude path and the four integers vary. The loop runs `<claude> agents --json --all` every
   interval_s, prints it between @@CC-BEGIN <rc> / @@CC-END when it changed, @@CC-HB <s> after
   heartbeat_s of silence, and exits (@@CC-BYE eof|quiet|life) on stdin EOF, no keepalive byte for
   quiet_s, or after life_s. Parsed by cc_watch.h. */
typedef struct { int interval_s, heartbeat_s, quiet_s, life_s; } cc_watch_params_t;
/* ranges: interval 1..120, heartbeat 1..600, quiet 2..600, life 5..7200 */
bool cc_cmd_watch(const char *claude, const cc_watch_params_t *p, char *out, size_t cap);
#define CC_WATCH_KEEPALIVE_S 20     /* K: the Tab5 writes "\n" this often */
/* product values: interval 3 (Active/Dim) or 15 (Off), heartbeat 60, quiet 3K = 60, life 1800 */
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
/* Voice in a session (follow-up 2026-10-24): a transcript typed into a take-over's or an SSH session's pty as
   input, never sent. bracketed: ESC[200~ + text + ESC[201~ (always in a take-over; in an SSH session only when the
   remote side turned on DECSET 2004); otherwise the bare text. No Ctrl+U clear, no Enter. From text it drops every
   C0 control except that \n, \r, \t, \v and \f become a space, and drops DEL, ESC, invalid UTF-8 (overlong,
   surrogates, past U+10FFFF) and C1 controls (U+0080..U+009F, NEL included); whitespace runs collapse to one space
   and both ends are trimmed. So the text is never sent with Enter and can't end the paste early, but it IS typed:
   a program that acts on single keys (a Claude Code selector, less, mutt, vi normal mode) acts on it, so the caller
   types it only at a prompt. join: a dictation already typed before it, so text that starts with an ASCII letter or
   digit gets one leading space. NUL-terminated; returns the byte count (without the NUL), or 0 (out = "") for empty
   text (also after sanitizing) or when it doesn't fit cap. */
size_t cc_voice_type_build_ex(const char *text, bool bracketed, bool join, char *out, size_t cap);
size_t cc_voice_type_build(const char *text, bool bracketed, char *out, size_t cap);   /* join false */
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
/* The SSH Terminal's guard (no interactive shell on the companion's Host): true when server idx
   with host is the companion server, by index (comp_idx >= 0) or by cc_same_host(host,
   comp_host). comp_host may be NULL or "" when the companion entry is missing. */
bool cc_is_companion_server(int idx, const char *host, int comp_idx, const char *comp_host);

/* Voice input (spec §7.1, §7.2). Both run on a plain exec channel, never a pty. */
#define CC_WHISPER_DEFAULT        "/opt/homebrew/bin/whisper-cli"
#define CC_WHISPER_MODEL_DEFAULT  "~/.local/share/whisper/ggml-base.en.bin"
#define CC_VOICE_RC_NO_WHISPER 120   /* the whisper path isn't an executable file */
#define CC_VOICE_RC_NO_MODEL   121   /* the model path isn't a readable file */
/* spec §7.1: stdin is a 16 kHz mono 16-bit WAV, stdout the transcript. false for an invalid path or a small cap (out cleared). */
bool cc_cmd_transcribe(const char *whisper, const char *model, char *out, size_t cap);
/* spec §7.2: exit 0, 120 or 121; no output. */
bool cc_cmd_voice_check(const char *whisper, const char *model, char *out, size_t cap);

/* Firmware updates (Plan 6, spec §9.1). Both run on a plain exec channel, never a pty (binary stdout). */
#define CC_FW_PATH_DEFAULT  "~/.local/share/claude-companion/claude_companion.bin"
#define CC_FW_RC_NO_FILE    120   /* not a readable regular file */
#define CC_FW_RC_SIZE       121   /* wc failed */
#define CC_FW_RC_HASH       122   /* no hash tool worked */
/* spec §9.1: prints "<size> <sha256>\n" then the first 288 bytes. false for an invalid path or a small cap (out cleared). */
bool cc_cmd_fw_info(const char *path, char *out, size_t cap);
/* spec §9.1: prints the file. */
bool cc_cmd_fw_read(const char *path, char *out, size_t cap);
