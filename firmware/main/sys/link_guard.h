#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Plan 3 final review C1: every path that takes the link down closes SSH first.
   Pure (no ESP-IDF headers) so the host tests cover the ordering: the owners of SSH sessions (the
   companion link, the SSH Terminal and take-over clients) register a closer; a path that is about to
   drop Wi-Fi, stop Tailscale or restart calls lg_close (or lg_around) first, which asks every closer to
   close its sessions and waits, bounded, until all are idle. Only then does the down step run, while
   the network still carries the SSH close to the Host. */
#ifdef __cplusplus
extern "C" {
#endif

#define LG_CLOSE_MS 2000   /* the bound on the close-and-wait */
#define LG_MAX 4

typedef struct {
    const char *name;
    /* Asks the sessions it owns to close (tailnet_only: only those over a tailnet address). True when it
       took something reopen must undo (a park); it may return false and still have closed sessions. */
    bool (*close)(bool tailnet_only, void *ctx);
    /* True once none of those sessions is open or opening. */
    bool (*idle)(bool tailnet_only, void *ctx);
    /* Undoes close's park (NULL: nothing to undo). */
    void (*reopen)(void *ctx);
    void *ctx;
} lg_closer_t;

typedef struct {
    int64_t (*now_ms)(void);
    void (*sleep_ms)(int ms);
} lg_clock_t;

/* The device sets esp_timer and vTaskDelay; the tests a fake clock. */
void lg_set_clock(const lg_clock_t *clk);
/* Copies c. False when LG_MAX are registered. Registered at init, before any session can open. */
bool lg_register(const lg_closer_t *c);
/* Tests only: forgets every closer. */
void lg_reset(void);

typedef struct {
    uint8_t took;   /* bit i: closer i took a park for reopen to undo */
    bool idle;      /* every closer was idle within the bound */
    int waited_ms;
} lg_token_t;

/* Closes every closer's sessions, then waits until all are idle or timeout_ms has passed. */
lg_token_t lg_close(bool tailnet_only, int timeout_ms);
/* Undoes the parks that t took (in reverse order). */
void lg_reopen(lg_token_t t);
/* lg_close, then down(ctx) (always: the close is bounded, the down is never skipped), then lg_reopen when
   reopen is set (the device stays up: a Wi-Fi leave or a network switch). Returns lg_close's idle. */
bool lg_around(bool tailnet_only, int timeout_ms, void (*down)(void *ctx), void *ctx, bool reopen);

#ifdef __cplusplus
}
#endif
