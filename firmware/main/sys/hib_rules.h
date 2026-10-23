#pragma once
/* Pure Hibernate rules (no ESP-IDF headers; also built by test/host). */
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HIB_CAUSE_NONE, HIB_CAUSE_TIMER, HIB_CAUSE_RTC, HIB_CAUSE_BUTTON, HIB_CAUSE_RESET } hib_cause_t;
typedef struct {
    bool wakeup_timer;             /* esp_sleep_get_wakeup_cause() == TIMER */
    uint8_t rtc_flags;             /* RX8130 flag register: AF bit3, TF bit4 */
    bool reset_poweron;            /* ESP_RST_POWERON: the board was off */
    bool reset_deepsleep_or_wdt;   /* ESP_RST_DEEPSLEEP, or ESP_RST_WDT (this P4's timer wake) */
    uint32_t wake_after_s;         /* the timer that was armed, 0 = none */
    long asleep_s;                 /* now - the epoch saved going down (negative when the clock was lost) */
} hib_wake_in_t;
/* "Timed" = the time asleep is within -2..+20 s of the armed timer (never under 3 s). An RTC flag and timed ->
   RTC (a flag alone may be stale); the wakeup cause, or a watchdog/deep-sleep reset and timed -> TIMER;
   power-on -> BUTTON (or a USB plug-in); otherwise RESET. */
hib_cause_t hib_cause(const hib_wake_in_t *in);

typedef enum { HIB_PROBE_OK, HIB_PROBE_USB, HIB_PROBE_NO_SAMPLES } hib_probe_verdict_t;
/* No samples wins; then a fast (USB) run or charging current is "usb". */
hib_probe_verdict_t hib_probe_verdict(uint32_t n, bool fast, int64_t sum_ma);

#ifdef __cplusplus
}
#endif
