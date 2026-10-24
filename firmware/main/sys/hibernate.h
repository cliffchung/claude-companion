#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sys/power_rules.h"
#include "sys/hib_rules.h"   /* power_boot_t, the plans */
#ifdef __cplusplus
extern "C" {
#endif
/* Hibernate (Plan 2 Task 6): the whole device down until a wake source boots it again. Every wake is
 * a cold boot. The board facts behind it (schematic, factory firmware, probes) are in
 * docs/superpowers/notes/2026-10-23-light-sleep-spike.md. Motion wake is out of scope (PT15/PT16).
 * PT26: every product Hibernate is an ESP deep sleep with a timer (a check-in, or the 2 h power probe);
 * CHG_EN stays driven high, so a sleeping Tab5 charges. The power-off is `power hib test off` only. */

typedef enum { HIB_SRC_TIMER = 1, HIB_SRC_BUTTON = 2, HIB_SRC_RESET = 4, HIB_SRC_RTC = 8 } hib_src_t;
typedef enum { HIB_MODE_NONE, HIB_MODE_POWEROFF, HIB_MODE_DEEPSLEEP } hib_mode_t;

bool hibernate_supported(void);         /* mode != NONE and a wake source other than the timer works */
hib_mode_t hibernate_mode(void);
uint32_t hibernate_wake_sources(void);  /* hib_src_t bits proved on this board (compile-time, from the notes) */
/* Deep sleep after wake_after_s (hib_sleep_plan: 0 becomes the power probe, so never without a timer).
   Saves the power-log sums, writes NVS ns "power" key "hib", turns Wi-Fi off, suspends the BMI270, cuts
   every rail in hibernate_rails() marked cut, puts the INA226 into power-down (unless a bench probe is
   running), then deep-sleeps. Returns only if that failed. The caller has already set the light. */
void hibernate_now(uint32_t wake_after_s, power_boot_t next);
/* The same, deep sleep with only the timer, whatever the mode: the bench's `floor` step. */
void hibernate_deep_sleep(uint32_t wake_after_s, power_boot_t next);
power_boot_t hibernate_boot_kind(void); /* from "hib" and the wakeup cause (or the RX8130 TF flag after power-off) */
uint32_t hibernate_wake_src(void);      /* the hib_src_t bit that woke this boot, 0 on a normal boot */
const char *hibernate_cause_text(void); /* "timer", "rtc", "button", "reset", "-" */
/* PT16, from Step 4c: the keyboard stays powered through deep sleep and its light holds (Task 8's
   check-in can sleep again with the amber light on) */
bool hibernate_kbd_rail_survives(void);

typedef struct { const char *name; uint8_t addr; uint8_t pin; bool cut; } hib_rail_t;
const hib_rail_t *hibernate_rails(int *n);   /* every expander output the schematic names */
/* CHG_EN (0x44 P7) driven high (PT22): 1, else 0; -1 unreadable */
int hibernate_chg_en(void);

/* Before the rails come up (right after bsp_i2c_init): reads and clears "hib", the RX8130 flags and the
   wakeup cause; a bench probe boot samples the INA226 and goes back to sleep from here, and so does a power
   probe (PT26) that reads no USB. Calls nvs_flash_init() itself. */
void hibernate_boot_early(void);
uint32_t hibernate_boot_to_live_ms(void);    /* the last hibernate wake: boot to the first rc-0 result */
void hibernate_init(void);                   /* after cc_link_start: starts the boot-to-live clock */

uint32_t hibernate_test_wake_s(void);        /* `power hib test-wake`, 0 when not armed */

/* ---- Task 8: the Hibernate check-in (PT16) ---- */
uint32_t hibernate_checkin_s(void);          /* pr_hib_checkin_clamp_min(pw_hibernate_checkin_min) * 60; 0 = Off */
/* The ladder's (and `power go hibernate`'s, and a check-in boot's) hibernate, by hib_ladder_plan (PT26):
   always a deep sleep. A check-in after hibernate_checkin_s() backed off by checkin_fails (I3); the power
   probe (light off, no network on its wakes) when the check-in is Off or low_battery (the <= 10% trigger);
   `power hib test-wake`'s timer when armed. checkin_fails is the count the next check-in boot gets. */
void hibernate_ladder(bool low_battery, int checkin_fails);
int hibernate_boot_checkin_fails(void);      /* I3: consecutive failed check-ins before this boot (hib record) */
/* `power hib probe <s> <n>` (RTC memory, test only): the power probe every s seconds instead of 2 h, and
   the next n probe wakes sleep again whatever they read (so the sleep-again path runs on USB). s = 0 ends it. */
void hibernate_probe_test_set(uint32_t s, uint32_t n);
/* Debug flags for the next check-in boot, kept in the "hib" record (RAM until then) */
#define HIB_DBG_STAY_AWAKE   1   /* a re-hibernate result stays awake, so the console comes back */
#define HIB_DBG_FORCE_KEEP   2   /* the keyboard-rail-survives branch, whatever Task 6 found */
#define HIB_DBG_FORCE_NOKEEP 4   /* the stay-in-Deep-idle branch */
#define HIB_DBG_AS_BATTERY   8   /* Plan 3 Task 2 timing: on USB, check in as on battery (no full wake) */
void hibernate_set_debug_next(uint8_t flags);
uint8_t hibernate_boot_debug(void);          /* this boot's flags */
/* I3: the keyboard light last written before this boot's sleep (kept in the "hib" record), so a check-in
   boot can hold it until its first result instead of switching it off. False on a normal boot. */
bool hibernate_boot_light(pr_led_t *out);
/* `power hib backstop <s> <n>`: an extra timer wake after s seconds on each of the next n hibernates
   (RTC memory: it survives deep sleep and the watchdog reset on the way up, not a power-off, which
   it prevents by making the hibernate timed). n = 0 ends it. */
void hibernate_backstop_set(uint32_t s, uint32_t n);

/* Console (`power hib ...`, `power boot`, `power rails`, `power rail`, `power imu`). */
int hibernate_console(int argc, char **argv);

#ifdef __cplusplus
}
#endif
