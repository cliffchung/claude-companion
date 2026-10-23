#pragma once
#include "esp_err.h"
#include "esp_console.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Start an esp_console REPL on USB-Serial-JTAG (prompt "tab5> ") and register built-in commands. */
void debug_console_start(void);
/* Register an additional command; may be called before or after debug_console_start(). */
esp_err_t debug_console_register(const esp_console_cmd_t *cmd);
#ifdef __cplusplus
}
#endif
