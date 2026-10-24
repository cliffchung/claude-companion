#pragma once
#include <stdbool.h>
/* I1 (final review): the SSH client's callbacks (data, state, host key) run on ssh_task while their owner (the
   Terminal screen, on the LVGL task) may stop the client and free the context the callbacks write into. Each
   callback runs inside cb_gate_enter/cb_gate_exit; the owner's stop detaches the gate and waits (bounded) until no
   callback is in flight. After a true return no callback runs again, so the owner may free its context. Pure (no
   ESP-IDF): the lock and the nap are passed in (portMUX and vTaskDelay on the device, host-tested with pthreads). */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile bool detached;   /* the owner left: no callback enters again (GONE reads it unlocked) */
    volatile int busy;        /* callbacks entered and not yet left */
} cb_gate_t;

typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*nap_ms)(void *ctx, int ms);   /* sleeps about ms, outside the lock */
    void *ctx;
} cb_gate_ops_t;

/* True: counted in, call the callback, then cb_gate_exit. False: detached, don't call it. */
bool cb_gate_enter(cb_gate_t *g, const cb_gate_ops_t *ops);
void cb_gate_exit(cb_gate_t *g, const cb_gate_ops_t *ops);
/* Detaches, then waits up to bound_ms for the callbacks in flight to leave. True once none is in flight (the owner
   may free what they use); false when the bound ran out (one is still running: the owner must not free it). */
bool cb_gate_detach_wait(cb_gate_t *g, const cb_gate_ops_t *ops, int bound_ms);

#ifdef __cplusplus
}
#endif
