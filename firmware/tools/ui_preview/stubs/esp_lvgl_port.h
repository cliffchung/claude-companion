#pragma once
/* pv_lvgl_lock_fails (preview.c): that many next takes fail, as a bounded wait that timed out (I2) */
extern int pv_lvgl_lock_fails;
static inline int lvgl_port_lock(int x) { (void)x; if (pv_lvgl_lock_fails > 0) { pv_lvgl_lock_fails--; return 0; } return 1; }
static inline void lvgl_port_unlock(void) {}
