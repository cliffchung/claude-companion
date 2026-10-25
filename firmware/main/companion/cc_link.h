#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "companion/cc_sessions.h"
#include "servers/host_rules.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The companion's own SSH link to the Host (separate from the terminal's session).
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
   "<failure> (retry in Ns)" | "<failure> (retrying now)" | "no Host set up", where <failure>
   is "host key changed for <ip>; not connecting", "can't read the host key pin for <ip>; not connecting",
   "new host key SHA256:... for <ip>; check and trust it in Settings › Host" (a LAN address with no key to carry over),
   "Host not found at <host> (set its fallback address in Settings › Host)" (the name doesn't resolve, no fallback),
   "Host not at <fallback> (fallback for <host>)" (the name didn't resolve and the fallback doesn't answer),
   "no Host entry named '<name>' (Settings › Host)", "<name> refused the device key" or CC_LINK_TS_OFF_TEXT. */
const char *cc_link_status_text(char *buf, size_t cap);
/* Why the link is down when it isn't reachability (the status text has the words). */
typedef enum {
    CC_LINK_FAIL_NONE,        /* connected, or unreachable / no Wi-Fi / still trying */
    CC_LINK_FAIL_NO_ENTRY,    /* no server with the configured name */
    CC_LINK_FAIL_HOSTKEY,     /* the Host's host key doesn't match its pin, or the pin is unreadable */
    CC_LINK_FAIL_AUTH,        /* the Host refused the device key */
    CC_LINK_FAIL_TS_OFF,      /* the Host entry is a tailnet host and the Tailscale switch is off */
    CC_LINK_FAIL_NOT_FOUND,   /* the Host's name doesn't resolve, or its fallback address doesn't answer */
} cc_link_fail_t;
#define CC_LINK_TS_OFF_TEXT "the Host is a tailnet host and Tailscale is off (Settings \xe2\x80\xba Network)"
cc_link_fail_t cc_link_failure(void);
/* Forgets the Host's host key: erases the link's pin (the address it connects to) and the
   terminal's (the server entry's host string, and its MagicDNS address), then has the link task
   drop the session (the Tab5's own close: never counted toward the watch fallback) and connect
   again at once, pinning the key it meets as new. ESP_OK with fp_out "SHA256:..." when that
   connect happened within timeout_ms, ESP_ERR_TIMEOUT if not (the pins are cleared either way,
   and the forget stays pending: see cc_link_forget_done). It counts as link-busy while it waits.
   ESP_ERR_INVALID_STATE when no Host is set up. Blocks: never call it from the LVGL task. */
esp_err_t cc_link_forget_host_key(char *fp_out, size_t fp_cap, int timeout_ms);
#define CC_FORGET_TIMEOUT_MS 30000   /* the UI and console wait: a tunnel wait (20 s) plus a connect */
/* A forget that timed out stays pending until the link's next connect pins a key. This counts
   the forgets and trusts that pinned a key so far (it only grows; a cancelled one doesn't count) and
   copies the key the last one pinned; the UI polls it and toasts "New host key pinned: SHA256:..."
   when it grows. Safe from any task. A switch to another Host ends a pending forget or trust
   (cc_link_forget_host_key then returns ESP_ERR_INVALID_RESPONSE if it is still waiting). */
uint32_t cc_link_forget_done(char *fp_out, size_t fp_cap);
/* First-run trust from the screen (polish 1). True when the last connect was refused because the Host presented
   a key that nothing pins or carries over (KHA_REFUSE_UNKNOWN), with that key's "SHA256:..." in fp_out. A changed
   key (CHANGED, a key unlike the carry source, an unreadable pin) is never reported here. Safe from any task. */
bool cc_link_unknown_host_key(char *fp_out, size_t fp_cap);
/* Trusts the key with fingerprint fp ("SHA256:...", the one cc_link_unknown_host_key showed and the user
   confirmed): the same pending forget as cc_link_forget_host_key, with the same results and toast, except that it
   erases no pin and the connect it starts pins only a key whose fingerprint is fp (kh_forget_applies); any other
   key is refused as unknown and the trust stays pending for fp. A pending trust is checked again at each connect
   (kh_trust_allowed): it ends without a pin after KH_TRUST_TTL_MS, on a switch to another Host or an edit of its
   host, and when a pin or carry source appears for it. ESP_ERR_INVALID_STATE when no Host is set up;
   ESP_ERR_NOT_ALLOWED when a key (or an unreadable pin, or a carry source) is already there for it, or fp isn't a
   fingerprint; ESP_ERR_INVALID_RESPONSE when it ended without a pin while waiting; ESP_ERR_TIMEOUT. Blocks: never
   call it from the LVGL task. */
esp_err_t cc_link_trust_host_key(const char *fp, char *fp_out, size_t fp_cap, int timeout_ms);
/* The fingerprint now pinned for the Host ("SHA256:..."), false if none or unreadable. Reads
   NVS: fine from the LVGL task, but not every frame. */
bool cc_link_pinned_fingerprint(char *fp_out, size_t fp_cap);
/* Debug (`companion hostkey corrupt`): overwrites the link's own pin with a wrong value and
   reconnects, so the changed-key path shows without touching the Host. host_out gets host:port. */
esp_err_t cc_link_debug_corrupt_host_key(char *host_out, size_t host_cap);
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
/* Seconds between refreshes: 3 (Active or Dim, any screen), 15 while a session is open
   full-screen, 0 = paused. With the watch running it is the loop's I. */
void cc_poll_set_rate(int seconds);
/* The effective interval is floor > 0 ? floor : rate (Task 5 sets 15 in Off, else 0). */
void cc_poll_set_floor(int seconds);
/* Counts successful results published (watch blocks and heartbeats, polls). */
uint32_t cc_poll_generation(void);
/* True while a paste, live check, host-key forget, voice check or transcription is in flight (a power hold). */
bool cc_link_busy(void);
/* The long-poll watch (spec "Long poll"): one pty exec channel runs cc_cmd_watch's loop and
   replaces the fixed poll while connected; it falls back to polling when it can't start or
   keeps ending, and is retried after 10 min or on the next connect. On by default. */
void cc_link_watch_enable(bool on);
bool cc_link_watch_wanted(void);           /* the `companion watch on|off` choice */
/* A check-in's pause and resume: sets the wish only, never forgets a fallback (cc_link_watch_enable(true) does). */
void cc_link_watch_resume(bool on);
/* Debug: the next watch start gives up on its banner after 1 ms (tests the fallback). */
void cc_link_watch_test_fallback(void);
/* "watch: on I=3s, last block 4s ago, heartbeat 12s ago" | "watch: starting" |
   "watch: fallback (<why>), polling every 3s" | "watch: off", plus "watch: starting (link down)"
   and "watch: off (polling paused)" (rate 0). Safe from any task. */
const char *cc_watch_status_text(char *buf, size_t cap);
typedef void (*cc_sessions_cb_t)(const cc_session_t *s, int n, void *ctx);
/* Called on the poll task after each successful refresh (up to 4 subscribers). */
void cc_poll_subscribe(cc_sessions_cb_t cb, void *ctx);
/* Copies the last successful poll; returns the count, or -1 if there hasn't been one. */
int cc_poll_snapshot(cc_session_t *out, int cap);
/* The esp_timer time (ms) of the last successful result from the Host (a poll, a watch block or a
   watch heartbeat after a good block), or 0 if there has been none.
   Safe from any task; it does not block. */
int64_t cc_poll_last_ok_ms(void);
/* Wi-Fi hook (from the app's global Wi-Fi callback). */
void cc_link_on_wifi(bool connected);

/* Settings */
/* Copy the setting into out (under the lock) and return out. */
const char *cc_settings_claude_path(char *out, size_t cap);   /* default "~/.local/bin/claude" */
int cc_settings_server(void);                /* index in the fork's server list, or -1 */
const char *cc_settings_server_name(char *out, size_t cap);   /* the configured name, "" when none */
/* True when server idx is the companion's Host: its index, or an address cc_same_host matches with any address the
   link knows the Host by (cc_link_host_addresses; also after a MagicDNS lookup of the entry's name). Terminal never
   lists it and the SSH Terminal opens no login shell there (hard rule 1). */
bool cc_link_is_companion_server(int idx);
/* Every address the link knows the Host by: the entry's host and its tailnet IP, the LAN fallback, mac_ts_ip and the
   carry address, the last verified and pinned addresses, the last LAN address, the host cache. Empty with no Host. */
void cc_link_host_addresses(hf_host_addrs_t *out);
/* The Host's entry was renamed (the same entry): follows it without clearing the pin carry-over. False (nothing
   changed) unless the configured name was old_name. */
bool cc_link_rename_server(const char *old_name, const char *new_name);
/* Store a new server name / Claude path and reconnect (server) as needed. */
void cc_link_set_server(const char *name);
bool cc_link_set_claude_path(const char *path);   /* false unless cc_path_valid */

/* ---- Plan 5: voice input (spec §7) ---- */
const char *cc_settings_whisper_path(char *out, size_t cap);    /* default CC_WHISPER_DEFAULT */
const char *cc_settings_whisper_model(char *out, size_t cap);   /* default CC_WHISPER_MODEL_DEFAULT */
bool cc_link_set_whisper_path(const char *path);                /* false unless cc_path_valid; saves */
bool cc_link_set_whisper_model(const char *path);
/* cc_cmd_voice_check on an exec channel (10 s). ESP_OK with *exit_status 0/120/121/…; ESP_ERR_INVALID_STATE when not
   connected or parked; ESP_ERR_TIMEOUT. Blocks: never from the LVGL task. Counts as link-busy. */
esp_err_t cc_link_voice_check(int *exit_status);
/* cc_cmd_transcribe on a non-pty exec channel: writes wav (len bytes) to stdin in 16 KB chunks, draining stdout and
   stderr between chunks, sends EOF, then reads stdout into out (NUL-terminated) and the exit status. *upload_ms
   (may be NULL) is the time from the exec request to the EOF. ESP_OK; ESP_ERR_INVALID_STATE (not connected, or a
   park mid-request); ESP_FAIL (a channel or write error: the session is dropped); ESP_ERR_TIMEOUT (timeout_ms
   from the request's start; the session is dropped, as do_exec does); ESP_ERR_INVALID_SIZE (stdout didn't fit).
   Counts as link-busy (a power hold) while it runs. Logs only lengths, durations and the exit status.
   The only public way to run an exec with stdin (ruling P5-2): it builds its command itself from the settings. */
esp_err_t cc_link_transcribe(const void *wav, size_t len, char *out, size_t cap, int *exit_status,
                             int *upload_ms, int timeout_ms);
#define CC_TRANSCRIBE_TIMEOUT_MS 60000

/* ---- Plan 2 Task 8: Radio off ---- */
/* Parked closes the session (the Tab5's own close: never counted toward the watch fallback) and
   doesn't reconnect; unparked reconnects at once (the backoff starts again). */
void cc_link_park(bool parked);
bool cc_link_parked(void);
/* ---- Plan 3 final review C1/I1: close before the link goes down ---- */
/* Parks the link (a guard park, nested and separate from Radio off's) and waits up to timeout_ms until it
   is idle: no session and no connect in flight. A connect that finishes its auth meanwhile closes its new
   session at once, and a poll or paste in flight gives up. True when idle. Blocks: not from the link task.
   The close-before-down guard (sys/link_guard.h) calls the same park through its registered closer. */
bool cc_link_close_now(int timeout_ms);
/* Drops cc_link_close_now's park; the link reconnects at once unless Radio off still parks it. */
void cc_link_reopen(void);
/* No session and no connect in flight. */
bool cc_link_idle(void);
/* Review M6: the last address whose key the link checked and authenticated on ("" after a refusal, a
   forget or a host change). The only address a take-over may be handed. */
bool cc_link_verified_address(char *out, size_t cap);
/* One plain refresh() now, as a queued request: ESP_OK when it published a result, ESP_FAIL when the
   poll failed, ESP_ERR_INVALID_STATE when not connected, ESP_ERR_TIMEOUT. Blocks up to timeout_ms. */
esp_err_t cc_link_poll_now(int timeout_ms);
/* The stale-peer fix: after a failed ssh_connect to a tailnet IP, kick a handshake before the next
   retry. Console `companion kick on|off` (RAM only, default on). */
void cc_link_set_kick(bool on);
bool cc_link_kick_on(void);
/* The last 8 link down/up events with wall-clock time, one per line (for the Host sleep measurement). */
const char *cc_link_reconnect_log(char *buf, size_t cap);
/* The Host's address for a check-in: the address the link last pinned (a tailnet IP after MagicDNS),
   else the server entry's host. False when no Host is set up. */
bool cc_link_mac_address(char *out, size_t cap);

/* ---- Plan 3 Task 1: the local link ---- */
/* Which path the link uses now: "lan <ip>" or "tailnet <ip>" while connected (true), else "none". */
bool cc_link_mac_path(char *out, size_t cap);
/* Debug (`companion resolve <host>`): resolves host the way the link resolves the Host's (L2). Blocks. */
bool cc_link_resolve_test(const char *host, char *out, size_t cap, const char **how);
/* `companion server-host <host> [fallback-ip]`: sets the Host entry's host (fallback NULL keeps the
   fallback address, "none" clears it), first remembering the entry's tailnet address so its pin
   carries over to the LAN; then reconnects. ESP_ERR_INVALID_ARG for a bad host or fallback,
   ESP_ERR_INVALID_STATE with no Host entry, ESP_FAIL when the server list didn't save. */
esp_err_t cc_link_set_server_host(const char *host, const char *fallback);
/* The Host's fallback LAN address, used when its name doesn't resolve: "" clears it. False unless
   "" or a private IPv4 address. Retries the link at once. */
bool cc_link_set_lan_fallback(const char *ip);
const char *cc_link_lan_fallback(char *out, size_t cap);
/* The carry source's tailnet address: the key last met there carries over to the LAN ("" = none). */
const char *cc_link_tailnet_pin_source(char *out, size_t cap);

/* Registers companion, sessions, reply, logs, wifi and ts console commands. */
void cc_console_init(void);
/* Plan 3 Task 3 Step 4: the Host's last looked-up address (connected to first; the name is looked up only
   when that fails or its pin doesn't match). Console: `companion hostcache [on|off|clear]`. */
void cc_link_host_cache_text(char *out, size_t cap);
void cc_link_host_cache_enable(bool on);   /* measurement only, RAM; on at boot */
void cc_link_host_cache_clear(void);
bool cc_link_host_cache_test_set(const char *ip);   /* debug, RAM only: `companion hostcache set <ip>` */

#ifdef __cplusplus
}
#endif
