#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "net/ts_host.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Tailscale membership via the vendored MicroLink client. */
typedef enum { TS_OFF = 0, TS_WAIT_WIFI, TS_CONNECTING, TS_REGISTERING, TS_CONNECTED, TS_RECONNECTING, TS_ERROR } ts_state_t;
typedef struct { char hostname[64]; char ip[16]; bool online, direct, exit_node; uint16_t derp_region; } ts_peer_t;
uint16_t tailscale_home_derp(void);
/* The home DERP region's code ("sea"), or its number when the map has no code; false (out "") with no
   region yet (spec §9 copy rule 7: the one internal name the UI shows, as "DERP sea"). */
bool tailscale_home_derp_code(char *out, size_t cap);
typedef void (*ts_event_cb_t)(void *user);

void tailscale_init(void);                 /* reads settings; starts if enabled and WiFi is up */
/* Starts MicroLink: a no-op (false, "Tailscale is off") while the switch is off. Needs the auth key
   the first time, and Wi-Fi. */
bool tailscale_start(void);
void tailscale_stop(void);
/* ---- Plan 3 Task 1: the Tailscale switch ---- */
/* The sticky switch (g_settings.ts_enable): saves it to NVS at once, then starts MicroLink (Wi-Fi
   up) or stops it and frees it. While off nothing calls microlink_init, _start or _rebind. */
void tailscale_set_enabled(bool on);
bool tailscale_enabled(void);
/* True while a switched-off instance is still stopping or not yet freed (UI: "Turning Tailscale off…"). */
bool tailscale_stopping(void);
/* True while tailscale_forget_node runs; a second call meanwhile returns false at once (N4). */
bool tailscale_forgetting(void);
/* The SSH terminal's connect refusal for a tailnet host while the switch is off (ruling C15: the Terminal
   banner has its own string, STR_TS_OFF_BANNER). */
#define TS_OFF_MESSAGE TS_HOST_OFF_MESSAGE   /* net/ts_host.h: the pure precheck returns it */
void tailscale_on_wifi(bool connected);   /* called from the WiFi manager hook */
ts_state_t tailscale_state(void);
const char *tailscale_state_text(void);
const char *tailscale_vpn_ip(void);       /* "" if not connected */
int  tailscale_peer_count(void);
bool tailscale_peer(int i, ts_peer_t *out);
/* True if ip is a tailnet address (100.64.0.0/10). */
bool tailscale_is_tailnet_ip(const char *ip);
/* Wake the WireGuard session to a peer (handshake + DISCO) and wait until it is up.
 * Must be called before opening a plain TCP socket to a tailnet address. */
bool tailscale_prepare_peer(const char *ip, uint32_t timeout_ms);
/* Diagnostics: log lwIP netifs/route for ip and try a raw TCP connect (timeout ms). */
void tailscale_tcp_probe(const char *ip, int port, int timeout_ms);
/* MagicDNS-style lookup of a tailnet hostname; returns false if unknown. */
bool tailscale_resolve(const char *host, char *ip_out, size_t cap);
void tailscale_set_event_cb(ts_event_cb_t cb, void *user);
/* Final review C1: called around every stop of a live instance (the switch, `ts off`,
   tailscale_stop, a node forget), before the tunnel goes: hook(stop, ctx) must close the SSH sessions over
   tailnet addresses and then call stop(ctx) exactly once. Not called with s_ctl held. */
typedef void (*ts_stop_hook_t)(void (*stop)(void *ctx), void *ctx);
void tailscale_set_stop_hook(ts_stop_hook_t hook);
bool tailscale_forget_node(void);         /* factory reset of node keys (re-auth needed) */
const char *tailscale_last_error(void);

/* ---- Plan 2 Task 8: Radio-off check-ins ---- */
/* While on, tailscale_on_wifi(true) doesn't call microlink_rebind: a check-in tries the
   WireGuard handshake first and rebinds only when that fails. */
void tailscale_set_fast_resume(bool on);
bool tailscale_fast_resume(void);
/* TS_CHECKIN_SKIPPED (Plan 3): no tunnel was needed (the Mac is on the LAN, or Tailscale is off). */
typedef enum { TS_CHECKIN_HANDSHAKE, TS_CHECKIN_REBIND, TS_CHECKIN_FAILED, TS_CHECKIN_SKIPPED } ts_checkin_t;
const char *tailscale_checkin_name(ts_checkin_t c);   /* "handshake", "rebind", "failed", "lan" */
/* Brings the tunnel to the Mac up. host is a tailnet IP or a MagicDNS name (resolved here). Waits up to
   60 s for MicroLink to be connected (a cold boot registers first), then kicks
   ml_wg_mgr_trigger_handshake + ml_wg_mgr_send_cmm and waits up to 8 s for ml_wg_mgr_peer_is_up. Only
   when that fails (or force_rebind) it calls microlink_rebind, waits up to 60 s for
   ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED and handshakes again. A host that isn't on the
   tailnet needs no tunnel: HANDSHAKE at once. *ms_out (may be NULL) gets the time taken. Blocks. */
ts_checkin_t tailscale_checkin(const char *host, bool force_rebind, uint32_t *ms_out);
/* Kicks a handshake (and CMM) to a tailnet peer without waiting: the stale-peer fix. */
void tailscale_kick_peer(const char *ip);
#ifdef __cplusplus
}
#endif
