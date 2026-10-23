#pragma once
/* Pure power-log ring and per-tag summaries (no ESP-IDF headers; also built by test/host). */
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t t_s; uint16_t mv; int16_t ma; uint16_t mw; uint8_t tag; uint8_t flags; } plog_sample_t;
#define PLOG_F_PIN_LOW 1     /* charge pin read low */
#define PLOG_F_PIN_OK 2      /* charge pin read OK */
#define PLOG_F_CHARGING 4    /* current flowing in (ma < 0) or pin low: not a battery sample */
/* tag: power state 0..3 (Task 5), 0x10+n for bench step n, 0x80|state for a Radio-off check-in (0x83);
 * flags bit0 = charge pin low, bit1 = pin read OK */
typedef struct { plog_sample_t *buf; uint32_t cap, head, count; } plog_ring_t;

void plog_init(plog_ring_t *r, plog_sample_t *buf, uint32_t cap);
void plog_push(plog_ring_t *r, const plog_sample_t *s);           /* overwrites the oldest when full */
bool plog_get(const plog_ring_t *r, uint32_t i, plog_sample_t *out); /* i = 0 is the oldest */

typedef struct { uint32_t n; int64_t sum_mw, sum_ma; uint16_t min_mw, max_mw; uint32_t first_t, last_t; } plog_sum_t;
void plog_sum_add(plog_sum_t *s, const plog_sample_t *x);
uint32_t plog_sum_avg_mw(const plog_sum_t *s);                    /* 0 when empty */
uint32_t plog_sum_mwh_x1000(const plog_sum_t *s);                 /* avg mW x n / 3.6: the log is 1 Hz, so n is the seconds covered (a tag that
                                                                   * interleaves with others would be overstated by last_t - first_t + 1) */

/* mW from the volts and amps, rounded: the INA226 power register is 25 mW coarse and unsigned. */
uint16_t plog_mw(uint16_t mv, int16_t ma);

typedef enum { PLOG_STEP_OK = 0, PLOG_STEP_USB, PLOG_STEP_NO_SAMPLES } plog_step_verdict_t;
/* A bench step is ok only with battery samples and no USB seen; USB wins over an empty sum. */
plog_step_verdict_t plog_step_verdict(const plog_sum_t *s, bool usb_seen);

#ifdef __cplusplus
}
#endif
