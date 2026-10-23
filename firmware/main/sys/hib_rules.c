#include "hib_rules.h"

#define RTC_AF 0x08
#define RTC_TF 0x10

hib_cause_t hib_cause(const hib_wake_in_t *in)
{
    if (in->rtc_flags & (RTC_AF | RTC_TF)) return HIB_CAUSE_RTC;
    if (in->wakeup_timer) return HIB_CAUSE_TIMER;
    bool timed = in->wake_after_s && in->asleep_s >= (long)in->wake_after_s - 2 && in->asleep_s <= (long)in->wake_after_s + 20;
    if (timed && in->reset_deepsleep_or_wdt) return HIB_CAUSE_TIMER;
    if (in->reset_poweron) return HIB_CAUSE_BUTTON;
    return HIB_CAUSE_RESET;
}

hib_probe_verdict_t hib_probe_verdict(uint32_t n, bool fast, int64_t sum_ma)
{
    if (n == 0) return HIB_PROBE_NO_SAMPLES;
    if (fast || sum_ma < 0) return HIB_PROBE_USB;
    return HIB_PROBE_OK;
}
