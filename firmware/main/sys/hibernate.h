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

/* ---- Task 8: the Hibernate check-in (PT16) ---- */
uint32_t hibernate_checkin_s(void);          /* pr_hib_checkin_clamp_min(pw_hibernate_checkin_min) * 60; 0 = Off */
/* The ladder's (and `power go hibernate`'s) hibernate: a check-in wake after hibernate_checkin_s()
   (deep sleep, next = POWER_BOOT_HIB_CHECKIN), or no timed wake (the power-off) when the check-in is Off,
   when low_battery (the battery <= 10% trigger: a nearly flat pack isn't drained further) or when
   `power hib test-wake` is armed (its own timer, next = HIB_WAKE, as in Task 6). */
void hibernate_ladder(bool low_battery);
/* Debug flags for the next check-in boot, kept in the "hib" record (RAM until then) */
#define HIB_DBG_STAY_AWAKE   1   /* a re-hibernate result stays awake, so the console comes back */
#define HIB_DBG_FORCE_KEEP   2   /* the keyboard-rail-survives branch, whatever Task 6 found */
#define HIB_DBG_FORCE_NOKEEP 4   /* the stay-in-Deep-idle branch */
void hibernate_set_debug_next(uint8_t flags);
uint8_t hibernate_boot_debug(void);          /* this boot's flags */
/* `power hib backstop <s> <n>`: an extra timer wake after s seconds on each of the next n hibernates
   (RTC memory: it survives deep sleep and the watchdog reset on the way up, not a power-off, which
   it prevents by making the hibernate timed). n = 0 ends it. */
void hibernate_backstop_set(uint32_t s, uint32_t n);

/* Console (`power hib ...`, `power boot`, `power rails`, `power rail`, `power imu`). */
int hibernate_console(int argc, char **argv);

#ifdef __cplusplus
}
#endif
