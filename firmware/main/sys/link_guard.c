#include "link_guard.h"
#include <stddef.h>

/* Plan 3 final review C1. Registered at init; read by any task afterwards (an entry is written before the
   count that publishes it, and never changes). */
static lg_closer_t s_c[LG_MAX];
static volatile int s_n;
static const lg_clock_t *s_clk;

#define LG_POLL_MS 10

void lg_set_clock(const lg_clock_t *clk) { s_clk = clk; }

bool lg_register(const lg_closer_t *c)
{
    if (!c || !c->close || !c->idle || s_n >= LG_MAX) return false;
    s_c[s_n] = *c;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    s_n = s_n + 1;
    return true;
}

void lg_reset(void) { s_n = 0; s_clk = NULL; }

static bool all_idle(int n, bool tailnet_only)
{
    for (int i = 0; i < n; i++)
        if (!s_c[i].idle(tailnet_only, s_c[i].ctx)) return false;
    return true;
}

lg_token_t lg_close(bool tailnet_only, int timeout_ms)
{
    lg_token_t t = { 0 };
    int n = s_n;
    for (int i = 0; i < n; i++)
        if (s_c[i].close(tailnet_only, s_c[i].ctx)) t.took |= (uint8_t)(1u << i);
    if (!s_clk || !s_clk->now_ms || !s_clk->sleep_ms) {   /* no clock yet: ask once, never spin */
        t.idle = false;
        return t;
    }
    int64_t t0 = s_clk->now_ms();
    for (;;) {
        if (all_idle(n, tailnet_only)) { t.idle = true; break; }
        if (s_clk->now_ms() - t0 >= timeout_ms) { t.idle = false; break; }
        s_clk->sleep_ms(LG_POLL_MS);
    }
    t.waited_ms = (int)(s_clk->now_ms() - t0);
    return t;
}

void lg_reopen(lg_token_t t)
{
    for (int i = s_n - 1; i >= 0; i--)
        if ((t.took & (1u << i)) && s_c[i].reopen) s_c[i].reopen(s_c[i].ctx);
}

bool lg_around(bool tailnet_only, int timeout_ms, void (*down)(void *ctx), void *ctx, bool reopen)
{
    lg_token_t t = lg_close(tailnet_only, timeout_ms);
    if (down) down(ctx);
    if (reopen) lg_reopen(t);
    return t.idle;
}
