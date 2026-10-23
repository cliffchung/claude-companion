#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "sys/vitals.h"      /* power_src_t */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float bus_v;        /* battery/system bus voltage */
    float current_a;    /* + = discharging, - = charging */
    float power_w;
    int percent;        /* -1 if unknown */
    power_src_t src;
    bool valid;
} power_info_t;

void sysinfo_init(void);                     /* INA226 + RX8130 on the BSP I2C bus */
void sysinfo_poll(void);                     /* refresh cached power info (~1 Hz) */
power_info_t sysinfo_power(void);
int sysinfo_charge_pin(void);                /* raw CHG_STAT, diagnostics only (PT23: reads 0 charging or not): -1 unreadable, 0, 1 */
int sysinfo_charge_pin_last(void);           /* the pin as read by the last sysinfo_poll */
bool sysinfo_charging(void);                 /* the last sample's charge current is under -5 mA (vitals_charging) */
int sysinfo_rssi_cached(void);               /* wifi_mgr_rssi at most every 10 s (an SDIO RPC) */
int sysinfo_battery_percent(void);           /* -1 if unknown */
const char *sysinfo_power_src_text(void);
bool sysinfo_rtc_read(struct tm *out);
bool sysinfo_rtc_write(const struct tm *in);
void sysinfo_apply_timezone(void);           /* TZ from g_settings.tz_rule */
void sysinfo_time_from_rtc(void);
void sysinfo_start_sntp(void);
bool sysinfo_ina226_present(void);
/* Task 6. slow: 1024 averages, bus 140 us, shunt 8.244 ms, continuous (0x4E3F; a result every ~8.6 s);
   otherwise the Plan 1 config (0x4527), which sysinfo_init writes at every boot. */
void sysinfo_ina226_slow(bool slow);
void sysinfo_ina226_power_down(void);        /* config 0x4520 (mode 000); the next boot's sysinfo_init undoes it */
/* Early boot (before sysinfo_init): config, bus (1.25 mV LSB) and shunt (2.5 uV LSB) registers through a
   device of its own on bsp_i2c_get_handle(). False when the INA226 doesn't answer. */
bool sysinfo_ina226_raw(uint16_t *config, uint16_t *bus, int16_t *shunt);
/* Early boot: the RX8130's UTC time as an epoch, through a device of its own (the system clock starts at 0
   after a power-off). False when it doesn't answer or holds no valid date. */
bool sysinfo_rtc_epoch_raw(time_t *out);
bool sysinfo_rtc_present(void);
uint32_t sysinfo_uptime_s(void);
const char *sysinfo_app_version(void);
const char *sysinfo_app_project(void);
void sysinfo_mem(size_t *dram_free, size_t *dram_total, size_t *dma_free, size_t *dma_total,
                 size_t *psram_free, size_t *psram_total);

#ifdef __cplusplus
}
#endif
