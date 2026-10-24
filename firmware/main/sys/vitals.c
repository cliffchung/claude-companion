#include "vitals.h"
#include <string.h>

static const struct { float v, pct; } k_curve[] = {   /* per cell */
    { 4.20f, 100 }, { 4.10f, 90 }, { 4.00f, 80 }, { 3.92f, 70 }, { 3.85f, 60 }, { 3.79f, 50 },
    { 3.74f, 40 }, { 3.70f, 30 }, { 3.66f, 20 }, { 3.60f, 10 }, { 3.50f, 5 }, { 3.30f, 0 },
};
#define CURVE_N ((int)(sizeof(k_curve) / sizeof(k_curve[0])))

float vitals_ocv(float bus_v, float current_a, float r_ohm)
{
    return bus_v + current_a * r_ohm;
}

int vitals_percent_from_ocv(float pack_v)
{
    float c = pack_v / 2.0f;
    if (c >= k_curve[0].v) return 100;
    if (c <= k_curve[CURVE_N - 1].v) return 0;
    for (int i = 1; i < CURVE_N; i++) {
        if (c >= k_curve[i].v) {
            float f = (c - k_curve[i].v) / (k_curve[i - 1].v - k_curve[i].v);
            return (int)(k_curve[i].pct + f * (k_curve[i - 1].pct - k_curve[i].pct) + 0.5f);
        }
    }
    return 0;
}

int vitals_batt_update(vitals_batt_t *b, float bus_v, float current_a, float dt_s)
{
    if (bus_v < 5.5f) return -1;
    float ocv = vitals_ocv(bus_v, current_a, VITALS_R_INT_OHM);
    if (!b->init) {
        b->ema_v = ocv;
        b->init = true;
    } else {
        if (dt_s < 0) dt_s = 0;
        b->ema_v += (ocv - b->ema_v) * (dt_s / (30.0f + dt_s));
    }
    return vitals_percent_from_ocv(b->ema_v);
}

vitals_bat_level_t vitals_bat_level(int percent, bool charging)
{
    if (percent < 0) return VB_NONE;
    return (percent <= 15 && !charging) ? VB_LOW : VB_NORMAL;
}

int vitals_wifi_bars(bool connected, int rssi)
{
    if (!connected) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

bool vitals_tz_valid(const char *rule)
{
    if (!rule) return false;
    size_t n = strlen(rule);
    if (n < 1 || n > 47) return false;
    char c0 = rule[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '<')) return false;
    for (size_t i = 0; i < n; i++) {
        char c = rule[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == ',' || c == '.' || c == ':' || c == '/' || c == '+' || c == '<' || c == '>' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const vitals_tz_t vitals_tz_presets[] = {
    { "US Pacific", "PST8PDT,M3.2.0,M11.1.0" },
    { "US Mountain", "MST7MDT,M3.2.0,M11.1.0" },
    { "Arizona", "MST7" },
    { "US Central", "CST6CDT,M3.2.0,M11.1.0" },
    { "US Eastern", "EST5EDT,M3.2.0,M11.1.0" },
    { "UTC", "UTC0" },
    { "UK", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Japan", "JST-9" },
    { "Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { NULL, NULL },
};

bool vitals_charging(int ma) { return ma < VITALS_CHG_MA; }

/* PT23: the INA226 alone (CHG_STAT reads 0 whether or not the pack charges). mA, + = discharging.
   Charge current under -5 mA is USB at once; a bus at or above 8.25 V is a full pack on USB: standby,
   never battery (a full pack just unplugged reads standby until it sags below 8.25 V); 20 mA or more out of
   the pack is a battery sample (vitals_src_step wants VITALS_BATT_SAMPLES in a row). Anything else is no
   evidence: keep the previous source (battery stays battery, a finished charge is still USB). */
power_src_t vitals_power_src(int mv, int ma, power_src_t prev)
{
    if (mv < 5500) return PWR_UNKNOWN;
    if (vitals_charging(ma)) return PWR_USB_CHARGING;
    if (mv >= 8250) return PWR_USB_STANDBY;
    if (ma >= 20) return PWR_BATTERY;
    return prev == PWR_USB_CHARGING ? PWR_USB : prev;
}

power_src_t vitals_src_step(vitals_src_t *st, int mv, int ma)
{
    power_src_t n = vitals_power_src(mv, ma, st->src);
    if (n != PWR_BATTERY) { st->batt_n = 0; st->src = n; return n; }
    if (st->src == PWR_BATTERY) return n;
    /* not on battery so far: a battery-like sample must repeat; meanwhile keep the previous source
       (unknown after a boot) */
    if (++st->batt_n >= VITALS_BATT_SAMPLES) { st->src = PWR_BATTERY; return PWR_BATTERY; }
    return st->src;
}

void vitals_src_seed_battery(vitals_src_t *st, int n)
{
    if (!st || st->src != PWR_UNKNOWN) return;
    if (n < 1) n = 1;
    if (n > VITALS_BATT_SAMPLES) n = VITALS_BATT_SAMPLES;
    st->batt_n = VITALS_BATT_SAMPLES - n;
}

int vitals_clamp_or_zero(int v, int lo, int hi)
{
    if (v <= 0) return 0;
    if (v < lo) return v * 2 < lo ? 0 : lo;
    return v > hi ? hi : v;
}

void vitals_clamp_dim_off(uint16_t *dim_s, uint16_t *off_s)
{
    if (*dim_s < 10) *dim_s = 10;
    if (*dim_s > 600) *dim_s = 600;
    if (*off_s < *dim_s + 10) *off_s = (uint16_t)(*dim_s + 10);
    if (*off_s > 3600) *off_s = 3600;
}
