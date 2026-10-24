#include "ssh/cb_gate.h"

bool cb_gate_enter(cb_gate_t *g, const cb_gate_ops_t *ops)
{
    ops->lock(ops->ctx);
    bool ok = !g->detached;
    if (ok) g->busy++;
    ops->unlock(ops->ctx);
    return ok;
}

void cb_gate_exit(cb_gate_t *g, const cb_gate_ops_t *ops)
{
    ops->lock(ops->ctx);
    g->busy--;
    ops->unlock(ops->ctx);
}

bool cb_gate_detach_wait(cb_gate_t *g, const cb_gate_ops_t *ops, int bound_ms)
{
    ops->lock(ops->ctx);
    g->detached = true;
    bool idle = g->busy == 0;
    ops->unlock(ops->ctx);
    for (int waited = 0; !idle && waited < bound_ms; waited++) {
        ops->nap_ms(ops->ctx, 1);
        ops->lock(ops->ctx);
        idle = g->busy == 0;
        ops->unlock(ops->ctx);
    }
    return idle;
}
