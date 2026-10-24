#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void usb_mgr_init(void);                 /* starts USB host for the HID keyboard */
const char *usb_mgr_status_text(void);   /* HID only; goes with ui_devinfo.c in Plan 4 Task 3 (C1) */
#ifdef __cplusplus
}
#endif
