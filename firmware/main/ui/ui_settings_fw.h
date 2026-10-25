/* Plan 6 (spec §8): Settings › About › Update firmware and Previous version, Settings › Host › Firmware file on Host,
   the Install and Go back confirms, and the boot-note toasts on Sessions. */
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_settings_fw_host_row(lv_obj_t *panel);   /* Firmware file on Host (a value row with a prompt) */
/* Update firmware, Previous version (only when the other slot is runnable) and the footnote. Runs the background
   check (P6-16) unless one is running or the last is under FW_CHECK_TTL_MS old. */
void ui_settings_fw_about(lv_obj_t *panel);
/* The Sessions worker's job bodies (P6-16: never on the LVGL task). */
void ui_fw_job_check(void);
void ui_fw_job_start(void);
/* Sessions' 300 ms tick: a boot note (Updated, rolled back, boot loop, rolling back) toasts once, 6 s. */
void ui_fw_note_tick(void);
#ifdef __cplusplus
}
#endif
