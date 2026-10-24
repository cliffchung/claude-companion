#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Print a JPEG frame on stdout: "@@SHOT jpeg <len> <w> <h>", base64 lines of 76 chars, "@@END". */
bool screenshot_dump_usb(void);
#ifdef __cplusplus
}
#endif
