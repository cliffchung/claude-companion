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
/* Writes the status into buf and returns buf (safe from any task):
   "connected <host>" | "connecting to <host>" (attempt running) |
   "connecting to <host> (retry in Ns)" | "host key changed for <host> (retry in Ns)" |
   "no Mac set up". */
const char *cc_link_status_text(char *buf, size_t cap);
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
   wait for quiet, Ctrl+U x4, bracketed paste, CR, Ctrl+Z. Refuses (ESP_ERR_NOT_ALLOWED, err
   "needs approval — open the session") when the screen shows a permission dialog. */
esp_err_t cc_link_paste(const char *id, const char *text, char *err, size_t err_cap);
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
/* Store a new server name / Claude path and reconnect (server) as needed. */
void cc_link_set_server(const char *name);
bool cc_link_set_claude_path(const char *path);   /* false unless cc_path_valid */

/* Registers companion, sessions, reply, logs and wifi console commands. */
void cc_console_init(void);
#ifdef __cplusplus
}
#endif
