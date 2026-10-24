#pragma once
/* Settings (spec §5.6–§5.12): a 320 px rail of six sections and one scrolling panel. Only the visible
 * section is built; a rail click cleans and refills that one panel on the same screen object (B2). */
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_SET_HOST, UI_SET_NETWORK, UI_SET_POWER, UI_SET_KBD, UI_SET_DISPLAY, UI_SET_ABOUT, UI_SET_COUNT } ui_set_section_t;

/* Pushes Settings on section s. ui_settings_create(NULL) (Ctrl+P, the footer chip) lands on the last used
 * section: Host after a boot (RAM only). */
void ui_settings_open(ui_set_section_t s);
lv_obj_t *ui_settings_create(void *arg);

/* The Network section's rows into panel. Task 8 rebuilds them in ui_settings_net.c; until then they are the
 * Plan 3 rows (the Tailscale switch, the fallback, the static address and the P4-9 Wi-Fi bridge row). */
void ui_settings_fill_network(lv_obj_t *panel);

#ifdef __cplusplus
}
#endif
