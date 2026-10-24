#include "hib_rules.h"
#include "vitals.h"

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
    /* PT27: before the armed timer could have fired, only the side button ends a deep sleep (proved on the
       board: a press 76 s into a 900 s timer, hw reason 7); a crash on the way down never counts (M4) */
    if (in->was_deepsleep && in->reset_deepsleep_or_wdt && !in->reset_crash && in->asleep_s >= 0) {
        long lo = (long)in->wake_after_s - 2;
        if (!in->wake_after_s || in->asleep_s < lo) return HIB_CAUSE_BUTTON;
    }
    return HIB_CAUSE_RESET;
}

hib_probe_verdict_t hib_probe_verdict(uint32_t n, bool fast, int64_t sum_ma)
{
    if (n == 0) return HIB_PROBE_NO_SAMPLES;
    if (fast || sum_ma < 0) return HIB_PROBE_USB;
    return HIB_PROBE_OK;
}


power_boot_t hib_boot_kind(power_boot_t next, hib_cause_t cause)
{
    if (cause == HIB_CAUSE_RESET) return POWER_BOOT_NORMAL;
    if (cause == HIB_CAUSE_BUTTON) return POWER_BOOT_HIB_WAKE;
    return next;
}

hib_plan_t hib_sleep_plan(uint32_t wake_after_s, power_boot_t next, uint32_t test_wake_s, uint32_t backstop_s)
{
    uint32_t t = wake_after_s ? wake_after_s : test_wake_s;
    if (backstop_s && (!t || backstop_s < t)) t = backstop_s;
    if (!t) return (hib_plan_t){ HIB_POWER_PROBE_S, POWER_BOOT_HIB_POWER_PROBE };
    return (hib_plan_t){ t, next };
}

uint32_t hib_checkin_backoff_s(uint32_t period_s, int fails)
{
    if (period_s >= HIB_CHECKIN_BACKOFF_CAP_S || fails <= 0) return period_s;
    int k = fails > 3 ? 3 : fails;
    uint32_t t = period_s << k;
    return t > HIB_CHECKIN_BACKOFF_CAP_S ? HIB_CHECKIN_BACKOFF_CAP_S : t;
}

int hib_checkin_fails_next(int fails, bool poll_ok)
{
    if (poll_ok) return 0;
    if (fails < 0) fails = 0;
    return fails >= 255 ? 255 : fails + 1;
}

hib_plan_t hib_ladder_plan(bool low_battery, uint32_t checkin_s, int fails, uint32_t test_wake_s, uint32_t probe_s)
{
    if (test_wake_s) return (hib_plan_t){ test_wake_s, POWER_BOOT_HIB_WAKE };
    if (low_battery || !checkin_s) return (hib_plan_t){ probe_s ? probe_s : HIB_POWER_PROBE_S, POWER_BOOT_HIB_POWER_PROBE };
    return (hib_plan_t){ hib_checkin_backoff_s(checkin_s, fails), POWER_BOOT_HIB_CHECKIN };
}

hib_pp_t hib_power_probe(bool have, int mv, int ma)
{
    if (!have) return HIB_PP_SLEEP;
    if (mv < 5500) return HIB_PP_BOOT;   /* no pack: the board runs from USB */
    power_src_t s = vitals_power_src(mv, ma, PWR_UNKNOWN);
    return s == PWR_USB_CHARGING || s == PWR_USB_STANDBY ? HIB_PP_BOOT : HIB_PP_SLEEP;
}
