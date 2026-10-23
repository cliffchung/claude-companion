#include "plog.h"

void plog_init(plog_ring_t *r, plog_sample_t *buf, uint32_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->head = 0;
    r->count = 0;
}

void plog_push(plog_ring_t *r, const plog_sample_t *s)
{
    if (!r->cap) return;
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) r->count++;
}

bool plog_get(const plog_ring_t *r, uint32_t i, plog_sample_t *out)
{
    if (i >= r->count) return false;
    /* head is the next write slot; when full it is also the oldest */
    uint32_t start = (r->head + r->cap - r->count) % r->cap;
    *out = r->buf[(start + i) % r->cap];
    return true;
}

void plog_sum_add(plog_sum_t *s, const plog_sample_t *x)
{
    if (s->n == 0) {
        s->min_mw = s->max_mw = x->mw;
        s->first_t = x->t_s;
    } else {
        if (x->mw < s->min_mw) s->min_mw = x->mw;
        if (x->mw > s->max_mw) s->max_mw = x->mw;
    }
    s->n++;
    s->sum_mw += x->mw;
    s->sum_ma += x->ma;
    s->last_t = x->t_s;
}

uint32_t plog_sum_avg_mw(const plog_sum_t *s)
{
    return s->n ? (uint32_t)(s->sum_mw / (int64_t)s->n) : 0;
}

uint32_t plog_sum_mwh_x1000(const plog_sum_t *s)
{
    if (!s->n) return 0;
    return (uint32_t)((uint64_t)plog_sum_avg_mw(s) * s->n * 10 / 36);
}

uint16_t plog_mw(uint16_t mv, int16_t ma)
{
    uint32_t a = ma < 0 ? (uint32_t)(-(int32_t)ma) : (uint32_t)ma;
    uint32_t mw = ((uint32_t)mv * a + 500) / 1000;
    return mw > 0xFFFF ? 0xFFFF : (uint16_t)mw;
}

plog_step_verdict_t plog_step_verdict(const plog_sum_t *s, bool usb_seen)
{
    if (usb_seen) return PLOG_STEP_USB;
    return s->n ? PLOG_STEP_OK : PLOG_STEP_NO_SAMPLES;
}
