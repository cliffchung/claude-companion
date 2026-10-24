#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "plog.h"
#ifdef __cplusplus
extern "C" {
#endif

/* On-device power log: a 65 536-sample PSRAM ring (about 18 h at 1 Hz), per-tag summaries kept in
 * NVS (ns "power", key "sum") and the reset-reason counters (key "rst"). */
void power_log_boot(void);                 /* logs "reset reason: <name>", counts it in NVS; call right after settings_init() */
void power_log_sample(uint8_t tag);        /* called each second by the idle task while logging is on */
bool power_log_on(void);
void power_log_accept_charging(bool on);   /* default off: charging samples stay out of the summaries */
void power_log_enable(bool on);
void power_log_dump(void);                 /* @@PLOG <n>, "t_s,mv,ma,mw,tag,flags" lines, @@END */
void power_log_clear(void);
void power_log_save(void);                 /* the per-tag sums to NVS now (before Hibernate) */
void power_log_status(void);               /* sample count and per-tag summaries */
void power_log_resets(void);               /* table of reset reasons and counts */
void power_log_resets_clear(void);
/* The bench reads and resets the summary of its own step tag. */
bool power_log_sum_get(uint8_t tag, plog_sum_t *out);
void power_log_sum_reset(uint8_t tag);

#ifdef __cplusplus
}
#endif
