#include "power_bench.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "sys/sysinfo.h"
#include "sys/power_log.h"
#include "sys/power.h"
#include "sys/display_ctl.h"
#include "esp_lcd_touch.h"
#include "settings/settings.h"
#include "ui/ui.h"
#include "sys/hibernate.h"
#include "sys/hib_rules.h"
#include "input/keyboard.h"
#include "esp_timer.h"
#include "companion/cc_link.h"

/* Battery bench: runs a fixed list of steps, one log tag each (0x10+n), and keeps the summaries in NVS
 * (ns "power", key "bench") so they survive a brownout reset. */

static const char *TAG = "bench";

#define BENCH_MAX 16
#define BATTERY_WAIT_S 5
#define FAST_STEP_S 3
#define BATTERY_TIMEOUT_S 600

/* Each step sets the state it measures; power_go pins it until a wake (or the next step). */
static bool step_active(void) { power_go(PS_ACTIVE); power_hold(POWER_HOLD_BENCH, true); return true; }
static void leave_active(void) { power_hold(POWER_HOLD_BENCH, false); }
static bool step_dim(void) { power_go(PS_DIM); return true; }
/* Off as it ships: on v2 the 10 Hz touch poll runs */
static bool step_off_touch_poll(void)
{
    if (display_ctl_board_version() == 1) return false;   /* v1 wakes from the GT911 INT: no poll to measure */
    power_go(PS_OFF);
    return true;
}
/* Off with the poll paused: the difference from off-touch-poll is the poll's cost */
static bool step_off_touch_awake(void) { power_touch_poll_pause(true); power_go(PS_OFF); return true; }
static void leave_touch_paused(void) { power_touch_poll_pause(false); }
/* Off with the touch controller asleep (never shipped: a sleeping controller may not wake) */
static bool step_off_touch_asleep(void)
{
    power_touch_poll_pause(true);
    power_go(PS_OFF);
    vTaskDelay(pdMS_TO_TICKS(300));   /* the power task has paused LVGL's touch read */
    esp_lcd_touch_handle_t tp = display_ctl_touch();
    esp_err_t e = tp ? esp_lcd_touch_enter_sleep(tp) : ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "touch enter_sleep: %s", esp_err_to_name(e));
    return true;
}
static void leave_touch_asleep(void)
{
    esp_lcd_touch_handle_t tp = display_ctl_touch();
    esp_err_t e = tp ? esp_lcd_touch_exit_sleep(tp) : ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(50));
    bool ok = tp && esp_lcd_touch_read_data(tp) == ESP_OK;
    ESP_LOGI(TAG, "touch exit_sleep: %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "touch after exit_sleep: %s", ok ? "ok" : "dead");
    power_touch_poll_pause(false);
}
static bool step_off_light_amber(void) { power_bench_light(POWER_BENCH_LIGHT_AMBER); power_go(PS_OFF); return true; }
static bool step_off_light_off(void) { power_bench_light(POWER_BENCH_LIGHT_OFF); power_go(PS_OFF); return true; }
static void leave_light(void) { power_bench_light(POWER_BENCH_LIGHT_NONE); }

/* ---- probe steps (Task 6): the device sleeps, each boot is one sample ---- */
#define PROBE_CYCLES 3
#define PROBE_SLEEP_S 60
#define PROBE_SLEEP_FAST_S 10
typedef struct { uint8_t row, left, fast, done, resume; } probe_rec_t;   /* resume: the row the bench goes on at, 0xFF none */
typedef enum { PROBE_FLOOR, PROBE_LIGHT, PROBE_HIB } probe_kind_t;
static bool probe_start(probe_kind_t k);
static bool step_floor(void) { return probe_start(PROBE_FLOOR); }
/* PT16: only where the keyboard keeps its power and light through deep sleep */
static bool step_hibernate_light(void) { return hibernate_kbd_rail_survives() && probe_start(PROBE_LIGHT); }
static bool step_hibernate(void) { return hibernate_mode() != HIB_MODE_NONE && probe_start(PROBE_HIB); }

/* Task 8 adds its steps here, before `floor`; `hibernate` stays last. */
static const bench_step_t k_steps[] = {
    { "settle", 60, step_active, leave_active },
    { "active", 120, step_active, leave_active },
    { "dim", 90, step_dim, NULL },
    { "off-touch-poll", 120, step_off_touch_poll, NULL },
    { "off-touch-awake", 120, step_off_touch_awake, leave_touch_paused },
    { "off-touch-asleep", 120, step_off_touch_asleep, leave_touch_asleep },
    { "off-light-amber", 90, step_off_light_amber, leave_light },
    { "off-light-off", 90, step_off_light_off, leave_light },
    { "hibernate-light", PROBE_SLEEP_S, step_hibernate_light, NULL },
    { "floor", PROBE_SLEEP_S, step_floor, NULL },
    { "hibernate", PROBE_SLEEP_S, step_hibernate, NULL },
};
#define STEP_COUNT (sizeof(k_steps) / sizeof(k_steps[0]))
_Static_assert(STEP_COUNT <= BENCH_MAX, "too many bench steps: rows and log tag slots hold 16");

/* ST_RUNNING is written when a step starts: after a reset it is reported "interrupted". */
typedef enum { ST_PENDING = 0, ST_OK, ST_SKIPPED, ST_ABORTED, ST_RUNNING, ST_USB, ST_NOSAMPLES } step_status_t;
typedef struct { uint8_t status; uint16_t seconds; plog_sum_t sum; } bench_row_t;

static bench_row_t s_rows[BENCH_MAX];
static volatile bool s_running, s_abort;
static volatile int s_step = -1;
static bool s_fast;
static bool s_usb_seen;
static const char *s_reason;   /* why the last run stopped early */
static int s_first;            /* the step a run starts at (0, or where a resumed run goes on) */

static const char *status_text(uint8_t st)
{
    switch (st) {
    case ST_OK: return "ok";
    case ST_SKIPPED: return "skipped: not built";
    case ST_ABORTED: return "aborted";
    case ST_RUNNING: return "interrupted";   /* only seen when no bench task is running */
    case ST_USB: return "usb";
    case ST_NOSAMPLES: return "no samples";
    default: return "pending";
    }
}

static void save_rows(void) { nvs_set_blob_ns("power", "bench", s_rows, sizeof(s_rows)); }

static void load_rows(void)
{
    void *blob = NULL;
    size_t len = 0;
    memset(s_rows, 0, sizeof(s_rows));
    if (nvs_get_blob_alloc("power", "bench", &blob, &len)) {
        if (len == sizeof(s_rows)) memcpy(s_rows, blob, len);
        free(blob);
    }
}

uint8_t power_bench_tag(void) { return s_running && s_step >= 0 ? (uint8_t)(0x10 + s_step) : 0; }

/* Sleeps up to secs seconds in 1 s slices; false if aborted. */
static bool wait_s(int secs)
{
    for (int i = 0; i < secs; i++) {
        if (s_abort) return false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* USB mid-step: keep going (the rest of the run is still useful) but the step won't be "ok" */
        if (!s_fast && (sysinfo_power().current_a < 0 || sysinfo_charge_pin_last() == 0)) s_usb_seen = true;
    }
    return !s_abort;
}

static void bench_fail(const char *why)
{
    s_reason = why;
    s_abort = true;
    ESP_LOGE(TAG, "bench stopped: %s", why);
}

static void bench_task(void *arg)
{
    (void)arg;
    bool log_was_on = power_log_on();
    power_log_accept_charging(s_fast);   /* the fast scaffold check runs on USB */
    power_log_enable(true);
    if (!power_log_on()) bench_fail("the power log can't start");
    if (!s_fast && !s_abort) {
        ESP_LOGI(TAG, "waiting for battery power (unplug USB)");
        int on_batt = 0, waited = 0;
        while (on_batt < BATTERY_WAIT_S && !s_abort) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            on_batt = sysinfo_power().src == PWR_BATTERY ? on_batt + 1 : 0;
            if (++waited >= BATTERY_TIMEOUT_S) bench_fail("no battery power within 10 minutes");
        }
    }
    if (s_first) vTaskDelay(pdMS_TO_TICKS(5000));   /* a resumed run: the power task and keyboard are up */
    for (size_t i = (size_t)s_first; i < STEP_COUNT && !s_abort; i++) {
        const bench_step_t *st = &k_steps[i];
        s_rows[i].seconds = s_fast ? FAST_STEP_S : st->seconds;
        s_step = (int)i;
        s_usb_seen = false;
        power_log_sum_reset((uint8_t)(0x10 + i));
        if (st->enter && !st->enter()) {
            /* a probe step that got as far as "running" and came back failed to sleep: not "not built" */
            s_rows[i].status = s_rows[i].status == ST_RUNNING ? ST_ABORTED : ST_SKIPPED;
            save_rows();
            continue;
        }
        s_rows[i].status = ST_RUNNING;
        save_rows();
        ESP_LOGI(TAG, "step %u %s: %us", (unsigned)i, st->name, (unsigned)s_rows[i].seconds);
        bool ran = wait_s(s_rows[i].seconds);
        if (st->leave) st->leave();
        power_log_sum_get((uint8_t)(0x10 + i), &s_rows[i].sum);
        if (!ran) s_rows[i].status = ST_ABORTED;
        else {
            plog_step_verdict_t v = plog_step_verdict(&s_rows[i].sum, s_usb_seen);
            s_rows[i].status = v == PLOG_STEP_OK ? ST_OK : v == PLOG_STEP_USB ? ST_USB : ST_NOSAMPLES;
        }
        save_rows();
    }
    bool done = !s_abort;
    s_step = -1;
    power_log_accept_charging(false);
    power_log_enable(log_was_on);
    s_running = false;
    ESP_LOGI(TAG, "bench %s%s%s (stack free %u B of 6144)", done ? "done" : "aborted", s_reason ? ": " : "",
             s_reason ? s_reason : "", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    power_go(PS_ACTIVE);
    if (done) {
        bsp_display_lock(0);
        ui_toast("Bench done. Plug USB back in.");
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static int step_index(const char *name)
{
    for (size_t i = 0; i < STEP_COUNT; i++) if (strcmp(k_steps[i].name, name) == 0) return (int)i;
    return -1;
}

/* On the bench task: the light off, the INA226 slow, the probe record, then down. Returns only on failure. */
static bool probe_start(probe_kind_t k)
{
    int row = s_step;
    power_bench_light(POWER_BENCH_LIGHT_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));   /* the power task writes the light */
    sysinfo_ina226_slow(true);
    probe_rec_t r = { .row = (uint8_t)row, .left = PROBE_CYCLES, .fast = s_fast, .resume = 0xFF };
    nvs_set_blob_ns("power", "probe", &r, sizeof(r));
    s_rows[row].status = ST_RUNNING;
    s_rows[row].seconds = s_fast ? PROBE_SLEEP_FAST_S : PROBE_SLEEP_S;
    save_rows();
    ESP_LOGI(TAG, "step %d %s: %d cycles of %u s", row, k_steps[row].name, PROBE_CYCLES, (unsigned)s_rows[row].seconds);
    uint32_t s = s_rows[row].seconds;
    if (k == PROBE_LIGHT) {
        /* amber at the Deep-idle 30% cap, written last and held: the power task can't change it now */
        keyboard_led_lock();
        keyboard_write_led(1, 30, 0xf4, 0xb5, 0x45);
        hibernate_deep_sleep(s, POWER_BOOT_HIB_PROBE);
        keyboard_led_unlock();
    } else if (k == PROBE_FLOOR) {
        hibernate_deep_sleep(s, POWER_BOOT_HIB_PROBE);
    } else {
        hibernate_now(s, POWER_BOOT_HIB_PROBE);   /* a timed hibernate is a deep sleep (hibernate.c) */
    }
    sysinfo_ina226_slow(false);
    nvs_erase_key_ns("power", "probe");
    return false;
}

static bool probe_get(probe_rec_t *r)
{
    void *blob = NULL;
    size_t len = 0;
    bool ok = nvs_get_blob_alloc("power", "probe", &blob, &len) && len == sizeof(*r);
    if (ok) memcpy(r, blob, sizeof(*r));
    free(blob);
    return ok;
}

/* Early boot (hibernate_boot_early), before the rails and the log: NVS only. */
power_bench_probe_t power_bench_probe_boot(bool have, uint16_t bus_mv, int16_t shunt_raw, uint32_t *sleep_s, bool *deep)
{
    probe_rec_t r;
    if (!probe_get(&r) || r.row >= STEP_COUNT || r.done) return POWER_BENCH_PROBE_DONE;
    load_rows();
    bench_row_t *row = &s_rows[r.row];
    int64_t at = esp_timer_get_time() / 1000;
    if (have) {
        int32_t ma10 = shunt_raw * 5;   /* 0.1 mA */
        uint32_t mw = (uint32_t)bus_mv * (uint32_t)(shunt_raw < 0 ? -shunt_raw : shunt_raw) / 2000;
        plog_sample_t x = { .t_s = (uint32_t)(PROBE_CYCLES - r.left), .mv = bus_mv,
                            .ma = (int16_t)((ma10 + (ma10 < 0 ? -5 : 5)) / 10), .mw = (uint16_t)(mw > 65535 ? 65535 : mw),
                            .tag = (uint8_t)(0x10 + r.row) };
        plog_sum_add(&row->sum, &x);
        ESP_LOGI(TAG, "hib probe: %u mV %ld.%ld mA at boot+%lld ms", bus_mv, (long)(ma10 / 10), (long)(labs(ma10) % 10), (long long)at);
    } else {
        ESP_LOGW(TAG, "hib probe: no sample (the INA226 lost its config: it was unpowered) at boot+%lld ms", (long long)at);
    }
    if (r.left) r.left--;
    bool is_floor = strcmp(k_steps[r.row].name, "floor") == 0;
    if (r.left == 0) {
        hib_probe_verdict_t v = hib_probe_verdict(row->sum.n, r.fast, row->sum.sum_ma);
        row->status = v == HIB_PROBE_NO_SAMPLES ? ST_NOSAMPLES : v == HIB_PROBE_USB ? ST_USB : ST_OK;
        int hib = step_index("hibernate");
        if (strcmp(k_steps[r.row].name, "hibernate-light") == 0) {
            /* the light has to go off for `floor`, and only a running keyboard driver can do that: boot on
               and let power_bench_boot() restart the bench there */
            r.done = 1;
            r.resume = (uint8_t)(r.row + 1);
        } else if (is_floor && hib >= 0 && hibernate_mode() != HIB_MODE_NONE) {
            /* straight into the hibernate step's first cycle, the rails still cut */
            r.row = (uint8_t)hib;
            r.left = PROBE_CYCLES;
            s_rows[hib].status = ST_RUNNING;
            s_rows[hib].seconds = r.fast ? PROBE_SLEEP_FAST_S : PROBE_SLEEP_S;
            is_floor = false;
        } else {
            r.done = 1;   /* power_bench_boot() finishes once the link is live */
        }
    }
    if (!nvs_set_blob_ns("power", "probe", &r, sizeof(r))) {
        /* M3: without the record every boot would re-read the same cycles left and sleep again */
        row->status = ST_ABORTED;
        save_rows();
        nvs_erase_key_ns("power", "probe");
        ESP_LOGE(TAG, "hib probe: the probe record didn't save: row aborted, booting on");
        return POWER_BENCH_PROBE_DONE;
    }
    save_rows();
    if (r.done) return POWER_BENCH_PROBE_DONE;
    *sleep_s = r.fast ? PROBE_SLEEP_FAST_S : PROBE_SLEEP_S;
    *deep = true;   /* every probe cycle is timed, so a deep sleep (hibernate.c) */
    return POWER_BENCH_PROBE_SLEEP;
}

static void bench_boot_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 600 && cc_poll_generation() == 0; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    load_rows();
    int hib = step_index("hibernate");
    if (hib >= 0 && cc_poll_generation() != 0) s_rows[hib].seconds = (uint16_t)((ms + 500) / 1000);   /* boot to live */
    save_rows();
    nvs_erase_key_ns("power", "probe");
    ESP_LOGI(TAG, "bench done: boot to live %u ms", (unsigned)ms);
    bsp_display_lock(0);
    ui_toast("Bench done. Plug USB back in.");
    bsp_display_unlock();
    ESP_LOGI(TAG, "bench_boot: stack free %u B of 4096", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

void power_bench_boot(void)
{
    probe_rec_t r;
    if (!probe_get(&r)) return;
    if (!r.done) { nvs_erase_key_ns("power", "probe"); return; }   /* a probe cut short: its row reads "interrupted" */
    if (r.resume < STEP_COUNT) {
        nvs_erase_key_ns("power", "probe");
        load_rows();
        s_first = r.resume;
        s_fast = r.fast;
        s_abort = false;
        s_running = true;
        ESP_LOGI(TAG, "bench: going on at step %d", s_first);
        if (xTaskCreate(bench_task, "bench", 6144, NULL, 2, NULL) != pdPASS) s_running = false;
        return;
    }
    if (xTaskCreate(bench_boot_task, "bench_boot", 4096, NULL, 2, NULL) != pdPASS) nvs_erase_key_ns("power", "probe");
}

void power_bench_start(bool fast)
{
    if (s_running) { printf("bench: already running\n"); return; }
    if (!sysinfo_ina226_present()) { printf("bench: aborted: no INA226, nothing to measure\n"); return; }
    s_reason = NULL;
    s_first = 0;
    memset(s_rows, 0, sizeof(s_rows));
    save_rows();
    s_fast = fast;
    s_abort = false;
    s_running = true;
    if (xTaskCreate(bench_task, "bench", 6144, NULL, 2, NULL) != pdPASS) {
        s_running = false;
        printf("bench: no memory for the task\n");
        return;
    }
    printf("bench: started%s (%u steps)\n", fast ? " (fast)" : "", (unsigned)STEP_COUNT);
}

void power_bench_abort(void)
{
    if (!s_running) { printf("bench: not running\n"); return; }
    s_abort = true;
    printf("bench: aborting\n");
}

void power_bench_status(void)
{
    if (s_running)
        printf("bench: running step %d/%u (%s)\n", s_step + 1, (unsigned)STEP_COUNT,
               s_step >= 0 ? k_steps[s_step].name : "waiting for battery");
    else
        printf("bench: idle%s%s\n", s_reason ? ", last run stopped: " : "", s_reason ? s_reason : "");
}

void power_bench_report(void)
{
    if (!s_running) load_rows();
    printf("@@BENCH %u\n", (unsigned)STEP_COUNT);
    for (size_t i = 0; i < STEP_COUNT; i++) {
        bench_row_t row = s_rows[i];
        if (row.status == ST_RUNNING && !s_running) {
            /* a reset cut the step short: take what the log's NVS summary had for it */
            plog_sum_t part;
            if (row.sum.n == 0 && power_log_sum_get((uint8_t)(0x10 + i), &part) && part.n) row.sum = part;
        }
        const bench_row_t *r = &row;
        printf("%s,%u,%lu,%u,%u,%ld,%lu,%s\n", k_steps[i].name, r->seconds ? r->seconds : k_steps[i].seconds,
               (unsigned long)plog_sum_avg_mw(&r->sum), r->sum.n ? r->sum.min_mw : 0, r->sum.n ? r->sum.max_mw : 0,
               r->sum.n ? (long)(r->sum.sum_ma / (int64_t)r->sum.n) : 0L, (unsigned long)plog_sum_mwh_x1000(&r->sum),
               (r->status == ST_RUNNING && s_running && (int)i == s_step) ? "running" : status_text(r->status));
    }
    printf("@@END\n");
}
