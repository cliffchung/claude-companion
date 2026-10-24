#pragma once
#include <stdbool.h>
#include <stdint.h>
/* The companion link's park state (Plan 3 final review C1/I1). Pure, host-tested.
   Two owners can park the link: Radio off (cc_link_park, a check-in, hibernate) and the close-before-down
   guard (cc_link_close_now, nested). The link is parked while either holds it, so a guard's reopen never
   unparks a Radio-off park and the reverse. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool radio;      /* cc_link_park(true) */
    uint8_t guard;   /* cc_link_close_now takes, cc_link_reopen drops (nested) */
} cc_park_t;

void cc_park_set_radio(cc_park_t *p, bool on);
void cc_park_guard_take(cc_park_t *p);
void cc_park_guard_drop(cc_park_t *p);   /* never below 0 */
bool cc_park_on(const cc_park_t *p);

/* I1: a session that has just authenticated is kept only while the link isn't parked; parked, it is
   disconnected at once (the Wi-Fi is still up, so the Host sees the close). */
bool cc_park_keep_new_session(const cc_park_t *p);
/* I1: a refresh or paste in flight gives up between steps once the link is parked. */
bool cc_park_abort_work(const cc_park_t *p);
/* Idle: no session and no connect in flight (what the close-before-down waits for). */
bool cc_park_idle(bool have_session, bool in_connect);

#ifdef __cplusplus
}
#endif
