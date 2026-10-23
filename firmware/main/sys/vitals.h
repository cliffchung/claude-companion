#pragma once
/* Pure battery, Wi-Fi and time-zone maths for the vitals bar. No ESP-IDF headers. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PWR_UNKNOWN = 0, PWR_USB, PWR_USB_CHARGING, PWR_USB_STANDBY, PWR_BATTERY } power_src_t;

#define VITALS_R_INT_OHM 0.15f      /* pack internal resistance: an estimate */

float vitals_ocv(float bus_v, float current_a, float r_ohm);   /* bus_v + current_a * r_ohm (+ = discharging) */
int   vitals_percent_from_ocv(float pack_v);                   /* 2S curve, clamped 0..100 */

typedef struct { float ema_v; bool init; } vitals_batt_t;
/* bus_v < 5.5 V (no 2S pack sits that low) -> -1 without touching the EMA; otherwise load-compensate
   with VITALS_R_INT_OHM, EMA with tau 30 s (alpha = dt / (30 + dt)); the first sample seeds the EMA. */
int   vitals_batt_update(vitals_batt_t *b, float bus_v, float current_a, float dt_s);

typedef enum { VB_NONE, VB_NORMAL, VB_LOW } vitals_bat_level_t;
vitals_bat_level_t vitals_bat_level(int percent, bool charging);   /* -1 -> NONE; <= 15 and not charging -> LOW */

int   vitals_wifi_bars(bool connected, int rssi);   /* 0 if not connected; >= -55: 4, >= -67: 3, >= -75: 2, else 1 */

bool  vitals_tz_valid(const char *rule);            /* 1..47 chars of [A-Za-z0-9,.:/+<>-], starts with a letter or '<' */
typedef struct { const char *label; const char *rule; } vitals_tz_t;
extern const vitals_tz_t vitals_tz_presets[];       /* NULL-terminated, US Pacific first */

/* Where the power comes from, one sample (1 Hz). mv is the INA226 bus voltage, ma + = discharging,
   chg_pin as sysinfo_charge_pin (-1 unreadable, 0 low = charging, 1 high). No pack (mv < 5500)
   -> PWR_UNKNOWN, never PWR_BATTERY. This is the instantaneous guess, with hysteresis from prev;
   a positive current alone does not prove battery, because a pack under load can supply current
   while USB is still plugged in. vitals_src_step applies the sustained rule below. */
power_src_t vitals_power_src(int mv, int ma, int chg_pin, power_src_t prev);

/* No VBUS signal is known for this board, so USB -> battery is decided from a pattern: the charge
   pin not low and a battery-like current for VITALS_BATT_SAMPLES samples in a row. Anything that
   looks like USB or charging (pin low, current < 0) ends it at once and moves battery -> USB. */
#define VITALS_BATT_SAMPLES 10
typedef struct { power_src_t src; int batt_n; } vitals_src_t;
power_src_t vitals_src_step(vitals_src_t *st, int mv, int ma, int chg_pin);

#ifdef __cplusplus
}
#endif
