#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *name; uint16_t seconds; bool (*enter)(void); void (*leave)(void); bool (*done)(void); } bench_step_t;
/* enter() returns false when the feature isn't built yet: the step is reported "skipped: not built".
   done (may be NULL) ends the step early: `seconds` is then its limit (the check-in steps). */

void power_bench_start(bool fast);   /* fast: on USB, 3 s per step, no battery wait (scaffold check) */
void power_bench_abort(void);
void power_bench_report(void);       /* @@BENCH <n>, then "name,seconds,avg_mw,min_mw,max_mw,avg_ma,mwh_x1000,status", then @@END */
void power_bench_status(void);
uint8_t power_bench_tag(void);       /* log tag for the running step (0x10+n), else 0 */

/* Probe steps (`floor`, `hibernate`): each cycle deep-sleeps or powers off, and the next boot samples the
   INA226 in hibernate_boot_early() before any rail comes up. bus_mv from the bus register, shunt_raw in
   2.5 uV (0.5 mA with the 5 mOhm shunt). Returns what that boot does next. */
typedef enum { POWER_BENCH_PROBE_SLEEP, POWER_BENCH_PROBE_DONE } power_bench_probe_t;
power_bench_probe_t power_bench_probe_boot(bool have, uint16_t bus_mv, int16_t shunt_raw, uint32_t *sleep_s, bool *deep);
void power_bench_boot(void);         /* after cc_link_start: finishes a probe run once the link is live */

#ifdef __cplusplus
}
#endif
