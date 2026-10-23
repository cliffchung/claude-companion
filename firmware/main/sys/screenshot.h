#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void screenshot_init(void);                /* three-finger tap detector (LVGL task) */
bool screenshot_take(char *out_path, int cap);
/* Print a JPEG frame on stdout: "@@SHOT jpeg <len> <w> <h>", base64 lines of 76 chars, "@@END". */
bool screenshot_dump_usb(void);
#ifdef __cplusplus
}
#endif
