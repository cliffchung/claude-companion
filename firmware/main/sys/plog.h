#pragma once
/* Pure power-log ring and per-tag summaries (no ESP-IDF headers; also built by test/host). */
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t t_s; uint16_t mv; int16_t ma; uint16_t mw; uint8_t tag; uint8_t flags; } plog_sample_t;
/* tag: power state 0..3 (Task 5), 0x10+n for bench step n, 0x80|state for a Radio-off check-in (0x83);
 * flags bit0 = charge pin low, bit1 = pin read OK */
typedef struct { plog_sample_t *buf; uint32_t cap, head, count; } plog_ring_t;

void plog_init(plog_ring_t *r, plog_sample_t *buf, uint32_t cap);
void plog_push(plog_ring_t *r, const plog_sample_t *s);           /* overwrites the oldest when full */
bool plog_get(const plog_ring_t *r, uint32_t i, plog_sample_t *out); /* i = 0 is the oldest */

typedef struct { uint32_t n; int64_t sum_mw, sum_ma; uint16_t min_mw, max_mw; uint32_t first_t, last_t; } plog_sum_t;
void plog_sum_add(plog_sum_t *s, const plog_sample_t *x);
uint32_t plog_sum_avg_mw(const plog_sum_t *s);                    /* 0 when empty */
uint32_t plog_sum_mwh_x1000(const plog_sum_t *s);                 /* avg mW x seconds covered / 3.6; seconds covered = last_t - first_t + 1 */

#ifdef __cplusplus
}
#endif
