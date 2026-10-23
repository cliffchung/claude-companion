#include "power_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/sysinfo.h"
#include "settings/settings.h"

static const char *TAG = "plog";

#define PLOG_CAP 65536
#define NVS_NS "power"
#define SAVE_EVERY_S 60
#define RST_MAX 16

/* One summary slot per tag in use: 0..3 power states, 0x10+n bench steps, 0x80|state check-ins. */
#define SLOT_STATE 0
#define SLOT_BENCH 4
#define SLOT_CHECKIN 20
#define SLOT_COUNT 24

static plog_sample_t *s_buf;
static plog_ring_t s_ring;
static plog_sum_t s_sum[SLOT_COUNT];
static bool s_on;
static bool s_dumping;       /* pushes pause while a dump runs, so row indices don't shift */
static bool s_accept_chg;    /* the fast bench runs on USB: let charging samples into the sums */
static uint32_t s_last_save_t;
static SemaphoreHandle_t s_mu;

static const char *const k_rst_names[RST_MAX] = {
    "unknown", "poweron", "external", "software", "panic", "int_wdt", "task_wdt", "wdt",
    "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup",
};

static int slot_of(uint8_t tag)
{
    if (tag <= 3) return SLOT_STATE + tag;
    if (tag >= 0x10 && tag <= 0x1F) return SLOT_BENCH + (tag - 0x10);
    if (tag >= 0x80 && tag <= 0x83) return SLOT_CHECKIN + (tag - 0x80);
    return -1;
}

static uint8_t tag_of(int slot)
{
    if (slot < SLOT_BENCH) return (uint8_t)(slot - SLOT_STATE);
    if (slot < SLOT_CHECKIN) return (uint8_t)(0x10 + slot - SLOT_BENCH);
    return (uint8_t)(0x80 + slot - SLOT_CHECKIN);
}

static void lock(void) { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

static void save_sums(void)
{
    nvs_set_blob_ns(NVS_NS, "sum", s_sum, sizeof(s_sum));
}

void power_log_boot(void)
{
    s_mu = xSemaphoreCreateMutex();
    esp_reset_reason_t r = esp_reset_reason();
    int i = (r >= 0 && r < RST_MAX) ? (int)r : 0;
    ESP_LOGI(TAG, "reset reason: %s", k_rst_names[i]);
    uint32_t cnt[RST_MAX] = {0};
    void *blob = NULL;
    size_t len = 0;
    if (nvs_get_blob_alloc(NVS_NS, "rst", &blob, &len)) {
        memcpy(cnt, blob, len < sizeof(cnt) ? len : sizeof(cnt));
        free(blob);
    }
    cnt[i]++;
    nvs_set_blob_ns(NVS_NS, "rst", cnt, sizeof(cnt));
    /* the previous run's summaries, kept so a brownout reset doesn't lose them */
    if (nvs_get_blob_alloc(NVS_NS, "sum", &blob, &len)) {
        if (len == sizeof(s_sum)) memcpy(s_sum, blob, len);
        free(blob);
    }
}

bool power_log_on(void) { return s_on; }

void power_log_enable(bool on)
{
    if (on == s_on) return;
    if (on) {
        lock();
        if (!s_buf) {
            s_buf = heap_caps_malloc(PLOG_CAP * sizeof(plog_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_buf) { unlock(); ESP_LOGE(TAG, "no PSRAM for the log"); return; }
            plog_init(&s_ring, s_buf, PLOG_CAP);
        }
        /* a new run: uptime restarts at 0 each boot, so old summaries can't be mixed in */
        memset(s_sum, 0, sizeof(s_sum));
        s_last_save_t = sysinfo_uptime_s();
        s_on = true;
        unlock();
    } else {
        lock();
        s_on = false;
        save_sums();
        unlock();
    }
}

void power_log_sample(uint8_t tag)
{
    if (!s_on) return;
    power_info_t p = sysinfo_power();
    if (!p.valid) return;
    int pin = sysinfo_charge_pin_last();   /* sysinfo_poll just read it */
    uint16_t mv = (uint16_t)lroundf(p.bus_v * 1000.0f);
    int16_t ma = (int16_t)lroundf(p.current_a * 1000.0f);
    bool charging = ma < 0 || pin == 0;
    plog_sample_t x = {
        .t_s = sysinfo_uptime_s(),
        .mv = mv,
        .ma = ma,
        .mw = plog_mw(mv, ma),
        .tag = tag,
        .flags = (uint8_t)((pin == 0 ? PLOG_F_PIN_LOW : 0) | (pin >= 0 ? PLOG_F_PIN_OK : 0) |
                           (charging ? PLOG_F_CHARGING : 0)),
    };
    lock();
    if (s_on) {
        if (!s_dumping) plog_push(&s_ring, &x);
        int slot = slot_of(tag);
        /* charge power isn't consumption: keep charging samples out of the battery summaries */
        if (slot >= 0 && (!charging || s_accept_chg)) plog_sum_add(&s_sum[slot], &x);
        if (x.t_s - s_last_save_t >= SAVE_EVERY_S) { s_last_save_t = x.t_s; save_sums(); }
    }
    unlock();
}

void power_log_accept_charging(bool on) { s_accept_chg = on; }

void power_log_dump(void)
{
    lock();
    if (s_dumping) { unlock(); printf("power log: a dump is already running\n"); return; }
    uint32_t n = s_buf ? s_ring.count : 0;
    s_dumping = true;
    unlock();
    printf("@@PLOG %lu\n", (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        plog_sample_t x;
        lock();
        bool ok = plog_get(&s_ring, i, &x);
        unlock();
        if (!ok) break;   /* cannot happen: pushes and clear are held off */
        printf("%lu,%u,%d,%u,%u,%u\n", (unsigned long)x.t_s, x.mv, x.ma, x.mw, x.tag, x.flags);
        if ((i & 0xFF) == 0xFF) vTaskDelay(1);   /* let the console task and the idle poll run */
    }
    printf("@@END\n");
    lock();
    s_dumping = false;
    unlock();
}

void power_log_clear(void)
{
    lock();
    if (s_dumping) { unlock(); printf("power log: not cleared, a dump is running\n"); return; }
    if (s_buf) plog_init(&s_ring, s_buf, PLOG_CAP);
    memset(s_sum, 0, sizeof(s_sum));
    nvs_erase_key_ns(NVS_NS, "sum");
    unlock();
}

void power_log_status(void)
{
    lock();
    printf("power log: %s, %lu/%d samples\n", s_on ? "on" : "off",
           (unsigned long)(s_buf ? s_ring.count : 0), PLOG_CAP);
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!s_sum[i].n) continue;
        printf("  tag 0x%02x: n=%lu avg=%lu mW min=%u max=%u mWh=%.2f\n", tag_of(i), (unsigned long)s_sum[i].n,
               (unsigned long)plog_sum_avg_mw(&s_sum[i]), s_sum[i].min_mw, s_sum[i].max_mw,
               plog_sum_mwh_x1000(&s_sum[i]) / 1000.0);
    }
    unlock();
}

bool power_log_sum_get(uint8_t tag, plog_sum_t *out)
{
    int slot = slot_of(tag);
    if (slot < 0) return false;
    lock();
    *out = s_sum[slot];
    unlock();
    return true;
}

void power_log_sum_reset(uint8_t tag)
{
    int slot = slot_of(tag);
    if (slot < 0) return;
    lock();
    memset(&s_sum[slot], 0, sizeof(s_sum[slot]));
    unlock();
}

void power_log_resets(void)
{
    uint32_t cnt[RST_MAX] = {0};
    void *blob = NULL;
    size_t len = 0;
    if (nvs_get_blob_alloc(NVS_NS, "rst", &blob, &len)) {
        memcpy(cnt, blob, len < sizeof(cnt) ? len : sizeof(cnt));
        free(blob);
    }
    printf("reset reasons since the counters were last cleared:\n");
    for (int i = 0; i < RST_MAX; i++)
        if (cnt[i]) printf("  %-10s %lu\n", k_rst_names[i], (unsigned long)cnt[i]);
}

void power_log_resets_clear(void)
{
    nvs_erase_key_ns(NVS_NS, "rst");
}
