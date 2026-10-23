#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Home screen: the Claude Code sessions on the companion Mac. Sessions that need you are
 * amber; the selected card shows recent output and takes a quick answer, and Enter or
 * Ctrl+O opens a background session full screen. */
lv_obj_t *ui_dashboard_create(void *arg);
/* The centre of the selected card on screen (power sim touch); false off the dashboard. */
bool ui_dashboard_selected_center(int32_t *x, int32_t *y);
/* Settings -> Forget Mac host key: runs cc_link_forget_host_key on the dashboard's worker and
   toasts "New Mac key pinned: SHA256:..." or "Old key forgotten; reconnecting...". Call from
   the LVGL task; false if the worker can't take it now. */
bool ui_dashboard_forget_host_key(void);
#define FORGET_TOAST_MS 6000   /* long enough to read the fingerprint */

#ifdef __cplusplus
}
#endif
