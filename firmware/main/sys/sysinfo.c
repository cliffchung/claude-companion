#include <time.h>
#include "sysinfo.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"
#include "settings/settings.h"
#include "i18n/i18n.h"
#include "net/wifi_mgr.h"

static const char *TAG = "sysinfo";
#define INA226_ADDR 0x41
#define RX8130_ADDR 0x32

static i2c_master_dev_handle_t s_ina = NULL, s_rtc = NULL;
static power_info_t s_pwr;
static bool s_charge_stat_valid = false;
static int s_last_pin = -1;
static vitals_batt_t s_batt;
static int64_t s_batt_us;
static vitals_src_t s_src;


static bool i2c_wr_rd(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    if (!dev) return false;
    esp_err_t e = r ? i2c_master_transmit_receive(dev, w, wl, r, rl, 50) : i2c_master_transmit(dev, w, wl, 50);
    return e == ESP_OK;
}

static bool ina_read16(uint8_t reg, uint16_t *v)
{
    uint8_t r[2];
    if (!i2c_wr_rd(s_ina, &reg, 1, r, 2)) return false;
    *v = (r[0] << 8) | r[1];
    return true;
}

static void ina_config(uint16_t cfg);

void sysinfo_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGW(TAG, "no I2C bus"); return; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA226_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_probe(bus, INA226_ADDR, 50) == ESP_OK && i2c_master_bus_add_device(bus, &cfg, &s_ina) == ESP_OK) {
        /* config: avg 16, 1.1ms conversions, continuous shunt+bus (also undoes a probe's slow mode and a
           Hibernate's power-down) */
        ina_config(0x4527);
        /* calibration for 5mR shunt: CAL = 0.00512 / (LSB * R); LSB = 1mA -> 1024 */
        uint8_t c[3] = { 0x05, 0x04, 0x00 };
        i2c_wr_rd(s_ina, c, 3, NULL, 0);
        ESP_LOGI(TAG, "INA226 ready");
    } else {
        ESP_LOGW(TAG, "INA226 not found");
    }
    cfg.device_address = RX8130_ADDR;
    if (i2c_master_probe(bus, RX8130_ADDR, 50) == ESP_OK && i2c_master_bus_add_device(bus, &cfg, &s_rtc) == ESP_OK) {
        ESP_LOGI(TAG, "RX8130 ready");
    } else {
        ESP_LOGW(TAG, "RX8130 not found");
    }
    sysinfo_apply_timezone();
    sysinfo_time_from_rtc();
    sysinfo_poll();
}

bool sysinfo_ina226_present(void) { return s_ina != NULL; }

static void ina_config(uint16_t cfg)
{
    uint8_t w[3] = { 0x00, (uint8_t)(cfg >> 8), (uint8_t)cfg };
    if (!i2c_wr_rd(s_ina, w, 3, NULL, 0)) ESP_LOGW(TAG, "INA226 config 0x%04x failed", cfg);
}

void sysinfo_ina226_slow(bool slow) { ina_config(slow ? 0x4E3F : 0x4527); }
void sysinfo_ina226_power_down(void) { ina_config(0x4520); }

bool sysinfo_ina226_raw(uint16_t *config, uint16_t *bus, int16_t *shunt)
{
    i2c_master_bus_handle_t b = bsp_i2c_get_handle();
    if (!b || i2c_master_probe(b, INA226_ADDR, 50) != ESP_OK) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA226_ADDR, .scl_speed_hz = 400000 };
    i2c_master_dev_handle_t d = NULL;
    if (i2c_master_bus_add_device(b, &cfg, &d) != ESP_OK) return false;
    uint8_t r[2], reg;
    bool ok = true;
    reg = 0x00; ok = ok && i2c_wr_rd(d, &reg, 1, r, 2); *config = (uint16_t)(r[0] << 8 | r[1]);
    reg = 0x02; ok = ok && i2c_wr_rd(d, &reg, 1, r, 2); *bus = (uint16_t)(r[0] << 8 | r[1]);
    reg = 0x01; ok = ok && i2c_wr_rd(d, &reg, 1, r, 2); *shunt = (int16_t)(r[0] << 8 | r[1]);
    i2c_master_bus_rm_device(d);
    return ok;
}

bool sysinfo_ina226_oneshot(int *mv, int *ma)
{
    i2c_master_bus_handle_t b = bsp_i2c_get_handle();
    if (!b) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA226_ADDR, .scl_speed_hz = 400000 };
    i2c_master_dev_handle_t d = NULL;
    if (i2c_master_bus_add_device(b, &cfg, &d) != ESP_OK) return false;
    /* AVG 4 (001), VBUSCT and VSHCT 1.1 ms (100), mode 011: shunt and bus, triggered */
    const uint8_t trig[3] = { 0x00, 0x43, 0x23 };
    bool ok = i2c_wr_rd(d, trig, 3, NULL, 0);
    uint8_t r[2], reg;
    bool ready = false;
    for (int i = 0; ok && i < 10 && !ready; i++) {   /* CVRF (Mask/Enable bit 3): up to 20 ms */
        vTaskDelay(pdMS_TO_TICKS(2));
        reg = 0x06;
        ready = i2c_wr_rd(d, &reg, 1, r, 2) && (r[1] & 0x08);
    }
    int16_t shunt = 0;
    uint16_t bus = 0;
    if (ready) {
        reg = 0x01; ok = i2c_wr_rd(d, &reg, 1, r, 2); shunt = (int16_t)(r[0] << 8 | r[1]);
        reg = 0x02; ok = ok && i2c_wr_rd(d, &reg, 1, r, 2); bus = (uint16_t)(r[0] << 8 | r[1]);
    }
    const uint8_t down[3] = { 0x00, 0x45, 0x20 };   /* power-down, as hibernate left it */
    i2c_wr_rd(d, down, 3, NULL, 0);
    i2c_master_bus_rm_device(d);
    if (!ok || !ready) return false;
    *mv = (int)bus * 5 / 4;          /* 1.25 mV */
    *ma = (int)shunt / 2;            /* 2.5 uV / 5 mR = 0.5 mA */
    return true;
}

bool sysinfo_rtc_present(void) { return s_rtc != NULL; }

int sysinfo_charge_pin(void)
{
    esp_io_expander_handle_t ex2 = bsp_io_expander1_init();
    uint32_t lv = 0;
    /* CHG_STAT (the IP2326's LED output) is expander 2 pin 6 on the Tab5 schematic; pin 4, which the fork
       read, is PWROFF_PULSE and reads 0 whatever the supply (Task 6 notes) */
    if (!ex2 || esp_io_expander_get_level(ex2, IO_EXPANDER_PIN_NUM_6, &lv) != ESP_OK) return -1;
    return lv ? 1 : 0;
}

int sysinfo_charge_pin_last(void) { return s_last_pin; }

bool sysinfo_charging(void)
{
    return s_pwr.valid && vitals_charging((int)lroundf(s_pwr.current_a * 1000.0f));
}

void sysinfo_poll(void)
{
    if (!s_ina) { s_pwr.valid = false; s_pwr.percent = -1; s_pwr.src = PWR_UNKNOWN; memset(&s_src, 0, sizeof(s_src)); return; }
    uint16_t bus, cur, pwr;
    if (!ina_read16(0x02, &bus) || !ina_read16(0x04, &cur) || !ina_read16(0x03, &pwr)) { s_pwr.valid = false; return; }
    s_pwr.bus_v = bus * 1.25f / 1000.0f;
    s_pwr.current_a = (int16_t)cur * 0.001f;
    s_pwr.power_w = pwr * 0.025f;
    int64_t now = esp_timer_get_time();
    float dt = s_batt_us ? (now - s_batt_us) / 1e6f : 0.0f;
    s_batt_us = now;
    s_pwr.percent = vitals_batt_update(&s_batt, s_pwr.bus_v, s_pwr.current_a, dt);
    s_pwr.valid = true;
    /* CHG_STAT (IO expander 2 pin 6) for diagnostics only: it reads 0 whether or not the pack charges, so the
       source comes from the INA226 alone (PT23) */
    int pin = sysinfo_charge_pin();
    s_last_pin = pin;
    if (pin >= 0) s_charge_stat_valid = true;
    s_pwr.src = vitals_src_step(&s_src, (int)(s_pwr.bus_v * 1000.0f + 0.5f), (int)lroundf(s_pwr.current_a * 1000.0f));
}

power_info_t sysinfo_power(void) { return s_pwr; }
int sysinfo_rssi_cached(void)
{
    static int s_rssi;
    static int64_t s_at;
    int64_t now = esp_timer_get_time();
    if (!s_at || now - s_at >= 10 * 1000000LL) {
        s_rssi = wifi_mgr_rssi();
        s_at = now;
        int64_t took = esp_timer_get_time() - now;
        if (took > 20000) ESP_LOGW(TAG, "rssi read took %d us", (int)took);
    }
    return s_rssi;
}

int sysinfo_battery_percent(void) { return s_pwr.valid ? s_pwr.percent : -1; }

const char *sysinfo_power_src_text(void)
{
    switch (s_pwr.src) {
    case PWR_USB: return tr(STR_PWR_USB_C);
    case PWR_USB_CHARGING: return tr(STR_PWR_USB_C_CHARGE);
    case PWR_USB_STANDBY: return tr(STR_PWR_USB_C_STANDBY);
    case PWR_BATTERY: return tr(STR_PWR_BATTERY);
    default: return tr(STR_UNKNOWN);
    }
}

/* newlib on ESP-IDF has no timegm(); days-from-civil algorithm. */
static time_t utc_mktime(const struct tm *t)
{
    int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
    if (m <= 2) { y--; m += 12; }
    long days = 365L * y + y / 4 - y / 100 + y / 400 + (153L * (m - 3) + 2) / 5 + d - 719469L;
    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static int bcd2i(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t i2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool sysinfo_rtc_read(struct tm *out)
{
    uint8_t reg = 0x10, r[7];
    if (!i2c_wr_rd(s_rtc, &reg, 1, r, 7)) return false;
    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd2i(r[0] & 0x7F);
    out->tm_min = bcd2i(r[1] & 0x7F);
    out->tm_hour = bcd2i(r[2] & 0x3F);
    out->tm_mday = bcd2i(r[4] & 0x3F);
    out->tm_mon = bcd2i(r[5] & 0x1F) - 1;
    out->tm_year = bcd2i(r[6]) + 100;
    return out->tm_year >= 124 && out->tm_mon >= 0 && out->tm_mon < 12 && out->tm_mday >= 1;
}

bool sysinfo_rtc_write(const struct tm *in)
{
    uint8_t w[8] = { 0x10, i2bcd(in->tm_sec), i2bcd(in->tm_min), i2bcd(in->tm_hour),
                     (uint8_t)(1 << in->tm_wday), i2bcd(in->tm_mday), i2bcd(in->tm_mon + 1), i2bcd(in->tm_year % 100) };
    return i2c_wr_rd(s_rtc, w, 8, NULL, 0);
}

void sysinfo_apply_timezone(void)
{
    setenv("TZ", vitals_tz_valid(g_settings.tz_rule) ? g_settings.tz_rule : TZ_DEFAULT, 1);
    tzset();
}

void sysinfo_time_from_rtc(void)
{
    struct tm tm;
    if (!sysinfo_rtc_read(&tm)) return;
    /* RTC keeps UTC */
    time_t t = utc_mktime(&tm);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

bool sysinfo_rtc_epoch_raw(time_t *out)
{
    i2c_master_bus_handle_t b = bsp_i2c_get_handle();
    if (!b) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RX8130_ADDR, .scl_speed_hz = 400000 };
    i2c_master_dev_handle_t d = NULL;
    if (i2c_master_bus_add_device(b, &cfg, &d) != ESP_OK) return false;
    i2c_master_dev_handle_t keep = s_rtc;
    s_rtc = d;   /* sysinfo_rtc_read through the temporary device */
    struct tm tm;
    bool ok = sysinfo_rtc_read(&tm);
    s_rtc = keep;
    i2c_master_bus_rm_device(d);
    if (ok) *out = utc_mktime(&tm);
    return ok;
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    sysinfo_rtc_write(&tm);
    ESP_LOGI(TAG, "time synced from NTP, RTC updated");
}

void sysinfo_start_sntp(void)
{
    static bool started = false;
    if (started) { esp_netif_sntp_start(); return; }
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_sync_cb;
    cfg.start = true;
    esp_netif_sntp_init(&cfg);
}

uint32_t sysinfo_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }
const char *sysinfo_app_version(void) { return esp_app_get_description()->version; }
const char *sysinfo_app_project(void) { return esp_app_get_description()->project_name; }

void sysinfo_mem(size_t *dram_free, size_t *dram_total, size_t *dma_free, size_t *dma_total,
                 size_t *psram_free, size_t *psram_total)
{
    if (dram_free) *dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dram_total) *dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dma_free) *dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (dma_total) *dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    if (psram_free) *psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_total) *psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}
