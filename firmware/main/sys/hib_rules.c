#include "hib_rules.h"

#define RTC_AF 0x08
#define RTC_TF 0x10

/* the time asleep matches the armed timer: -2..+20 s, and never under 3 s (M1: a crash right after a 1-2 s
   timer was armed is not a timer wake) */
static bool timed(const hib_wake_in_t *in)
{
    if (!in->wake_after_s) return false;
    long lo = (long)in->wake_after_s - 2;
    if (lo < 3) lo = 3;
    return in->asleep_s >= lo && in->asleep_s <= (long)in->wake_after_s + 20;
}

hib_cause_t hib_cause(const hib_wake_in_t *in)
{
    bool t = timed(in);
    /* a flag alone may be stale (a timer that fired without switching the board on) */
    if ((in->rtc_flags & (RTC_AF | RTC_TF)) && t) return HIB_CAUSE_RTC;
    /* a power-off that took is ended only by the power controller: the button (or a USB plug-in), whatever
       reset reason the P4 reports (this one reports an HP watchdog reset) */
    if (in->was_poweroff) return HIB_CAUSE_BUTTON;
    if (in->wakeup_timer) return HIB_CAUSE_TIMER;
    if (t && in->reset_deepsleep_or_wdt) return HIB_CAUSE_TIMER;
    if (in->reset_poweron) return HIB_CAUSE_BUTTON;
    return HIB_CAUSE_RESET;
}

hib_probe_verdict_t hib_probe_verdict(uint32_t n, bool fast, int64_t sum_ma)
{
    if (n == 0) return HIB_PROBE_NO_SAMPLES;
    if (fast || sum_ma < 0) return HIB_PROBE_USB;
    return HIB_PROBE_OK;
}
