#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The header's right-hand side: battery glyph + %, charging bolt, Wi-Fi bars, Tailscale mark,
   link dot + host, clock. `parent` is the bar (a flex row); the result is the group inside it. */
lv_obj_t *ui_vitals_create(lv_obj_t *parent);
void ui_vitals_update(lv_obj_t *bar);          /* battery, bolt, Wi-Fi, Tailscale and the clock */
lv_obj_t *ui_vitals_link_dot(lv_obj_t *bar);   /* the dashboard sets the link dot's colour ... */
lv_obj_t *ui_vitals_link_host(lv_obj_t *bar);  /* ... and the host name */

#ifdef __cplusplus
}
#endif
