#pragma once
/* The preview's automated checks (spec §12). Every screen case ends in pv_run_checks. */
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>

/* Pixels within CIE76 dE <= 10 of the NEEDS YOU amber outside the coords of every UI_NEEDS_YOU_PART
 * object under scr (and lv_layer_top). snap is an RGB888 snapshot of scr. */
int pv_amber_outside(lv_obj_t *scr, const lv_draw_buf_t *snap);
/* Every clickable control: in the body >= 64 visible and >= 80 hit; a UI_BAR_CONTROL >= 48 and >= 80;
 * neighbours >= 8 apart. 0 means OK; otherwise why says which. */
int pv_touch_targets(lv_obj_t *root, char *why, size_t cap);
/* No label extends past its parent, and an LV_LABEL_LONG_MODE_WRAP label is never clipped. 0 means OK. */
int pv_fit(lv_obj_t *root, char *why, size_t cap);
/* The pixels flushed over ms after the first draw (0: the screen is idle). */
unsigned pv_idle_pixels(lv_display_t *d, unsigned ms);
/* No timer under 300 ms except the pty pump while a terminal is open, and the 300 ms tick only
 * when sessions_shown. 0 means OK. */
int pv_timers(bool sessions_shown, char *why, size_t cap);

#define PV_SKIP_AMBER   1u
#define PV_SKIP_TARGETS 2u
/* Runs all five checks except the skipped ones, prints "check <name>: ok" (or the failures) and
 * writes <name>.ppm. Returns 0 when every check passed. The skips are only for the screens not
 * rebuilt yet (Task 4: dashboard and settings; Task 6 and Task 7 drop them). */
int pv_run_checks(lv_display_t *d, lv_obj_t *scr, const char *name, bool needs_you_expected, unsigned skip);
