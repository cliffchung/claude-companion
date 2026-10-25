Vendored from https://github.com/Csontikka/microlink (MIT, Cameron Malone), MicroLink v2.
Unofficial Tailscale-compatible client: ts2021 control protocol, WireGuard (wireguard-lwip),
DERP relay, DISCO/STUN. The WireGuard netif claims 100.64.0.0/10 in lwIP, so ordinary BSD
sockets (libssh) reach tailnet peers directly.
Local changes: none besides moving wireguard_lwip to a sibling component directory.
- ml_wg_mgr.c: config.enable_disco=false now really disables direct-path upgrades (relay-only mode).
- ml_coord.c: advertise the netcheck-chosen home DERP as PreferredDERP (was always the compile-time default).
- wireguardif.h: WIREGUARDIF_MTU 1420 -> 1280 (Tailscale tunnel MTU).
- microlink.c/.h: microlink_destroy_finish() + the ml_free_instance() split out of microlink_destroy()
  (Claude Companion Plan 3, rulings L6/L15): frees an instance microlink_stop() already stopped, once its
  last task exits, without stopping it twice. Required by main/net/tailscale.c; keep it on a re-vendor.
- ml_netcheck.c: the netcheck (on the coord task, up to 25 s) ends early once ML_EVT_SHUTDOWN_REQUEST is set: it
  checks before each region and at least every 250 ms while it waits, and then returns no region. Before this, a
  stop during the first map outlived microlink_stop()'s 15 s wait and was logged as a leaked instance (Claude
  Companion polish 2).
- ml_derp.c: the DERP connect and reconnect retry loops stop retrying once ML_EVT_SHUTDOWN_REQUEST is set
  (derp_stopping(), before each attempt and after each backoff). Same reason (polish 2).
- ml_coord.c: the coord task's wait for the first DERP connect (up to 15 s) also wakes on ML_EVT_SHUTDOWN_REQUEST
  and then skips the long-poll. With the ml_derp.c change the connect never comes after a stop, so the wait sat out
  its whole 15 s, as long as microlink_stop()'s own wait, and the device still logged "leaked" (polish 2 device pass).
