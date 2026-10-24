#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The panel and touch handles the BSP keeps static, read from esp_lvgl_port 2.8.0~1's driver
 * data (idf_component.yml pins that version, so the layouts mirrored in display_ctl.c hold).
 * Call from any task after bsp_display_start; nothing here is cached except the board version. */

esp_lcd_panel_handle_t display_ctl_panel(void);
esp_lcd_touch_handle_t display_ctl_touch(void);
esp_err_t display_ctl_panel_on(bool on);          /* esp_lcd_panel_disp_on_off */
/* The BSP's own probe, repeated once and cached: ST7123 touch FW 3 -> 2, FW 1 -> 3;
   GT911 at its backup address -> 1; else 0. */
int display_ctl_board_version(void);
/* v1 only (GT911): GPIO23 as an input with pull-up and a NEGEDGE ISR. Other boards: the ST7123
   driver owns GPIO23, so no ISR; logs "touch wake: polling on board vN". */
void display_ctl_touch_int_init(void (*isr)(void *));
/* v2 and v3: one read of the touch controller; *pressed = a point is down. Call only while
   LVGL's touch input is paused, so it never races LVGL's read callback. */
esp_err_t display_ctl_touch_poll(bool *pressed);

#ifdef __cplusplus
}
#endif
