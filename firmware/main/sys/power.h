#pragma once
#include <stddef.h>
#include "sys/power_rules.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Power owner. Task 3: the keyboard light only; Task 5 adds the states. */

void power_init(void);                      /* starts the `power` task (stack 4096, priority 3) */
void power_kbd_light_refresh(void);         /* re-evaluates the light now; Settings calls it after a change */
void power_kbd_light_get(pr_led_t *out);    /* the current choice, for hot-plug */
/* The choice with its reason: "needs you: <name>", "nobody needs you", "setting off" or "result too old". */
void power_kbd_light_explain(pr_led_t *out, char *reason, size_t cap);
typedef enum { POWER_LIGHT_TEST_BIND, POWER_LIGHT_TEST_USER, POWER_LIGHT_TEST_REPROBE } power_light_test_t;
void power_kbd_light_test(power_light_test_t what);   /* debug (console `power light test ...`) */

#ifdef __cplusplus
}
#endif
