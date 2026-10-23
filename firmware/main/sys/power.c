#include "power.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "companion/cc_link.h"
#include "input/keyboard.h"
#include "settings/settings.h"
#include "sys/sysinfo.h"
#include "sys/vitals.h"
#include "sys/power_log.h"
#include "sys/display_ctl.h"
#include "ui/ui.h"
#include "ui/ui_dashboard.h"

static const char *TAG = "power";

#define LOOP_MS 500                 /* the ladder's period while the screen is lit */
#define TOUCH_POLL_MS 100           /* the v2 touch poll in Off and deeper */
#define LIGHT_EVERY_MS 1000         /* the light is re-chosen at least this often (the result ages) */
#define OFF_FLOOR_S 15              /* watch interval in Off and deeper (PT11) */
#define NOTICE_MS 3000              /* the pre-Hibernate notice */
#define STILL_HIBERNATE_MS (30 * 60 * 1000LL)   /* Task 7 supplies still_ms and the setting */
#define WAKES_KEPT 8

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- shared state --------------------------------------------------------
 * s_mux guards the input and wake fields: they are touched from the LVGL task, the keyboard
 * task, the console and an ISR. Everything else belongs to the power task. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile power_state_t s_state = PS_ACTIVE;
static int64_t s_last_input_ms, s_off_since_ms;
static int64_t s_woke_at_ms = -1000000, s_last_swallowed_ms = -1000000;
static bool s_wake_pending;
static power_wake_t s_wake_why, s_last_reason = PW_CONSOLE;
static uint32_t s_wake_seq;
static uint32_t s_wake_gen;         /* cc_poll_generation() at the last wake */
static power_state_t s_woke_from = PS_ACTIVE;
static bool s_pinned;               /* power_go: the ladder leaves the state alone until a wake */
static int s_go_req = -1;
static uint32_t s_holds;            /* 1 << power_hold_t */
static int s_sim_batt = -1;         /* power sim battery */
typedef struct { int64_t at_ms; uint8_t why; uint16_t swallowed; } wake_rec_t;
static wake_rec_t s_wakes[WAKES_KEPT];
static int s_wakes_n, s_wakes_head;

static SemaphoreHandle_t s_lock;    /* the needs-you fields below */
static TaskHandle_t s_task;
static bool s_needs;                /* pr_any_needs_you of the last result */
static char s_needs_name[96];       /* the first needs-you session, for the console */
/* The id sets are 516 B each and internal RAM is tight (TLS needs it): they live in PSRAM. */
static pr_idset_t *s_now_ids;       /* pr_needs_you_ids of the last result */
static uint32_t s_ids_gen;

/* power task only */
static bool s_dark;                 /* panel off, LVGL paused, keyboard quiet */
static bool s_hib_blocked;          /* Hibernate returned (not built): don't retry until a wake */
static pr_idset_t *s_armed;         /* needs-you ids already known when the screen went off */
static pr_idset_t *s_poll_ids;      /* poll task scratch */
static pr_idset_t *s_step_ids;      /* power task scratch */
static uint32_t s_armed_gen;
static bool s_poll_on, s_poll_paused;
static uint32_t s_poll_reads, s_poll_errors;
static const char *s_poll_last = "released";
static bool s_poll_err_logged;
static power_bench_light_t s_bench_light;

/* LVGL task (or under bsp_display_lock) */
static lv_obj_t *s_shield;
static lv_indev_t *s_vptr;
static lv_indev_state_t s_vstate = LV_INDEV_STATE_RELEASED;
static lv_point_t s_vpt;

/* the light (unchanged from Task 3 apart from the state flags) */
static bool s_hold_light;           /* debug: `power light test bind` keeps the owner from writing */
static bool s_have_applied;
static pr_led_t s_applied;

static void notify(void) { if (s_task) xTaskNotifyGive(s_task); }

/* The display lock, logging every 2 s it waits: a stuck LVGL task shows up in the log. */
static void dlock(int line)
{
    int64_t t0 = esp_timer_get_time();
    while (!bsp_display_lock(2000))
        ESP_LOGE(TAG, "display lock: waiting %lld ms at line %d", (long long)(esp_timer_get_time() - t0) / 1000, line);
}

const char *power_state_name(power_state_t s)
{
    static const char *n[] = { "active", "dim", "off", "radio-off", "deep-idle", "hibernate" };
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

const char *power_wake_name(power_wake_t w)
{
    static const char *n[] = { "key", "touch", "lift", "needs-you", "console", "rule" };
    return (unsigned)w < sizeof(n) / sizeof(n[0]) ? n[w] : "?";
}

power_state_t power_state(void) { return s_state; }
uint32_t power_wake_seq(void) { return s_wake_seq; }
uint32_t power_wake_gen(void) { return s_wake_gen; }
power_state_t power_woke_from(void) { return s_woke_from; }
power_wake_t power_wake_reason(void) { return s_last_reason; }

int64_t power_idle_ms(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t t = s_last_input_ms;
    taskEXIT_CRITICAL(&s_mux);
    return now_ms() - t;
}

/* ---- wake requests (any task, under s_mux) ------------------------------- */

static wake_rec_t *cur_wake_locked(void)
{
    return s_wakes_n ? &s_wakes[(s_wakes_head + WAKES_KEPT - 1) % WAKES_KEPT] : NULL;
}

/* Records a wake request: the power task does the work. While Active it only resets idle. */
static bool wake_locked(power_wake_t why, int64_t now)
{
    if (why != PW_NEEDS_YOU && why != PW_RULE) s_last_input_ms = now;
    if (s_state == PS_ACTIVE || s_wake_pending) return false;
    s_wake_pending = true;
    s_wake_why = why;
    s_woke_at_ms = now;   /* first: pr_swallow drops the waking input from here on */
    s_wakes[s_wakes_head] = (wake_rec_t){ now, (uint8_t)why, 0 };
    s_wakes_head = (s_wakes_head + 1) % WAKES_KEPT;
    if (s_wakes_n < WAKES_KEPT) s_wakes_n++;
    return true;
}

void power_wake(power_wake_t why)
{
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_mux);
    wake_locked(why, now);
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

void IRAM_ATTR power_wake_from_isr(power_wake_t why)
{
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL_ISR(&s_mux);
    wake_locked(why, now);
    portEXIT_CRITICAL_ISR(&s_mux);
    BaseType_t hp = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void IRAM_ATTR power_touch_isr(void *arg)
{
    (void)arg;
    power_wake_from_isr(PW_TOUCH);
}

bool power_take_key(void)
{
    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_mux);
    power_state_t st = s_state;
    bool drop = pr_swallow(st, now, s_woke_at_ms, s_last_swallowed_ms);
    if (drop) {
        s_last_swallowed_ms = now;   /* a held key's repeats keep being dropped */
        if (st != PS_ACTIVE) wake_locked(PW_KEY, now);
        wake_rec_t *w = cur_wake_locked();
        if (w && w->swallowed < 0xFFFF) w->swallowed++;
    } else {
        s_last_input_ms = now;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (drop) notify();
    return !drop;
}

/* Touch indev events (LVGL task). A press on the shield is eaten and counted. */
static void indev_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;
    int64_t now = now_ms();
    bool woke = false;
    taskENTER_CRITICAL(&s_mux);
    if (code == LV_EVENT_PRESSED && s_state != PS_ACTIVE) woke = wake_locked(PW_TOUCH, now);
    else s_last_input_ms = now;
    if (code == LV_EVENT_PRESSED && s_shield) {
        wake_rec_t *w = cur_wake_locked();
        if (w && w->swallowed < 0xFFFF) w->swallowed++;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (woke) notify();
}

/* ---- holds --------------------------------------------------------------- */

void power_hold(power_hold_t h, bool on)
{
    taskENTER_CRITICAL(&s_mux);
    if (on) s_holds |= 1u << h;
    else s_holds &= ~(1u << h);
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

static bool holds_active(void)
{
    uint32_t h = s_holds;
    if (h & (1u << POWER_HOLD_TAKEOVER)) return true;
    if ((h & (1u << POWER_HOLD_SSH)) && g_settings.ss_disable_in_ssh) return true;
    if (h & (1u << POWER_HOLD_BENCH)) return true;
    return cc_link_busy();
}

void power_holds_text(char *buf, size_t cap)
{
    uint32_t h = s_holds;
    buf[0] = 0;
    if (h & (1u << POWER_HOLD_TAKEOVER)) strlcat(buf, "takeover,", cap);
    if ((h & (1u << POWER_HOLD_SSH)) && g_settings.ss_disable_in_ssh) strlcat(buf, "ssh,", cap);
    if (cc_link_busy()) strlcat(buf, "busy,", cap);
    if (h & (1u << POWER_HOLD_BENCH)) strlcat(buf, "bench,", cap);
    size_t n = strlen(buf);
    if (n) buf[n - 1] = 0;
    else strlcpy(buf, "none", cap);
}

/* ---- inputs to the rules ------------------------------------------------- */

static bool on_battery_now(int *pct, bool *charging)
{
    power_info_t p = sysinfo_power();
    int sim = s_sim_batt;
    if (sim >= 0) { *pct = sim; *charging = false; return true; }
    *pct = p.percent;
    *charging = p.src == PWR_USB_CHARGING;
    return p.src == PWR_BATTERY;   /* vitals' source: unknown or no pack is never battery (PT6-PT8) */
}

static void fill_in(pr_in_t *in, int64_t now)
{
    memset(in, 0, sizeof(*in));
    in->state = s_state;
    in->now_ms = now;
    taskENTER_CRITICAL(&s_mux);
    in->last_input_ms = s_last_input_ms;
    in->off_since_ms = s_off_since_ms;
    taskEXIT_CRITICAL(&s_mux);
    in->dim_s = g_settings.pw_dim_s;
    in->off_s = g_settings.pw_off_s;
    in->radio_off_min = g_settings.pw_radio_off_min;
    in->deep_idle_min = g_settings.pw_deep_idle_min;
    in->hibernate_h = g_settings.pw_hibernate_h;
    in->still_ms = 0;                         /* Task 7: the IMU */
    in->still_hibernate_ms = STILL_HIBERNATE_MS;
    in->on_battery = on_battery_now(&in->battery_pct, &in->charging);
    in->held = holds_active();
    in->sleep_on_usb = g_settings.pw_sleep_on_usb;
    in->radio_off_enabled = g_settings.pw_radio_off;
    in->hibernate_ok = hibernate_supported() && !s_hib_blocked;
}

static TaskHandle_t s_poll_task;
void power_poll_task_set(void *task) { s_poll_task = task; }
void power_poll_kick(void) { if (s_poll_task) xTaskNotifyGive(s_poll_task); }

int power_poll_period_ms(void)
{
    power_state_t s = s_state;
    bool dark = s == PS_OFF || s == PS_RADIO_OFF || s == PS_DEEP_IDLE;
    return dark && !power_log_on() ? 10000 : 1000;
}

/* ---- the light ----------------------------------------------------------- */

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
    int64_t age = last_ok > 0 ? now_ms() - last_ok : INT64_MAX;
    power_state_t st = s_state;
    int pct;
    bool chg;
    bool screen_off = st >= PS_OFF && on_battery_now(&pct, &chg);   /* F30: the Off cap is for battery */
    bool deep_idle = st >= PS_DEEP_IDLE;
    /* radio_off stays false until Task 8 parks the radio */
    int64_t max_age = pr_light_max_age_ms(false, g_settings.pw_checkin_min);
    power_bench_light_t bl = s_bench_light;
    if (bl == POWER_BENCH_LIGHT_OFF) {
        *out = PR_LED_OFF;
        if (reason) snprintf(reason, cap, "bench: off");
        return;
    }
    if (bl == POWER_BENCH_LIGHT_AMBER) {
        *out = pr_kbd_light(&user, true, true, 0, max_age, screen_off, g_settings.kbd_off_cap, deep_idle);
        if (reason) snprintf(reason, cap, "bench: amber");
        return;
    }
    *out = pr_kbd_light(&user, g_settings.kbd_needs_light, needs, age, max_age, screen_off, g_settings.kbd_off_cap,
                        deep_idle);
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
    if (!s_hold_light && (force || !s_have_applied || memcmp(&l, &s_applied, sizeof(l)) != 0)) {
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
        s_hold_light = true;
        keyboard_set_led(0, 60, 0, 0, 0);
        break;
    case POWER_LIGHT_TEST_USER:
        s_hold_light = false;
        s_have_applied = false;
        power_kbd_light_refresh();
        break;
    case POWER_LIGHT_TEST_REPROBE:
        /* the keyboard task re-detects it and asks led_source */
        s_hold_light = false;
        s_have_applied = false;
        keyboard_mark_absent();
        break;
    }
}

void power_bench_light(power_bench_light_t l)
{
    s_bench_light = l;
    notify();
}

/* ---- results from the Mac ------------------------------------------------ */

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
    pr_needs_you_ids(s, n, s_poll_ids);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_needs = needs;
    memcpy(s_needs_name, name, sizeof(s_needs_name));
    *s_now_ids = *s_poll_ids;
    s_ids_gen++;
    if (s_lock) xSemaphoreGive(s_lock);
    notify();
}

/* ---- the shield and the notice (under bsp_display_lock) ------------------ */

static void shield_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) { if (lv_event_get_target_obj(e) == s_shield) s_shield = NULL; return; }
    /* the release that follows the wake */
    if (code == LV_EVENT_RELEASED && s_state == PS_ACTIVE && !s_wake_pending && s_shield)
        lv_obj_delete_async(s_shield);
}

static void shield_up_locked(void)
{
    if (!s_shield) {
        lv_obj_t *o = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | UI_KEYBOARD_SKIP);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(o, shield_event_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(o, shield_event_cb, LV_EVENT_DELETE, NULL);
        s_shield = o;
    }
    lv_obj_move_foreground(s_shield);
}

bool power_shield_up(void) { return s_shield != NULL; }
void *power_shield_obj(void) { return s_shield; }

/* While not Active the shield stays the top layer's last child: a toast or the music bar made
   while Dim must not sit above it. Dark needs nothing: LVGL is paused, and the wake re-raises it. */
static void shield_on_top(void)
{
    if (!s_shield || s_state == PS_ACTIVE || s_dark) return;
    dlock(__LINE__);
    if (s_shield && lv_obj_get_index(s_shield) != (int32_t)lv_obj_get_child_count(lv_layer_top()) - 1)
        lv_obj_move_foreground(s_shield);
    bsp_display_unlock();
}

/* After a wake: gone PR_SWALLOW_MS later when no press is down. */
static void shield_expire(int64_t now)
{
    if (!s_shield || s_state != PS_ACTIVE || s_wake_pending || now - s_woke_at_ms < PR_SWALLOW_MS) return;
    dlock(__LINE__);
    lv_indev_t *tp = bsp_display_get_input_dev();
    bool down = (tp && lv_indev_get_state(tp) == LV_INDEV_STATE_PRESSED) || s_vstate == LV_INDEV_STATE_PRESSED;
    if (s_shield && !down) { lv_obj_delete(s_shield); s_shield = NULL; }
    bsp_display_unlock();
}

/* ---- transitions (power task) -------------------------------------------- */

static void set_state(power_state_t st)
{
    power_state_t was = s_state;
    if (was == st) return;
    taskENTER_CRITICAL(&s_mux);
    s_state = st;
    if (st >= PS_OFF && was < PS_OFF) s_off_since_ms = now_ms();
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "state: %s -> %s", power_state_name(was), power_state_name(st));
}

static void arm_needs_you(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s_armed = *s_now_ids;
    s_armed_gen = s_ids_gen;
    xSemaphoreGive(s_lock);
}

/* LVGL quiet: lvgl_port_stop() stops the 5 ms tick, but with timers disabled lv_timer_handler()
   returns 1 ms and the LVGL task loops every tick. So timers stay enabled and each one is paused
   instead: with nothing ready the task sleeps its task_max_sleep_ms (500 ms). Under the display lock. */
#define PAUSED_MAX 64
static lv_timer_t *s_paused[PAUSED_MAX];
static int s_paused_n;

static void lvgl_quiet_locked(void)
{
    lvgl_port_stop();
    lv_timer_enable(true);
    s_paused_n = 0;
    static bool s_cap_logged;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        if (lv_timer_get_paused(t)) continue;
        if (s_paused_n == PAUSED_MAX) {
            if (!s_cap_logged) { s_cap_logged = true; ESP_LOGW(TAG, "LVGL quiet: over %d timers, the rest keep running", PAUSED_MAX); }
            break;
        }
        lv_timer_pause(t);
        s_paused[s_paused_n++] = t;
    }
}

static void lvgl_wake_locked(void)
{
    /* only timers still alive: one may have been deleted while dark */
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
        for (int i = 0; i < s_paused_n; i++)
            if (s_paused[i] == t) { lv_timer_resume(t); break; }
    s_paused_n = 0;
    lvgl_port_resume();
}

/* The shield and the state change under one display lock: the shield's release handler (LVGL
   task) deletes it while the state is Active, so it must never see the shield up and Active. */
static void shield_and_state(power_state_t st)
{
    dlock(__LINE__);
    shield_up_locked();
    set_state(st);
    bsp_display_unlock();
}

static void enter_dim(void)
{
    shield_and_state(PS_DIM);
    bsp_display_brightness_set(pr_dim_brightness(g_settings.brightness));
}

/* Off entry: panel off, backlight 0, LVGL paused, keyboard quiet, watch at 15 s. The touch
   controller stays awake (never bsp_display_enter_sleep / bsp_touch_enter_sleep). */
static void enter_dark(power_state_t target)
{
    shield_and_state(target);   /* before anything: a press on a dim or dark screen never reaches a card */
    dlock(__LINE__);
    display_ctl_panel_on(false);
    bsp_display_brightness_set(0);
    /* LVGL's event-mode touch read runs from the INT even with its timers stopped: disable the
       indev so the poll below is the only reader of the controller */
    lv_indev_t *tp = bsp_display_get_input_dev();
    if (tp) lv_indev_enable(tp, false);
    lvgl_quiet_locked();
    bsp_display_unlock();
    keyboard_set_quiet(true);
    cc_poll_set_floor(OFF_FLOOR_S);
    arm_needs_you();
    s_dark = true;
    s_poll_on = display_ctl_board_version() != 1 && !s_poll_paused;   /* after LVGL is quiet */
}

static void leave_dark(void)
{
    s_poll_on = false;   /* first: it never overlaps LVGL's own touch read */
    dlock(__LINE__);
    lvgl_wake_locked();
    lv_indev_t *tp = bsp_display_get_input_dev();
    if (tp) lv_indev_enable(tp, true);
    display_ctl_panel_on(true);
    bsp_display_unlock();
    s_dark = false;
}

/* LVGL task: a needs-you wake goes to the dashboard. Built here, not on the power task's 4 KB
   stack: a screen build and the old screen's delete were sized for the LVGL task's 16 KB. */
static void nav_home_async(void *arg)
{
    (void)arg;
    /* off the dashboard polling is slow; an SSH Terminal is left alone (it would close) */
    if (!ui_nav_is_top(ui_dashboard_create) && !ui_terminal_is_top()) ui_nav_home();
}

/* The wake. The state stays non-Active until the last step, so the drain keeps swallowing. */
static void do_wake(void)
{
    int64_t t0 = now_ms();
    taskENTER_CRITICAL(&s_mux);
    power_wake_t why = s_wake_why;
    taskEXIT_CRITICAL(&s_mux);
    power_state_t from = s_state;
    power_state_t target = why == PW_NEEDS_YOU ? PS_DIM : PS_ACTIVE;
    if (s_dark) leave_dark();
    bsp_display_brightness_set(target == PS_DIM ? pr_dim_brightness(g_settings.brightness) : g_settings.brightness);
    /* before the dashboard can be built below: it reads these */
    s_wake_gen = cc_poll_generation();
    s_woke_from = from;
    s_last_reason = why;
    s_wake_seq++;
    dlock(__LINE__);
    lv_display_trigger_activity(NULL);
    if (why == PW_NEEDS_YOU) lv_async_call(nav_home_async, NULL);
    if (s_shield) lv_obj_move_foreground(s_shield);   /* over anything added to the top layer meanwhile */
    bsp_display_unlock();
    cc_poll_set_floor(0);
    keyboard_set_quiet(false);
    power_poll_kick();   /* back to 1 Hz at once */
    int64_t now = now_ms();
    /* the state and the pending flag change together: an input in between would otherwise see a
       non-Active state with no wake pending and record a second wake */
    taskENTER_CRITICAL(&s_mux);
    if (target == PS_DIM) s_last_input_ms = now - (int64_t)g_settings.pw_dim_s * 1000;   /* lands in Dim */
    else if (why == PW_RULE) s_last_input_ms = now;
    s_state = target;
    s_wake_pending = false;
    s_pinned = false;
    taskEXIT_CRITICAL(&s_mux);
    s_hib_blocked = false;
    ESP_LOGI(TAG, "state: %s -> %s", power_state_name(from), power_state_name(target));
    ESP_LOGI(TAG, "wake: %s from %s in %lld ms (stack free %u B)", power_wake_name(why), power_state_name(from),
             (long long)(now - t0), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    apply_light(false);
}

/* A wake the power task decides on itself (needs-you, a hold, the console's go). */
static void internal_wake(power_wake_t why)
{
    taskENTER_CRITICAL(&s_mux);
    wake_locked(why, now_ms());
    taskEXIT_CRITICAL(&s_mux);
    do_wake();
}

/* Shows the notice for 3 s, then hibernate_now(). Input or a hold during the 3 s cancels it. */
static void run_hibernate(power_state_t back_to)
{
    shield_and_state(PS_HIBERNATE);
    if (s_dark) leave_dark();
    bsp_display_brightness_set(pr_dim_brightness(g_settings.brightness));
    dlock(__LINE__);
    ui_sleep_notice(true);
    if (s_shield) lv_obj_move_foreground(s_shield);   /* the notice never takes a press */
    bsp_display_unlock();
    bool cancel = false;
    for (int64_t end = now_ms() + NOTICE_MS; now_ms() < end && !cancel;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (holds_active()) power_wake(PW_RULE);
        cancel = s_wake_pending;
    }
    if (!cancel) {
        hibernate_now();   /* returns only while Hibernate isn't built */
        cancel = s_wake_pending;
    }
    dlock(__LINE__);
    ui_sleep_notice(false);
    bsp_display_unlock();
    if (cancel) { do_wake(); return; }
    s_hib_blocked = true;
    enter_dark(back_to >= PS_OFF && back_to != PS_HIBERNATE ? back_to : PS_OFF);
}

static void handle_go(void)
{
    taskENTER_CRITICAL(&s_mux);
    int go = s_go_req;
    s_go_req = -1;
    taskEXIT_CRITICAL(&s_mux);
    if (go < 0) return;
    power_state_t st = s_state, to = (power_state_t)go;
    if (to == PS_HIBERNATE) {
        run_hibernate(PS_OFF);
    } else if (to == PS_DIM) {
        if (st == PS_ACTIVE) enter_dim();
        else if (st != PS_DIM) {
            taskENTER_CRITICAL(&s_mux);
            wake_locked(PW_CONSOLE, now_ms());
            taskEXIT_CRITICAL(&s_mux);
            do_wake();
            enter_dim();
        }
    } else if (to >= PS_OFF) {
        if (!s_dark) enter_dark(to);
        else set_state(to);
        apply_light(false);
    }
    if (s_state != PS_ACTIVE) {
        taskENTER_CRITICAL(&s_mux);
        s_pinned = true;
        taskEXIT_CRITICAL(&s_mux);
    }
}

static void ladder_step(int64_t now)
{
    if (s_pinned) return;
    pr_in_t in;
    fill_in(&in, now);
    power_state_t st = s_state, next = pr_next_state(&in);
    if (next == st) return;
    if (next == PS_HIBERNATE) { run_hibernate(st); return; }
    if (next == PS_ACTIVE) { internal_wake(PW_RULE); return; }   /* a hold, USB without sleep */
    if (next == PS_DIM) {
        if (st == PS_ACTIVE) enter_dim();
        else { internal_wake(PW_RULE); enter_dim(); }   /* a longer timeout while dark */
        return;
    }
    if (!s_dark) enter_dark(next);
    else set_state(next);   /* Off, Radio off and Deep idle share the dark screen in Task 5 */
    apply_light(false);
}

/* A new NEEDS YOU while the screen is off wakes it to Dim on the dashboard. */
static void needs_you_step(void)
{
    if (s_state < PS_OFF || s_wake_pending) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fresh = s_ids_gen != s_armed_gen;
    if (fresh) *s_step_ids = *s_now_ids;
    s_armed_gen = s_ids_gen;
    xSemaphoreGive(s_lock);
    if (!fresh) return;
    bool wake = g_settings.pw_wake_needs_you && pr_needs_you_new(s_armed, s_step_ids);
    *s_armed = *s_step_ids;   /* the new ids join; ids that stopped needing you leave (a later block counts again) */
    if (wake) internal_wake(PW_NEEDS_YOU);
}

static void touch_poll_step(void)
{
    if (!s_poll_on || !s_dark) return;
    bool pressed = false;
    esp_err_t e = display_ctl_touch_poll(&pressed);
    s_poll_reads++;
    if (e != ESP_OK) {
        s_poll_errors++;
        s_poll_last = "error";
        if (!s_poll_err_logged) { s_poll_err_logged = true; ESP_LOGW(TAG, "touch poll: %s", esp_err_to_name(e)); }
        return;   /* a read error never wakes */
    }
    s_poll_last = pressed ? "pressed" : "released";
    if (pressed) power_wake(PW_TOUCH);
}

static void power_task(void *arg)
{
    (void)arg;
    apply_light(true);
    int64_t next_light = 0;
    for (;;) {
        bool notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_poll_on ? TOUCH_POLL_MS : LOOP_MS)) > 0;
        s_poll_on = s_dark && !s_poll_paused && display_ctl_board_version() != 1;   /* only this task writes it */
        touch_poll_step();
        handle_go();
        if (s_wake_pending) do_wake();
        needs_you_step();
        /* the time is read after the steps above: a wake just moved last input */
        shield_on_top();
        shield_expire(now_ms());
        ladder_step(now_ms());
        int64_t now = now_ms();
        if (notified || now >= next_light) {
            apply_light(false);
            next_light = now + LIGHT_EVERY_MS;
        }
    }
}

void power_go(power_state_t s)
{
    if (s == PS_ACTIVE) { power_wake(PW_CONSOLE); return; }
    taskENTER_CRITICAL(&s_mux);
    s_go_req = s;
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

/* ---- console and bench helpers ------------------------------------------- */

static uint16_t u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }

bool power_setting_set(const char *name, int n)
{
    if (strcmp(name, "dim") == 0) {
        g_settings.pw_dim_s = u16(n);
        vitals_clamp_dim_off(&g_settings.pw_dim_s, &g_settings.pw_off_s);
    } else if (strcmp(name, "off") == 0) {
        g_settings.pw_off_s = u16(n);
        vitals_clamp_dim_off(&g_settings.pw_dim_s, &g_settings.pw_off_s);
    } else if (strcmp(name, "sleep-usb") == 0) {
        g_settings.pw_sleep_on_usb = n ? 1 : 0;
    } else if (strcmp(name, "needs-wake") == 0) {
        g_settings.pw_wake_needs_you = n ? 1 : 0;
    } else if (strcmp(name, "deep-idle") == 0) {
        g_settings.pw_deep_idle_min = (uint16_t)vitals_clamp_or_zero(u16(n), 15, 720);
    } else if (strcmp(name, "hibernate") == 0) {
        g_settings.pw_hibernate_h = (uint8_t)vitals_clamp_or_zero(u16(n), 1, 48);
    } else {
        return false;
    }
    settings_save();
    notify();
    return true;
}

void power_settings_defaults(void)
{
    settings_power_defaults();
    settings_save();
    power_kbd_light_refresh();
}

void power_settings_text(char *buf, size_t cap)
{
    snprintf(buf, cap,
             "dim=%u off=%u sleep-usb=%u wake-lift=%u needs-wake=%u needs-light=%u off-cap=%u radio-off=%u "
             "radio-off-min=%u checkin=%u light-sleep=%u deep-idle=%u hibernate=%u hib-checkin=%u ssh-awake=%u",
             g_settings.pw_dim_s, g_settings.pw_off_s, g_settings.pw_sleep_on_usb, g_settings.pw_wake_lift,
             g_settings.pw_wake_needs_you, g_settings.kbd_needs_light, g_settings.kbd_off_cap, g_settings.pw_radio_off,
             g_settings.pw_radio_off_min, g_settings.pw_checkin_min, g_settings.pw_light_sleep,
             g_settings.pw_deep_idle_min, g_settings.pw_hibernate_h, g_settings.pw_hibernate_checkin_h,
             g_settings.ss_disable_in_ssh);
}

void power_sim_battery(int pct)
{
    s_sim_batt = pct < 0 ? -1 : pct > 100 ? 100 : pct;
    notify();
}

void power_sim_idle(int seconds)
{
    taskENTER_CRITICAL(&s_mux);
    s_last_input_ms -= (int64_t)seconds * 1000;
    if (s_state >= PS_OFF) s_off_since_ms -= (int64_t)seconds * 1000;
    taskEXIT_CRITICAL(&s_mux);
    notify();
}

static void vptr_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_vpt;
    data->state = s_vstate;
}

bool power_sim_touch(void)
{
    dlock(__LINE__);
    if (!s_vptr) {
        s_vptr = lv_indev_create();
        lv_indev_set_type(s_vptr, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_vptr, vptr_read);
        lv_indev_set_mode(s_vptr, LV_INDEV_MODE_EVENT);   /* read only when asked below */
        lv_indev_add_event_cb(s_vptr, indev_cb, LV_EVENT_ALL, NULL);
    }
    int32_t x, y;
    if (!ui_dashboard_selected_center(&x, &y)) {
        x = lv_display_get_horizontal_resolution(NULL) / 2;
        y = lv_display_get_vertical_resolution(NULL) / 2;
    }
    s_vpt.x = x; s_vpt.y = y;
    s_vstate = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_vptr);
    s_vstate = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_vptr);
    bsp_display_unlock();
    printf("touch: sim press and release at %ld,%ld\n", (long)x, (long)y);
    return true;
}

void power_touch_text(char *buf, size_t cap)
{
    snprintf(buf, cap, "touch: poll %s, reads=%lu errors=%lu last=%s (board v%d)", s_poll_on ? "on" : "off",
             (unsigned long)s_poll_reads, (unsigned long)s_poll_errors, s_poll_last, display_ctl_board_version());
}

void power_touch_poll_pause(bool pause)
{
    s_poll_paused = pause;   /* the power task derives s_poll_on from it */
    notify();
}

void power_wakes_print(void)
{
    wake_rec_t w[WAKES_KEPT];
    int n, head;
    taskENTER_CRITICAL(&s_mux);
    memcpy(w, s_wakes, sizeof(w));
    n = s_wakes_n;
    head = s_wakes_head;
    taskEXIT_CRITICAL(&s_mux);
    int64_t now = now_ms();
    if (!n) { printf("wakes: none\n"); return; }
    for (int i = 0; i < n; i++) {
        const wake_rec_t *r = &w[(head - n + i + WAKES_KEPT) % WAKES_KEPT];
        printf("%.1f %s swallowed=%u\n", (now - r->at_ms) / 1000.0, power_wake_name((power_wake_t)r->why), r->swallowed);
    }
}

/* ---- Hibernate stubs (Task 6) -------------------------------------------- */

bool hibernate_supported(void) { return false; }

void hibernate_now(void)
{
    ESP_LOGW(TAG, "hibernate: not built");
}

power_boot_t hibernate_boot_kind(void) { return POWER_BOOT_NORMAL; }

/* ---- init ---------------------------------------------------------------- */

void power_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    pr_idset_t *ids = heap_caps_calloc(4, sizeof(pr_idset_t), MALLOC_CAP_SPIRAM);
    if (!ids) ids = calloc(4, sizeof(pr_idset_t));
    if (!ids) { ESP_LOGE(TAG, "no memory for the power owner"); return; }
    s_now_ids = &ids[0]; s_armed = &ids[1]; s_poll_ids = &ids[2]; s_step_ids = &ids[3];
    s_last_input_ms = now_ms();
    keyboard_set_led_source(led_source);
    cc_poll_subscribe(on_sessions, NULL);
    dlock(__LINE__);
    lv_indev_t *tp = bsp_display_get_input_dev();
    if (tp) lv_indev_add_event_cb(tp, indev_cb, LV_EVENT_ALL, NULL);
    bsp_display_unlock();
    if (xTaskCreate(power_task, "power", 4096, NULL, 3, &s_task) != pdPASS)
        ESP_LOGE(TAG, "no memory for the power task");
}
