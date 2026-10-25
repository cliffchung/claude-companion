/* Plan 6 (spec §8.4): the full-screen progress modal of a firmware update. */
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Builds it on lv_layer_top (once per update; nothing if it is up). A 300 ms timer refreshes it from fwu_status;
   when the update ends (IDLE) it closes and toasts the outcome for 6 s. Esc and Cancel cancel; every other key
   and tap is eaten. Asynchronous-safe as an lv_async_cb_t (arg unused). */
void ui_fwupdate_show(void *arg);
bool ui_fwupdate_shown(void);
lv_timer_t *ui_fwupdate_timer(void);   /* its refresh timer (NULL when closed): the preview's timer check */
#ifdef __cplusplus
}
#endif
