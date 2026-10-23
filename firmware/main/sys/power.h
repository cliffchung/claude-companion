#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sys/power_rules.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Power owner: one task runs the ladder Active -> Dim -> Off (-> Radio off) -> Deep idle ->
 * Hibernate, the wake path and the keyboard light. Every transition happens on that task;
 * the calls below only record a request and notify it. */

void power_init(void);                      /* starts the `power` task (stack 4096, priority 3) */

/* ---- states and wakes ---- */
power_state_t power_state(void);
const char *power_state_name(power_state_t s);   /* active, dim, off, radio-off, deep-idle, hibernate */
uint32_t power_wake_seq(void);              /* bumped on every wake */
uint32_t power_wake_gen(void);              /* cc_poll_generation() when the last wake happened */
power_state_t power_woke_from(void);        /* the state the last wake left (the dashboard marks cards
                                               stale only after Off or deeper: Dim never slowed the link) */
typedef enum { PW_KEY, PW_TOUCH, PW_LIFT, PW_NEEDS_YOU, PW_CONSOLE,
               PW_RULE   /* the ladder moved up without input: a hold, USB without sleep, a longer timeout */
} power_wake_t;
power_wake_t power_wake_reason(void);
const char *power_wake_name(power_wake_t w);
/* The keyboard drain calls this on the LVGL task for every key: false = drop it (pr_swallow).
   A drop while not Active also wakes. */
bool power_take_key(void);
/* Any task / an ISR. Wakes to Active when not Active; when Active it resets idle. */
void power_wake(power_wake_t why);
void power_wake_from_isr(power_wake_t why);
/* v1 touch INT (display_ctl_touch_int_init) */
void power_touch_isr(void *arg);

typedef enum { POWER_HOLD_TAKEOVER, POWER_HOLD_SSH, POWER_HOLD_BENCH } power_hold_t;
/* POWER_HOLD_SSH counts only while g_settings.ss_disable_in_ssh; cc_link_busy() is polled as
   the third hold. POWER_HOLD_BENCH keeps the bench's Active steps Active. */
void power_hold(power_hold_t h, bool on);
/* "takeover ssh busy bench" (the ones in force), or "none" */
void power_holds_text(char *buf, size_t cap);

int power_poll_period_ms(void);             /* idle_poll: 1000, or 10 000 in Off and deeper while logging is off */
/* idle_poll waits for power_poll_period_ms() or a kick (a wake, the log starting). */
void power_poll_task_set(void *task);       /* TaskHandle_t of idle_poll */
void power_poll_kick(void);
/* Console and bench. ACTIVE wakes (PW_CONSOLE). Any other state is entered at once and pinned:
   the ladder leaves it alone until the next wake. HIBERNATE shows the notice for 3 s first. */
void power_go(power_state_t s);
int64_t power_idle_ms(void);

/* ---- the wake shield (a full-screen catcher on lv_layer_top while not Active) ---- */
bool power_shield_up(void);                 /* LVGL task: gestures are ignored while it is up */

/* ---- console and bench helpers ---- */
/* `power set <name> <n>` and Settings -> Power: the Settings clamps, then saves.
   Names: dim, off, sleep-usb, needs-wake, deep-idle, hibernate. False for an unknown name. */
bool power_setting_set(const char *name, int n);
void power_settings_defaults(void);         /* every Plan 2 power setting back to its Task 2 default */
void power_settings_text(char *buf, size_t cap);   /* name=value ... on one line */
void power_sim_battery(int pct);            /* RAM only: pct >= 0 overrides battery_pct and on_battery; < 0 ends it */
void power_sim_idle(int seconds);           /* moves last input (and off_since when Off) that far back */
bool power_sim_touch(void);                 /* a press and release at the selected card, through a virtual pointer */
void power_touch_text(char *buf, size_t cap);   /* "touch: poll on|off, reads=<n> errors=<n> last=..." */
void power_wakes_print(void);               /* the last 8 wakes: "<seconds ago> <reason> swallowed=<n>" */
void power_touch_poll_pause(bool pause);    /* bench: Off with the poll stopped */
typedef enum { POWER_BENCH_LIGHT_NONE, POWER_BENCH_LIGHT_AMBER, POWER_BENCH_LIGHT_OFF } power_bench_light_t;
void power_bench_light(power_bench_light_t l);

/* ---- Hibernate (stubs until Task 6 moves them to sys/hibernate.c) ---- */
typedef enum { POWER_BOOT_NORMAL, POWER_BOOT_HIB_WAKE } power_boot_t;
bool hibernate_supported(void);             /* false until Task 6 */
void hibernate_now(void);                   /* logs "hibernate: not built" and returns */
power_boot_t hibernate_boot_kind(void);     /* POWER_BOOT_NORMAL until Task 6 */

/* ---- the keyboard light ---- */
void power_kbd_light_refresh(void);         /* re-evaluates the light now; Settings calls it after a change */
void power_kbd_light_get(pr_led_t *out);    /* the current choice, for hot-plug */
/* The choice with its reason: "needs you: <name>", "nobody needs you", "setting off" or "result too old". */
void power_kbd_light_explain(pr_led_t *out, char *reason, size_t cap);
typedef enum { POWER_LIGHT_TEST_BIND, POWER_LIGHT_TEST_USER, POWER_LIGHT_TEST_REPROBE } power_light_test_t;
void power_kbd_light_test(power_light_test_t what);   /* debug (console `power light test ...`) */

#ifdef __cplusplus
}
#endif
