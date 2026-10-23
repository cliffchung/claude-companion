#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sys/power_rules.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Radio off and the long-idle check-ins (Plan 2 Task 8). The power task calls checkin_step() every
 * loop; the network work (parking, unparking, a check-in, the Hibernate check-in) runs on a one-shot
 * `checkin` task (6 KB), never on the power task (4 KB, about 2 KB free). One job at a time.
 *
 * The radio is parked in Radio off, and in Deep idle when Radio off is on (`pw_radio_off`). A
 * check-in: Wi-Fi up, the WireGuard handshake first (rebind only if it fails), SSH, one plain poll;
 * a new NEEDS YOU wakes the screen (the power task's needs-you rule), otherwise park again. */

void checkin_init(void);                        /* power_init, before the power task starts */
void checkin_step(power_state_t st, int64_t now_ms);   /* power task, every loop */

bool checkin_radio_parked(void);                /* the radio is parked by Radio off (or a park is under way) */
bool checkin_busy(void);                        /* a job task is running (idle_poll samples at 1 Hz meanwhile) */
/* The last wake found the radio parked: the dashboard shows "Reconnecting to <host>…" over its stale
   cards until the first fresh result (or until that reconnect gives up). */
bool checkin_woke_parked(void);
int  checkin_period_min(void);                  /* the period scheduled for the next check-in, 0 = none */
/* The light's age limit for a parked radio: the period plus the longest a check-in can take (M1), 0 = none */
int  checkin_light_period_min(void);
int  checkin_next_s(void);                      /* seconds to the next check-in, -1 = none scheduled */
uint8_t checkin_log_tag(void);                  /* 0x80|state while a check-in runs (0x83, 0x84, 0x85), else 0 */
void checkin_on_sample(void);                   /* idle_poll, after each sysinfo_poll: sums the check-in's mW */

/* PT16 Hibernate check-in. True from a check-in boot until a wake: the light follows the Deep-idle rule
   (amber at 30% or off) and a needs-you result never lights the screen (it only arms). */
bool checkin_hib_quiet(void);
/* PR_HC_STAY_DEEP_IDLE: stays in Deep idle until input or until a later result shows nobody needs you,
   then hibernates again; the ladder keeps only its battery <= 10% trigger meanwhile. */
bool checkin_hib_hold(void);

/* ---- console and bench ---- */
void checkin_force(bool rebind);                /* one check-in now (parks first if needed) */
void checkin_bench_park(bool on);               /* bench `deep-idle`: park in Deep idle whatever pw_radio_off says */
bool checkin_ran_since(uint32_t seq);           /* bench: a check-in finished after checkin_seq() was seq */
uint32_t checkin_seq(void);
void checkin_print(void);                       /* `power checkins`: the last 8 (path, ms, mWh), kept in NVS */
/* `power checkin hibernate [keep|nokeep]`: hibernates now with a 60 s wake; the check-in boot then follows
   PT16, except that a re-hibernate result stays awake (debug). keep / nokeep force the light branch. */
void checkin_hibernate_test(int force_kbd);     /* 0 = as built, 1 = keep, 2 = nokeep */

#ifdef __cplusplus
}
#endif
