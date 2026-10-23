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
#include "settings/settings.h"
#include "ui/ui.h"

/* Battery bench: runs a fixed list of steps, one log tag each (0x10+n), and keeps the summaries in NVS
 * (ns "power", key "bench") so they survive a brownout reset. */

static const char *TAG = "bench";

#define BENCH_MAX 16
#define BATTERY_WAIT_S 5
#define FAST_STEP_S 3
#define BATTERY_TIMEOUT_S 600

static bool step_always(void) { return true; }

/* Off needs Task 5's power state: until then these steps are skipped. */
static bool step_off_light_amber(void) { return false; }
static bool step_off_light_off(void) { return false; }

/* Tasks 5, 6 and 8 add their steps here. */
static const bench_step_t k_steps[] = {
    { "settle", 60, step_always, NULL },
    { "active", 120, step_always, NULL },
    { "off-light-amber", 90, step_off_light_amber, NULL },
    { "off-light-off", 90, step_off_light_off, NULL },
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
    for (size_t i = 0; i < STEP_COUNT && !s_abort; i++) {
        const bench_step_t *st = &k_steps[i];
        s_rows[i].seconds = s_fast ? FAST_STEP_S : st->seconds;
        s_step = (int)i;
        s_usb_seen = false;
        power_log_sum_reset((uint8_t)(0x10 + i));
        if (st->enter && !st->enter()) {
            s_rows[i].status = ST_SKIPPED;
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
    ESP_LOGI(TAG, "bench %s%s%s", done ? "done" : "aborted", s_reason ? ": " : "", s_reason ? s_reason : "");
    if (done) {
        /* Back to Active is Task 5's job; until then the device is simply idle again. */
        bsp_display_lock(0);
        ui_toast("Bench done. Plug USB back in.");
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

void power_bench_start(bool fast)
{
    if (s_running) { printf("bench: already running\n"); return; }
    if (!sysinfo_ina226_present()) { printf("bench: aborted: no INA226, nothing to measure\n"); return; }
    s_reason = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    save_rows();
    s_fast = fast;
    s_abort = false;
    s_running = true;
    if (xTaskCreate(bench_task, "bench", 4096, NULL, 2, NULL) != pdPASS) {
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
