#pragma once
/* Pure battery, Wi-Fi and time-zone maths for the vitals bar. No ESP-IDF headers. */
#include <stdbool.h>
#include <stdint.h>
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

/* Charging = a charge current under VITALS_CHG_MA (-5 mA, ma + = discharging). PT23. */
#define VITALS_CHG_MA (-5)
bool  vitals_charging(int ma);

/* Where the power comes from, one sample (1 Hz), from the INA226 alone (PT23: the charge pin is not
   evidence). mv is the bus voltage, ma + = discharging. No pack (mv < 5500) -> PWR_UNKNOWN, never
   PWR_BATTERY. ma < -5 -> PWR_USB_CHARGING; else mv >= 8250 -> PWR_USB_STANDBY; else ma >= 20 -> PWR_BATTERY
   (a candidate: vitals_src_step wants it sustained, because a pack under load can supply current while USB is
   still plugged in); else no evidence: keep prev (PWR_USB_CHARGING becomes PWR_USB). */
power_src_t vitals_power_src(int mv, int ma, power_src_t prev);

/* No VBUS signal is known for this board, so USB -> battery is decided from a pattern: the bus below
   8.25 V and >= 20 mA out of the pack for VITALS_BATT_SAMPLES samples in a row (the previous source is
   reported meanwhile; unknown after a boot). Battery -> USB happens at once only on a charge current
   (< -5 mA) or a bus at 8.25 V; a low current alone stays battery. */
#define VITALS_BATT_SAMPLES 10
typedef struct { power_src_t src; int batt_n; } vitals_src_t;
power_src_t vitals_src_step(vitals_src_t *st, int mv, int ma);

/* Settings clamps: 0 stays 0 (never); a value in the gap goes to the nearest end of the range. */
int  vitals_clamp_or_zero(int v, int lo, int hi);
void vitals_clamp_dim_off(uint16_t *dim_s, uint16_t *off_s);   /* dim 10..600, off dim+10..3600 */

#ifdef __cplusplus
}
#endif
