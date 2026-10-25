#pragma once
/* Settings (spec §5.6–§5.12): a 320 px rail of six sections and one scrolling panel. Only the visible
 * section is built; a rail click cleans and refills that one panel on the same screen object (B2). */
#include "lvgl.h"
#include "net/tailscale.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_SET_HOST, UI_SET_NETWORK, UI_SET_POWER, UI_SET_KBD, UI_SET_DISPLAY, UI_SET_ABOUT, UI_SET_COUNT } ui_set_section_t;

/* Pushes Settings on section s. ui_settings_create(NULL) (Ctrl+P, the footer chip) lands on the last used
 * section: Host after a boot (RAM only). */
void ui_settings_open(ui_set_section_t s);
lv_obj_t *ui_settings_create(void *arg);

/* Rebuilds the shown section's panel in place, keeping its scroll position (after a row changed a value).
 * Does nothing while Settings isn't the current screen. */
void ui_settings_refill(void);
/* A Host key was pinned (a forget or a trust done): refills like ui_settings_refill, but a keyboard focus in Settings ›
 * Host's panel goes to its first control, never the new Forget row (fix round 1, M7). */
void ui_settings_key_pinned(void);

/* ---- Settings › Network (ui_settings_net.c, spec §5.8) ---- */
/* The Network section's rows into panel: Wi-Fi (the current network, Networks, Advanced), then Tailscale in
 * U10's order (switch, Status, Auth key, Device name, Control server, Connection, Peers, Forget). */
void ui_settings_fill_network(lv_obj_t *panel);

typedef enum { UI_NET_SUB_NETWORKS, UI_NET_SUB_ADVANCED, UI_NET_SUB_PEERS } ui_net_sub_t;
/* A pushed Network sub-page; arg is a ui_net_sub_t ((void *)(intptr_t)sub). Its header is
 * ui_header(scr, <title>, "Settings"); Back returns to Settings › Network. */
lv_obj_t *ui_settings_sub_create(void *arg);
/* Opens Settings › Network, then the sub-page sub on top of it (the console's `ui net <sub>`). */
void ui_settings_open_net_sub(ui_net_sub_t sub);
/* Peers › Add as SSH host: opens the Terminal host form prefilled with p (Task 9); until then a toast. */
void ui_settings_add_peer_host(const ts_peer_t *p);

#ifdef __cplusplus
}
#endif
