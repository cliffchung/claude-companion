#pragma once
/* Pure Hibernate rules (no ESP-IDF headers; also built by test/host). */
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The next boot's kind, kept in the hib record (hibernate.h). POWER_PROBE (PT26): a deep sleep with no check-in
   (battery <= 10% or the check-in Off) wakes every HIB_POWER_PROBE_S to read the power source only. */
typedef enum { POWER_BOOT_NORMAL, POWER_BOOT_HIB_WAKE, POWER_BOOT_HIB_CHECKIN, POWER_BOOT_HIB_PROBE,
               POWER_BOOT_HIB_POWER_PROBE } power_boot_t;
#define HIB_POWER_PROBE_S 7200

typedef enum { HIB_CAUSE_NONE, HIB_CAUSE_TIMER, HIB_CAUSE_RTC, HIB_CAUSE_BUTTON, HIB_CAUSE_RESET } hib_cause_t;
typedef struct {
    bool wakeup_timer;             /* esp_sleep_get_wakeup_cause() == TIMER */
    uint8_t rtc_flags;             /* RX8130 flag register: AF bit3, TF bit4 */
    bool reset_poweron;            /* ESP_RST_POWERON: the board was off */
    bool was_poweroff;             /* the hib record says the power-off took (not a deep-sleep fallback) */
    bool reset_deepsleep_or_wdt;   /* ESP_RST_DEEPSLEEP, or ESP_RST_WDT (this P4's timer wake, and its button wake) */
    bool was_deepsleep;            /* the hib record is a deep sleep (asked for, or a power-off that fell back) */
    bool reset_crash;              /* ESP_RST_PANIC, INT_WDT, TASK_WDT or BROWNOUT: never a wake (M4) */
    uint32_t wake_after_s;         /* the timer that was armed, 0 = none */
    long asleep_s;                 /* now - the epoch saved going down (negative when the clock was lost) */
} hib_wake_in_t;
/* "Timed" = the time asleep is within -2..+20 s of the armed timer (never under 3 s). An RTC flag and timed ->
   RTC (a flag alone may be stale); the wakeup cause, or a watchdog/deep-sleep reset and timed -> TIMER;
   power-on -> BUTTON (or a USB plug-in); PT27: after a deep-sleep record, a reset that is not a crash and comes
   before the armed timer (0 <= asleep < its window, or no timer) -> BUTTON (the side button wakes deep sleep
   through the same HP watchdog reset as the timer); otherwise RESET. */
hib_cause_t hib_cause(const hib_wake_in_t *in);
/* M9: RESET -> NORMAL; BUTTON -> HIB_WAKE (a press never boots dark, whatever the record said); else next */
power_boot_t hib_boot_kind(power_boot_t next, hib_cause_t cause);

typedef struct { uint32_t timer_s; power_boot_t next; } hib_plan_t;
/* M4, every deep sleep has a timer: wake_after_s, else test_wake_s; a backstop_s (0 = none) only shortens it;
   still 0 -> HIB_POWER_PROBE_S with next = POWER_PROBE. timer_s is never 0. */
hib_plan_t hib_sleep_plan(uint32_t wake_after_s, power_boot_t next, uint32_t test_wake_s, uint32_t backstop_s);
/* PT26, the ladder's Hibernate (never a power-off): test_wake_s -> {it, HIB_WAKE}; low_battery or no check-in
   (checkin_s 0) -> {probe_s or HIB_POWER_PROBE_S, POWER_PROBE}; else {hib_checkin_backoff_s(checkin_s, fails),
   CHECKIN}. */
hib_plan_t hib_ladder_plan(bool low_battery, uint32_t checkin_s, int fails, uint32_t test_wake_s, uint32_t probe_s);
/* I3 (PT28): period * 2^min(fails, 3), capped at 2 h; a period over 2 h is never shortened; 0 stays 0. */
#define HIB_CHECKIN_BACKOFF_CAP_S 7200
uint32_t hib_checkin_backoff_s(uint32_t period_s, int fails);
/* The failure count the next check-in boot gets: 0 after a live poll, else fails + 1 (saturates at 255). */
int hib_checkin_fails_next(int fails, bool poll_ok);

/* PT26: a power probe wake's one INA226 reading (mv bus, ma + = discharging). No reading -> SLEEP (the button
   still wakes it); ma < VITALS_CHG_MA, mv >= 8250 (USB standby) or mv < 5500 (no pack) -> BOOT; else SLEEP. */
typedef enum { HIB_PP_SLEEP, HIB_PP_BOOT } hib_pp_t;
hib_pp_t hib_power_probe(bool have, int mv, int ma);

typedef enum { HIB_PROBE_OK, HIB_PROBE_USB, HIB_PROBE_NO_SAMPLES } hib_probe_verdict_t;
/* No samples wins; then a fast (USB) run or charging current is "usb". */
hib_probe_verdict_t hib_probe_verdict(uint32_t n, bool fast, int64_t sum_ma);

#ifdef __cplusplus
}
#endif
