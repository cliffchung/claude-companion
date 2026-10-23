#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *name; uint16_t seconds; bool (*enter)(void); void (*leave)(void); } bench_step_t;
/* enter() returns false when the feature isn't built yet: the step is reported "skipped: not built". */

void power_bench_start(bool fast);   /* fast: on USB, 3 s per step, no battery wait (scaffold check) */
void power_bench_abort(void);
void power_bench_report(void);       /* @@BENCH <n>, then "name,seconds,avg_mw,min_mw,max_mw,avg_ma,mwh_x1000,status", then @@END */
void power_bench_status(void);
uint8_t power_bench_tag(void);       /* log tag for the running step (0x10+n), else 0 */

#ifdef __cplusplus
}
#endif
