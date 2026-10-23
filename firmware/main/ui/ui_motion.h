#pragma once
#include "lvgl.h"

/* Screen-owned draw hooks; no offscreen framebuffer or input-blocking overlay. */
void ui_motion_attach(lv_obj_t *screen);
void ui_motion_load(lv_obj_t *screen, lv_screen_load_anim_t requested);
void ui_motion_start(lv_obj_t *screen, lv_obj_t *region, bool reverse);
/* Drops the theme's scan lines and phosphor sweep from a screen that keeps its own look
 * (the sessions dashboard uses the spec's colours whatever the theme). */
void ui_motion_plain(lv_obj_t *screen);
