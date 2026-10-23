#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Home screen: the Claude Code sessions on the companion Mac. Sessions that need you are
 * amber; the selected card shows recent output and takes a quick answer, and Enter or
 * Ctrl+O opens a background session full screen. */
lv_obj_t *ui_dashboard_create(void *arg);

#ifdef __cplusplus
}
#endif
