#include "power.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "companion/cc_link.h"
#include "input/keyboard.h"
#include "settings/settings.h"

static const char *TAG = "power";

#define RECHECK_MS 10000   /* the result ages even when nothing arrives */

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_needs;               /* pr_any_needs_you of the last result */
static char s_needs_name[96];      /* the first needs-you session, for the console */
static bool s_hold;                /* debug: `power light test bind` keeps the owner from writing */
static bool s_have_applied;
static pr_led_t s_applied;

/* ---- the light -------------------------------------------------------- */

static void user_led(pr_led_t *u)
{
    u->mode = g_settings.kbd_led_mode ? 1 : 0;
    u->bright = g_settings.kbd_led_bright;
    u->r = g_settings.kbd_led_rgb[0]; u->g = g_settings.kbd_led_rgb[1]; u->b = g_settings.kbd_led_rgb[2];
}

/* Evaluates the rule on the stored state. Takes the lock itself. */
static void choose(pr_led_t *out, char *reason, size_t cap)
{
    pr_led_t user;
    user_led(&user);
    bool needs;
    char name[sizeof(s_needs_name)];
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    needs = s_needs;
    memcpy(name, s_needs_name, sizeof(name));
    if (s_lock) xSemaphoreGive(s_lock);
    int64_t last_ok = cc_poll_last_ok_ms();
    int64_t age = last_ok > 0 ? esp_timer_get_time() / 1000 - last_ok : INT64_MAX;
    /* Task 5 plugs in Radio off, the screen state and Deep idle */
    int64_t max_age = pr_light_max_age_ms(false, g_settings.pw_checkin_min);
    *out = pr_kbd_light(&user, g_settings.kbd_needs_light, needs, age, max_age, false, g_settings.kbd_off_cap, false);
    if (!reason) return;
    if (!g_settings.kbd_needs_light) snprintf(reason, cap, "setting off");
    else if (needs && age >= max_age) snprintf(reason, cap, "result too old");
    else if (needs) snprintf(reason, cap, "needs you: %s", name);
    else snprintf(reason, cap, "nobody needs you");
}

void power_kbd_light_get(pr_led_t *out) { choose(out, NULL, 0); }

void power_kbd_light_explain(pr_led_t *out, char *reason, size_t cap) { choose(out, reason, cap); }

/* keyboard_set_led_source: runs on the keyboard task when a keyboard is (re)detected */
static void led_source(uint8_t *mode, uint8_t *bright, uint8_t *r, uint8_t *g, uint8_t *b)
{
    pr_led_t l;
    power_kbd_light_get(&l);
    *mode = l.mode; *bright = l.bright; *r = l.r; *g = l.g; *b = l.b;
}

static void apply_light(bool force)
{
    pr_led_t l;
    keyboard_led_lock();   /* choose and write as one step, so a hot-plug probe can't land an older colour */
    power_kbd_light_get(&l);
    if (!s_hold && (force || !s_have_applied || memcmp(&l, &s_applied, sizeof(l)) != 0)) {
        /* remembered only after the keyboard took it: an absent keyboard or failed write retries */
        s_have_applied = keyboard_write_led(l.mode, l.bright, l.r, l.g, l.b);
        s_applied = l;
    }
    keyboard_led_unlock();
}

void power_kbd_light_refresh(void)
{
    if (s_task) xTaskNotifyGive(s_task);
    else {   /* before power_init: nothing owns the light yet */
        apply_light(true);
    }
}

void power_kbd_light_test(power_light_test_t what)
{
    switch (what) {
    case POWER_LIGHT_TEST_BIND:
        s_hold = true;
        keyboard_set_led(0, 60, 0, 0, 0);
        break;
    case POWER_LIGHT_TEST_USER:
        s_hold = false;
        s_have_applied = false;
        power_kbd_light_refresh();
        break;
    case POWER_LIGHT_TEST_REPROBE:
        /* the keyboard task re-detects it and asks led_source */
        s_hold = false;
        s_have_applied = false;
        keyboard_mark_absent();
        break;
    }
}

/* ---- results from the Mac ---------------------------------------------- */

/* On the poll task: copy what we need and wake our own task. No cc_link calls. */
static void on_sessions(const cc_session_t *s, int n, void *ctx)
{
    (void)ctx;
    bool needs = pr_any_needs_you(s, n);
    char name[sizeof(s_needs_name)] = "";
    for (int i = 0; i < n && needs; i++) {
        if (cc_session_needs_you(&s[i])) {
            snprintf(name, sizeof(name), "%s", s[i].name);
            break;
        }
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_needs = needs;
    memcpy(s_needs_name, name, sizeof(s_needs_name));
    if (s_lock) xSemaphoreGive(s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

static void power_task(void *arg)
{
    (void)arg;
    apply_light(true);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RECHECK_MS));
        apply_light(false);
    }
}

void power_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    keyboard_set_led_source(led_source);
    cc_poll_subscribe(on_sessions, NULL);
    if (xTaskCreate(power_task, "power", 4096, NULL, 3, &s_task) != pdPASS)
        ESP_LOGE(TAG, "no memory for the power task");
}
