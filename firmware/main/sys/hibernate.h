#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Hibernate (Plan 2 Task 6): the whole device down until a wake source boots it again. Every wake is
 * a cold boot. The board facts behind it (schematic, factory firmware, probes) are in
 * docs/superpowers/notes/2026-10-23-light-sleep-spike.md. Motion wake is out of scope (PT15/PT16). */

typedef enum { HIB_SRC_TIMER = 1, HIB_SRC_BUTTON = 2, HIB_SRC_RESET = 4, HIB_SRC_RTC = 8 } hib_src_t;
typedef enum { HIB_MODE_NONE, HIB_MODE_POWEROFF, HIB_MODE_DEEPSLEEP } hib_mode_t;
typedef enum { POWER_BOOT_NORMAL, POWER_BOOT_HIB_WAKE, POWER_BOOT_HIB_CHECKIN, POWER_BOOT_HIB_PROBE } power_boot_t;

bool hibernate_supported(void);         /* mode != NONE and a wake source other than the timer works */
hib_mode_t hibernate_mode(void);
uint32_t hibernate_wake_sources(void);  /* hib_src_t bits proved on this board (compile-time, from the notes) */
/* 0 = no timed wake. Saves the power-log sums, writes NVS ns "power" key "hib", turns Wi-Fi off, suspends
   the BMI270, cuts every rail in hibernate_rails() marked cut, puts the INA226 into power-down (unless a
   bench probe is running), then powers off or deep-sleeps. Returns only if that failed. The caller has
   already set the light by the Deep-idle rule. */
void hibernate_now(uint32_t wake_after_s, power_boot_t next);
/* The same, deep sleep with only the timer, whatever the mode: the bench's `floor` step. */
void hibernate_deep_sleep(uint32_t wake_after_s, power_boot_t next);
power_boot_t hibernate_boot_kind(void); /* from "hib" and the wakeup cause (or the RX8130 TF flag after power-off) */
uint32_t hibernate_wake_src(void);      /* the hib_src_t bit that woke this boot, 0 on a normal boot */
const char *hibernate_cause_text(void); /* "timer", "rtc", "button-or-usb", "reset", "-" */
/* PT16, from Step 4c: the keyboard stays powered through deep sleep and its light holds (Task 8's
   check-in can sleep again with the amber light on) */
bool hibernate_kbd_rail_survives(void);

typedef struct { const char *name; uint8_t addr; uint8_t pin; bool cut; } hib_rail_t;
const hib_rail_t *hibernate_rails(int *n);   /* every expander output the schematic names */

/* Before the rails come up (right after bsp_i2c_init): reads and clears "hib", the RX8130 flags and the
   wakeup cause; a bench probe boot samples the INA226 and goes back to sleep from here. Calls
   nvs_flash_init() itself. */
void hibernate_boot_early(void);
uint32_t hibernate_boot_to_live_ms(void);    /* the last hibernate wake: boot to the first rc-0 result */
void hibernate_init(void);                   /* after cc_link_start: starts the boot-to-live clock */

uint32_t hibernate_test_wake_s(void);        /* `power hib test-wake`, 0 when not armed */

/* Console (`power hib ...`, `power boot`, `power rails`, `power rail`, `power imu`). */
int hibernate_console(int argc, char **argv);

#ifdef __cplusplus
}
#endif
