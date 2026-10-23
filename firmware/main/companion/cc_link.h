#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "companion/cc_sessions.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The companion's own SSH link to the Mac (separate from the terminal's session).
 * One task owns the libssh session; every call below hands its work to that task
 * and waits for the answer, so callers never touch libssh themselves.
 * Every channel is an exec with an explicit command: never an interactive login shell. */

/* Connects to the companion server over the tailnet using the device key;
   reconnects with backoff 1, 2, 4, 8, 16, 30 s. */
esp_err_t cc_link_start(void);
bool cc_link_connected(void);
#define CC_STATUS_MAX 160
#define CC_POLL_FAILS_SHOWN 3   /* failed polls in a row before the status says so */
/* Writes the status into buf and returns buf (safe from any task):
   "connected <host>" | "connected <host>; session list over 256 KB, not updating" |
   "connected <host>; session list not updating (N polls failed)" |
   "connecting to <host>" (attempt running) | "connecting to <host> (retry in Ns)" |
   "<failure> (retry in Ns)" | "<failure> (retrying now)" | "no Mac set up", where <failure>
   is "host key changed for <ip>; not connecting", "no server named '<name>' in the server
   list" or "<name> refused the device key". */
const char *cc_link_status_text(char *buf, size_t cap);
/* Why the link is down when it isn't reachability (the status text has the words). */
typedef enum {
    CC_LINK_FAIL_NONE,        /* connected, or unreachable / no Wi-Fi / still trying */
    CC_LINK_FAIL_NO_ENTRY,    /* no server with the configured name */
    CC_LINK_FAIL_HOSTKEY,     /* the Mac's host key doesn't match its pin */
    CC_LINK_FAIL_AUTH,        /* the Mac refused the device key */
} cc_link_fail_t;
cc_link_fail_t cc_link_failure(void);
/* Failed polls in a row while connected (0 when not connected); *too_big (may be NULL) is
   set when the last one failed because `agents` output didn't fit the poll buffer. */
int cc_poll_failures(bool *too_big);
/* One exec channel on the shared session; out holds stdout and err the first line of stderr,
   both NUL-terminated. Returns ESP_ERR_INVALID_STATE when not connected, ESP_ERR_TIMEOUT on
   timeout, ESP_ERR_INVALID_SIZE when stdout didn't fit (out holds the start). */
esp_err_t cc_link_exec(const char *cmd, char *out, size_t cap, int *exit_status,
                       char *err, size_t err_cap, int timeout_ms);
/* Same, but when stdout doesn't fit, out keeps its newest bytes (ESP_ERR_INVALID_SIZE). */
esp_err_t cc_link_exec_tail(const char *cmd, char *out, size_t cap, int *exit_status,
                            char *err, size_t err_cap, int timeout_ms);
/* Types text into a running background session: refreshes the session list first (attach
   would wake a stopped session) and returns ESP_ERR_NOT_FOUND with err "no such session",
   "session has stopped" or "not a background session"; then pty exec `<claude> attach <id>`,
   wait for quiet, Ctrl+U x4, bracketed paste, CR, Ctrl+Z. Types nothing unless the screen
   ends in the empty input prompt: CC_PASTE_NEEDS_APPROVAL (err "needs approval — open the
   session") for a permission dialog, CC_PASTE_NOT_READY for any other screen. */
esp_err_t cc_link_paste(const char *id, const char *text, char *err, size_t err_cap);
/* The two refusals of cc_link_paste, where nothing was typed: */
#define CC_PASTE_NEEDS_APPROVAL ESP_ERR_NOT_ALLOWED      /* the screen ends in a permission dialog */
#define CC_PASTE_NOT_READY      ESP_ERR_INVALID_RESPONSE /* not the empty input prompt (a draft, queued messages ...) */
/* Refreshes the session list and checks that id is a running background session, the way
   cc_link_paste does before it attaches (`claude attach` would wake a stopped session).
   ESP_OK, or ESP_ERR_NOT_FOUND with err "no such session" / "session has stopped" /
   "not a background session"; ESP_ERR_INVALID_STATE when not connected. */
esp_err_t cc_link_check_live(const char *id, char *err, size_t err_cap);
/* Seconds between refreshes: 3 while the dashboard is visible, 30 when dimmed, 0 = paused. */
void cc_poll_set_rate(int seconds);
typedef void (*cc_sessions_cb_t)(const cc_session_t *s, int n, void *ctx);
/* Called on the poll task after each successful refresh (up to 4 subscribers). */
void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx);
/* Copies the last successful poll; returns the count, or -1 if there hasn't been one. */
int cc_poll_snapshot(cc_session_t *out, int cap);
/* Wi-Fi hook (from the app's global Wi-Fi callback). */
void cc_link_on_wifi(bool connected);

/* Settings */
/* Copy the setting into out (under the lock) and return out. */
const char *cc_settings_claude_path(char *out, size_t cap);   /* default "~/.local/bin/claude" */
int cc_settings_server(void);                /* index in the fork's server list, or -1 */
const char *cc_settings_server_name(char *out, size_t cap);   /* the configured name, "" when none */
/* True when server idx is the companion's Mac: its index, or the same host as the companion
   entry (also after a MagicDNS lookup). The SSH Terminal opens no login shell there. */
bool cc_link_is_companion_server(int idx);
/* Store a new server name / Claude path and reconnect (server) as needed. */
void cc_link_set_server(const char *name);
bool cc_link_set_claude_path(const char *path);   /* false unless cc_path_valid */

/* Registers companion, sessions, reply, logs and wifi console commands. */
void cc_console_init(void);
#ifdef __cplusplus
}
#endif
