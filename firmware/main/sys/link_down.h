#pragma once
/* Final review C1: the device side of the close-before-down guard (sys/link_guard.h). */
#ifdef __cplusplus
extern "C" {
#endif
/* Sets the guard's clock, registers the SSH Terminal closer and the Tailscale stop hook. Once at boot,
   before Wi-Fi, Tailscale or any SSH client starts (cc_link_start registers the link's closer). */
void link_down_init(void);
/* Every esp_restart of the app goes through here: closes every SSH session (bounded, about 2 s) while the
   Wi-Fi still carries the close, then restarts. why is logged. Never returns. Blocks: fine from the LVGL
   task and the console. */
void link_down_restart(const char *why) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
