#include "sdkconfig.h"
#include "hibernate.h"
#include "sys/boot_phase.h"
#include "sys/link_guard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"
#include "settings/settings.h"
#include "sys/sysinfo.h"
#include "sys/power_log.h"
#include "sys/power_bench.h"
#include "sys/imu.h"
#include "sys/hib_rules.h"
#include "sys/power_rules.h"
#include "sys/power.h"
#include "net/wifi_mgr.h"
#include "companion/cc_link.h"
#include "input/keyboard.h"

static const char *TAG = "hib";

/* ---- board facts (docs/superpowers/notes/2026-10-23-light-sleep-spike.md) ----
 * Tab5 schematic: expander 0x44 P4 is PWROFF_PULSE. Through D12 it turns on Q3A, which pulls the
 * power controller's trigger input (PMS150G U28 PA6, nINT_STAT_TRIG) low. The RX8130's nIRQ (via Q11/Q12),
 * the BMI270's INT1 (via Q8) and a USB plug-in edge pull the same line. The side button S1 is SW_PWR
 * on PA4. The factory firmware (M5Tab5-UserDemo bsp_generate_poweroff_signal) powers off with three
 * 100 ms pulses on P4 and wakes from an RX8130 alarm. */
/* PT26: Hibernate is a deep sleep. The side button wakes it (proved 2026-10-23: a press 76 s into a 900 s
   `power hib test timer`, arriving as hw reason 7, PT27), and CHG_EN stays driven high through it, so a
   sleeping Tab5 charges; a PWROFF_PULSE power-off drops the expander and with it CHG_EN (P115, PT22). The
   power-off stays for `power hib test off` only. */
#define HIB_MODE_BUILT HIB_MODE_DEEPSLEEP
/* Step 7 on USB: deep sleep wakes on the timer (proved); the PWROFF_PULSE power-off took (the board went
   dark with USB in); the RX8130 timer never switched it back on, so RTC is not a source; the side button
   did (S1 on the PMS150's SW_PWR; the user's press, 2026-10-23, 2 h 9 min after the power-off), and wakes
   deep sleep too (PT27). P60, P114. */
#define HIB_SOURCES (HIB_SRC_TIMER | HIB_SRC_BUTTON)
/* Step 4c: the keyboard (J9: SOC_3.3V or SYS_VIN, no expander switch) kept its A164 registers, amber at
   30%, through 60 s of deep sleep. Deep sleep only: a power-off drops SOC_3.3V. Ruling P61. */
#define KBD_SURVIVES true

#define EXP0 0x43
#define EXP1 0x44
#define PWROFF_PIN 4                      /* 0x44 P4 */
#define RX8130_ADDR 0x32

static const hib_rail_t k_rails[] = {
    { "rf-antenna", EXP0, 0, false },     /* RF_PTH_L_INT_H_EXT: the antenna switch, not a supply */
    { "speaker", EXP0, 1, true },         /* SPK_EN (NS4150B) */
    { "ext5v", EXP0, 2, true },           /* EXT5V_EN: the M5-Bus/header 5 V (off since boot: the BSP never enables it) */
    { "lcd-rst", EXP0, 4, true },         /* LCD_RST (BSP_LCD_EN): held in reset, not a supply */
    { "touch-rst", EXP0, 5, true },       /* TP_RST (BSP_TOUCH_EN): held in reset, not a supply */
    { "camera-rst", EXP0, 6, true },      /* CAM_RST (BSP_CAMERA_EN) */
    { "hp-det", EXP0, 7, false },         /* HP_DET: input, never driven */
    { "wifi", EXP1, 0, true },            /* WLAN_PWR_EN: the C6's supply (it cold-boots on the next start) */
    { "usb", EXP1, 3, true },             /* USB5V_EN: the USB-A host 5 V */
    { "pwroff", EXP1, 4, false },         /* PWROFF_PULSE: driven only to power off */
    { "chg-qc", EXP1, 5, false },         /* nCHG_QC_EN (IP2326): left alone */
    { "chg-stat", EXP1, 6, false },       /* CHG_STAT (IP2326 LED): input, never driven */
    { "chg-en", EXP1, 7, false },         /* CHG_EN (IP2326 EN): driven high at boot (charging on), never cut (PT22) */
};

const hib_rail_t *hibernate_rails(int *n)
{
    *n = (int)(sizeof(k_rails) / sizeof(k_rails[0]));
    return k_rails;
}

hib_mode_t hibernate_mode(void) { return HIB_MODE_BUILT; }
uint32_t hibernate_wake_sources(void) { return HIB_SOURCES; }
bool hibernate_kbd_rail_survives(void) { return KBD_SURVIVES; }
bool hibernate_supported(void) { return HIB_MODE_BUILT != HIB_MODE_NONE && (HIB_SOURCES & ~HIB_SRC_TIMER) != 0; }

/* ---- the "hib" record (NVS ns "power") ---------------------------------- */

typedef struct {
    uint8_t ver;
    uint8_t mode;          /* hib_mode_t asked for */
    uint8_t next;          /* power_boot_t for the next boot */
    uint8_t fell_back;     /* power-off didn't take (USB keeps the board on): deep sleep instead */
    uint32_t epoch;
    uint32_t wake_after_s;
    uint8_t debug;         /* HIB_DBG_* for the next boot (Task 8) */
    uint8_t led_ok;        /* led[] holds the light last written before the sleep */
    uint8_t led[5];        /* mode, bright, r, g, b */
    uint8_t fails;         /* I3: consecutive failed check-ins (was padding, always 0) */
} hib_rec_t;
#define HIB_VER 3

static power_boot_t s_kind = POWER_BOOT_NORMAL;
static uint32_t s_src;
static const char *s_cause = "-";
static uint8_t s_fell_back;
static uint32_t s_to_live;              /* the last hibernate wake's boot-to-live, from NVS "hibl" */
static uint32_t s_test_wake_s;          /* `power hib test-wake`: RAM only */
static bool s_early;                    /* going down from hibernate_boot_early: nothing else has started */
static uint8_t s_dbg_next, s_dbg_boot;  /* HIB_DBG_*: for the next boot, and this boot's */
static uint8_t s_fails_next, s_fails_boot;   /* I3: for the next check-in boot, and this boot's */
static portMUX_TYPE s_down_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_going_down;               /* M6: the first go_down wins */

/* the persistent backstop: RTC memory, valid only with its magic and check word */
typedef struct { uint32_t magic, s, n, check; } backstop_t;
#define BACKSTOP_MAGIC 0x42534b31u
static RTC_NOINIT_ATTR backstop_t s_backstop;
static bool backstop_armed(void)
{
    const backstop_t *b = &s_backstop;
    return b->magic == BACKSTOP_MAGIC && b->check == (b->s ^ b->n ^ BACKSTOP_MAGIC) && b->n > 0 && b->s > 0;
}
void hibernate_backstop_set(uint32_t s, uint32_t n)
{
    s_backstop = (backstop_t){ BACKSTOP_MAGIC, s, n, s ^ n ^ BACKSTOP_MAGIC };
}
/* PT26 test: the power probe's period and forced sleep-agains, RTC memory (survives deep sleep) */
typedef struct { uint32_t magic, s, n, slept, last_ms, check; } probe_test_t;
#define PROBE_MAGIC 0x50524231u
static RTC_NOINIT_ATTR probe_test_t s_ptest;
static uint32_t ptest_check(const probe_test_t *p) { return p->s ^ p->n ^ p->slept ^ p->last_ms ^ PROBE_MAGIC; }
static bool ptest_valid(void) { return s_ptest.magic == PROBE_MAGIC && s_ptest.check == ptest_check(&s_ptest); }
static void ptest_seal(void) { s_ptest.magic = PROBE_MAGIC; s_ptest.check = ptest_check(&s_ptest); }
static uint32_t probe_s(void) { return ptest_valid() ? s_ptest.s : 0; }
void hibernate_probe_test_set(uint32_t s, uint32_t n)
{
    if (!ptest_valid()) memset(&s_ptest, 0, sizeof(s_ptest));
    s_ptest.s = s;
    s_ptest.n = s ? n : 0;
    ptest_seal();
}
int hibernate_boot_checkin_fails(void) { return s_fails_boot; }

uint32_t hibernate_checkin_s(void) { return (uint32_t)pr_hib_checkin_clamp_min(power_cfg(PCFG_HIB_CHECKIN)) * 60u; }
static bool s_led_ok;
static pr_led_t s_led_boot;
bool hibernate_boot_light(pr_led_t *out)
{
    /* N2: only a timer wake (asleep within the timer's -2..+20 s) kept the keyboard powered with that light;
       a power-on lost it, and the stored light could be of any age */
    if (!s_led_ok || s_src != HIB_SRC_TIMER) return false;
    *out = s_led_boot;
    return true;
}
void hibernate_set_debug_next(uint8_t flags) { s_dbg_next = flags; }
uint8_t hibernate_boot_debug(void) { return s_dbg_boot; }

power_boot_t hibernate_boot_kind(void) { return s_kind; }
uint32_t hibernate_wake_src(void) { return s_src; }
const char *hibernate_cause_text(void) { return s_cause; }
uint32_t hibernate_boot_to_live_ms(void) { return s_to_live; }
uint32_t hibernate_test_wake_s(void) { return s_test_wake_s; }

/* ---- small I2C helpers (the BSP bus; the expander driver keeps a shadow, so writes go through it) ---- */

static i2c_master_dev_handle_t s_rtc, s_exp[2];

static i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    i2c_master_dev_handle_t *slot = addr == RX8130_ADDR ? &s_rtc : addr == EXP0 ? &s_exp[0] : addr == EXP1 ? &s_exp[1] : NULL;
    if (!slot) return NULL;
    if (*slot) return *slot;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return NULL;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, slot) != ESP_OK) *slot = NULL;
    return *slot;
}

static bool reg_read(uint8_t addr, uint8_t reg, uint8_t *v)
{
    i2c_master_dev_handle_t d = dev_for(addr);
    return d && i2c_master_transmit_receive(d, &reg, 1, v, 1, 50) == ESP_OK;
}

static bool reg_write(uint8_t addr, uint8_t reg, uint8_t v)
{
    i2c_master_dev_handle_t d = dev_for(addr);
    uint8_t w[2] = { reg, v };
    return d && i2c_master_transmit(d, w, 2, 50) == ESP_OK;
}

/* ---- RX8130: a countdown timer that pulls nIRQ low when it expires ---- */
#define RTC_EXT 0x1C      /* TSEL[2:0], TE bit4 */
#define RTC_FLAG 0x1D     /* AF bit3, TF bit4 */
#define RTC_CTRL0 0x1E    /* AIE bit3, TIE bit4, STOP bit6 */

static bool rtc_timer_arm(uint32_t s)
{
    uint8_t ext, ctrl;
    uint32_t count = s;
    uint8_t tsel = 2;                                 /* 1 Hz */
    if (count > 65535) { count = (s + 59) / 60; tsel = 3; }   /* 1/60 Hz: up to 45 days */
    if (count > 65535) count = 65535;
    if (!reg_read(RX8130_ADDR, RTC_EXT, &ext) || !reg_read(RX8130_ADDR, RTC_CTRL0, &ctrl)) return false;
    bool ok = reg_write(RX8130_ADDR, RTC_EXT, ext & ~0x10);                /* TE = 0 */
    ok = ok && reg_write(RX8130_ADDR, RTC_FLAG, 0);                        /* AF, TF cleared */
    ok = ok && reg_write(RX8130_ADDR, 0x1A, (uint8_t)count);
    ok = ok && reg_write(RX8130_ADDR, 0x1B, (uint8_t)(count >> 8));
    ok = ok && reg_write(RX8130_ADDR, RTC_EXT, (uint8_t)((ext & ~0x17) | 0x10 | tsel));
    ok = ok && reg_write(RX8130_ADDR, RTC_CTRL0, (uint8_t)((ctrl & ~0x08) | 0x10));   /* TIE on, AIE off */
    uint8_t e2 = 0, c2 = 0, lo = 0, hi = 0;
    reg_read(RX8130_ADDR, RTC_EXT, &e2);
    reg_read(RX8130_ADDR, RTC_CTRL0, &c2);
    reg_read(RX8130_ADDR, 0x1A, &lo);
    reg_read(RX8130_ADDR, 0x1B, &hi);
    ESP_LOGI(TAG, "rtc timer: %u x %s, ext=0x%02x ctrl0=0x%02x count=%u: %s", (unsigned)count, tsel == 2 ? "1 s" : "1 min", e2, c2,
             (unsigned)(lo | hi << 8), ok ? "armed" : "failed");
    return ok;
}

/* Disarms the timer and alarm and releases nIRQ: a held nIRQ would hold the power controller's
   trigger low, so the power-off pulses and the button couldn't reach it. Returns the flags before. */
static uint8_t rtc_clear(void)
{
    uint8_t flag = 0, ext, ctrl;
    if (!reg_read(RX8130_ADDR, RTC_FLAG, &flag)) return 0;
    if (reg_read(RX8130_ADDR, RTC_CTRL0, &ctrl)) reg_write(RX8130_ADDR, RTC_CTRL0, ctrl & ~0x18);
    if (reg_read(RX8130_ADDR, RTC_EXT, &ext)) reg_write(RX8130_ADDR, RTC_EXT, ext & ~0x10);
    reg_write(RX8130_ADDR, RTC_FLAG, 0);
    return flag;
}

/* ---- early boot ----------------------------------------------------------- */

static bool rec_get(hib_rec_t *r)
{
    void *blob = NULL;
    size_t len = 0;
    bool ok = nvs_get_blob_alloc("power", "hib", &blob, &len) && len == sizeof(*r);
    if (ok) memcpy(r, blob, sizeof(*r));
    free(blob);
    return ok && r->ver == HIB_VER;
}

/* the system clock, or the RX8130's after a reset that lost it */
static time_t epoch_now(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) sysinfo_rtc_epoch_raw(&now);
    return now;
}

static void go_down(uint32_t wake_after_s, power_boot_t next, hib_mode_t mode);

/* PT26: a power probe wake reads the source only (no Wi-Fi, no display, no rail). USB boots, anything else
   sleeps again at once with the same timer. */
static void power_probe_boot(const hib_rec_t *r)
{
    int mv = 0, ma = 0;
    bool have = sysinfo_ina226_oneshot(&mv, &ma);
    hib_pp_t v = hib_power_probe(have, mv, ma);
    bool forced = false;
    if (ptest_valid() && s_ptest.n > 0) { s_ptest.n--; ptest_seal(); forced = true; v = HIB_PP_SLEEP; }
    ESP_LOGI(TAG, "power probe: %d mV %d mA%s: %s%s (app %u ms)", mv, ma, have ? "" : " (no reading)",
             v == HIB_PP_BOOT ? "boot" : "sleep again", forced ? " (test)" : "", (unsigned)(esp_timer_get_time() / 1000));
    if (v == HIB_PP_BOOT) { s_kind = POWER_BOOT_HIB_WAKE; return; }
    s_fails_next = r->fails;
    s_early = true;
    go_down(r->wake_after_s, POWER_BOOT_HIB_POWER_PROBE, HIB_MODE_DEEPSLEEP);
    s_early = false;
    s_kind = POWER_BOOT_HIB_WAKE;   /* the sleep failed: boot on */
}

void hibernate_boot_early(void)
{
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) { ESP_LOGW(TAG, "nvs: %s (settings_init deals with it)", esp_err_to_name(e)); return; }
    void *blob = NULL;
    size_t len = 0;
    if (nvs_get_blob_alloc("power", "hibl", &blob, &len) && len == sizeof(uint32_t)) memcpy(&s_to_live, blob, sizeof(s_to_live));
    free(blob);
    uint8_t rtc_flags = rtc_clear();
    hib_rec_t r;
    if (!rec_get(&r)) return;   /* a normal boot */
    nvs_erase_key_ns("power", "hib");
    s_kind = (power_boot_t)r.next;
    s_fell_back = r.fell_back;
    s_dbg_boot = r.debug;
    s_fails_boot = r.fails;
    s_led_ok = r.led_ok;
    s_led_boot = (pr_led_t){ r.led[0], r.led[1], r.led[2], r.led[3], r.led[4] };
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t rr = esp_reset_reason();
    /* On this P4 (rev < 3) the timer wake from deep sleep goes through an HP watchdog reset (hw reason 7)
       on the way up, which clears the wakeup cause: so the time asleep against the timer decides it (the
       RTC keeps the time across deep sleep; after a power-off the clock starts again from 0). */
    time_t now = epoch_now();   /* after a power-off the system clock starts at 0 */
    long asleep = (long)(now - (time_t)r.epoch);
    hib_wake_in_t in = { .wakeup_timer = c == ESP_SLEEP_WAKEUP_TIMER, .rtc_flags = rtc_flags,
                         .reset_poweron = rr == ESP_RST_POWERON,
                         .was_poweroff = r.mode == HIB_MODE_POWEROFF && !r.fell_back,
                         .reset_deepsleep_or_wdt = rr == ESP_RST_DEEPSLEEP || rr == ESP_RST_WDT,
                         .was_deepsleep = r.mode == HIB_MODE_DEEPSLEEP || r.fell_back,
                         .reset_crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                                        rr == ESP_RST_BROWNOUT,
                         .wake_after_s = r.wake_after_s, .asleep_s = asleep };
    static const struct { uint32_t src; const char *name; } k_cause[] = {
        [HIB_CAUSE_NONE] = { 0, "-" }, [HIB_CAUSE_TIMER] = { HIB_SRC_TIMER, "timer" }, [HIB_CAUSE_RTC] = { HIB_SRC_RTC, "rtc" },
        [HIB_CAUSE_BUTTON] = { HIB_SRC_BUTTON, "button" }, [HIB_CAUSE_RESET] = { HIB_SRC_RESET, "reset" },
    };
    hib_cause_t hc = hib_cause(&in);
    s_src = k_cause[hc].src;
    s_cause = k_cause[hc].name;
    if ((rtc_flags & 0x18) && hc != HIB_CAUSE_RTC)
        ESP_LOGW(TAG, "the RX8130 fired earlier without switching the board on (flags 0x%02x)", rtc_flags);
    /* M4: a reset on the way down or mid-probe is not a hibernate wake: no Waking banner, no probe sample
       (the probe row then reads "interrupted"). M9: a button press never boots dark (a check-in) */
    s_kind = hib_boot_kind((power_boot_t)r.next, hc);
    ESP_LOGI(TAG, "boot after hibernate: kind=%d cause=%s (wakeup %d, reset %d, hw reset 0x%x, rtc flags 0x%02x%s, asleep %lds)",
             (int)s_kind, s_cause, (int)c, (int)rr, (unsigned)esp_rom_get_reset_reason(0), rtc_flags,
             r.fell_back ? ", power-off fell back to deep sleep" : "", asleep);
    if (s_kind == POWER_BOOT_HIB_POWER_PROBE) { power_probe_boot(&r); return; }
    if (s_kind != POWER_BOOT_HIB_PROBE) return;
    /* a bench probe: one INA226 sample, taken before any rail comes up, then sleep again or finish */
    uint16_t cfg = 0, bus = 0;
    int16_t shunt = 0;
    bool have = sysinfo_ina226_raw(&cfg, &bus, &shunt) && cfg == 0x4E3F;   /* still in the probe's slow mode */
    uint32_t sleep_s = 0;
    bool deep = false;
    power_bench_probe_t next = power_bench_probe_boot(have, (uint16_t)(bus * 5 / 4), shunt, &sleep_s, &deep);
    if (next == POWER_BENCH_PROBE_DONE) { s_kind = POWER_BOOT_NORMAL; return; }
    s_early = true;
    if (deep) hibernate_deep_sleep(sleep_s, POWER_BOOT_HIB_PROBE);
    else hibernate_now(sleep_s, POWER_BOOT_HIB_PROBE);
    s_early = false;
    s_kind = POWER_BOOT_NORMAL;   /* the sleep failed: boot on */
}

/* ---- boot to live --------------------------------------------------------- */

static bool s_live_done;

static void store_live_task(void *arg)
{
    (void)arg;
    uint32_t ms = s_to_live;
    nvs_set_blob_ns("power", "hibl", &ms, sizeof(ms));
    vTaskDelete(NULL);
}

static void on_sessions(const cc_session_t *s, int n, void *ctx)
{
    (void)s; (void)n; (void)ctx;
    if (s_live_done) return;
    s_live_done = true;   /* copy and store only: no cc_link calls from here */
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "boot to live: %u ms (kind %d)", (unsigned)ms, (int)s_kind);
    if (s_kind == POWER_BOOT_NORMAL) return;
    s_to_live = ms;
    /* the flash write goes on a one-shot task of its own, not the poll task */
    if (xTaskCreate(store_live_task, "hibl", 3072, NULL, 2, NULL) != pdPASS) ESP_LOGW(TAG, "boot to live: not stored");
}

void hibernate_init(void)
{
    cc_poll_subscribe(on_sessions, NULL);
}

/* ---- going down ----------------------------------------------------------- */

static void cut_rails(void)
{
    esp_io_expander_handle_t ex0 = bsp_io_expander_init(), ex1 = bsp_io_expander1_init();
    for (size_t i = 0; i < sizeof(k_rails) / sizeof(k_rails[0]); i++) {
        const hib_rail_t *r = &k_rails[i];
        if (!r->cut) continue;
        esp_io_expander_handle_t ex = r->addr == EXP0 ? ex0 : ex1;
        if (!ex) continue;
        /* output, push-pull, low: the enables off and the resets asserted */
        esp_io_expander_set_dir(ex, 1u << r->pin, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(ex, 1u << r->pin, 0);
        esp_io_expander_set_output_mode(ex, 1u << r->pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    }
    bsp_display_brightness_set(0);
}

static void power_off_pulses(void)
{
    esp_io_expander_handle_t ex1 = bsp_io_expander1_init();
    if (!ex1) return;
    uint32_t m = 1u << PWROFF_PIN;
    esp_io_expander_set_level(ex1, m, 0);
    esp_io_expander_set_output_mode(ex1, m, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_set_dir(ex1, m, IO_EXPANDER_OUTPUT);
    for (int i = 0; i < 3; i++) {   /* as the factory firmware */
        esp_io_expander_set_level(ex1, m, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_io_expander_set_level(ex1, m, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void go_down(uint32_t wake_after_s, power_boot_t next, hib_mode_t mode)
{
    /* M6: the hold's low-battery hibernate and JOB_DOWN can both get here; the first one goes down */
    taskENTER_CRITICAL(&s_down_mux);
    bool second = s_going_down;
    s_going_down = true;
    taskEXIT_CRITICAL(&s_down_mux);
    if (second) {
        ESP_LOGW(TAG, "hibernate: already going down (task %s waits)", pcTaskGetName(NULL));
        for (;;) vTaskDelay(portMAX_DELAY);
    }
    bool probe = next == POWER_BOOT_HIB_PROBE;
    bool pprobe = s_early && next == POWER_BOOT_HIB_POWER_PROBE;   /* PT26: a power probe sleeping again */
    bool bs = backstop_armed();
    /* M4: every deep sleep has a timer (Task 8's backstop only shortens it, so tests can't strand it) */
    hib_plan_t plan = hib_sleep_plan(wake_after_s, next, s_test_wake_s, bs ? s_backstop.s : 0);
    wake_after_s = plan.timer_s;
    next = plan.next;
    s_test_wake_s = 0;
    if (bs) {
        hibernate_backstop_set(s_backstop.s, s_backstop.n - 1);
        ESP_LOGW(TAG, "backstop: wake after %u s (%u more hibernates)", (unsigned)wake_after_s, (unsigned)s_backstop.n);
    }
    ESP_LOGW(TAG, "hibernate: %s, wake after %u s, next boot %d", mode == HIB_MODE_POWEROFF ? "power-off" : "deep sleep",
             (unsigned)wake_after_s, (int)next);
    if (!s_early) {
        power_log_save();
        /* Task 3: a check-in boot never starts the watch loop (M3), so there is nothing to wait for */
        bool fast = s_kind == POWER_BOOT_HIB_CHECKIN && !cc_link_watch_wanted() && !(boot_phase_exp() & BPX_OLD_DOWN);
        cc_link_watch_enable(false);   /* the Host's watch loop ends now, not after its quiet timeout */
        vTaskDelay(pdMS_TO_TICKS(fast ? 0 : 300));
        /* Task 3 (L37): close the SSH session while Wi-Fi is still up, so the Host's sshd sees the close.
           Before, the link noticed only after the Wi-Fi went, and every check-in left an idle session on the
           Host until its timeout (42 of them piled up in a fast test chain, and sshd then reset new connects). */
        cc_link_park(true);
        /* final review C1/I1: close the link and any open SSH Terminal or take-over session, and wait (about 2 s
           at most) until none is open or opening, not just until the link stops reading as connected: a
           connect in flight closes its new session itself, and a poll or paste in flight gives up */
        lg_token_t lt = lg_close(false, LG_CLOSE_MS);
        if (!lt.idle) ESP_LOGW(TAG, "hibernate: SSH still busy after %d ms; going down anyway", lt.waited_ms);
        wifi_mgr_disconnect();
        vTaskDelay(pdMS_TO_TICKS(fast ? 50 : 200));   /* the deauth goes out */
    }
    hib_rec_t r = { .ver = HIB_VER, .mode = (uint8_t)mode, .next = (uint8_t)next, .epoch = (uint32_t)epoch_now(),
                    .wake_after_s = wake_after_s, .debug = s_dbg_next,
                    .fails = next == POWER_BOOT_HIB_CHECKIN || next == POWER_BOOT_HIB_POWER_PROBE ? s_fails_next : 0 };
    s_dbg_next = 0;
    s_fails_next = 0;
    if (!s_early) {   /* the light the keyboard holds through the sleep (I3); the LED lock is recursive */
        keyboard_led_lock();
        keyboard_get_last_led(&r.led[0], &r.led[1], &r.led[2], &r.led[3], &r.led[4]);
        keyboard_led_unlock();
        r.led_ok = 1;
    }
    nvs_set_blob_ns("power", "hib", &r, sizeof(r));
    if (!pprobe) {   /* a power probe: the IMU is still suspended and the one-shot left the INA226 down */
        imu_suspend();
        if (!probe) sysinfo_ina226_power_down();
    }
    esp_sleep_enable_timer_wakeup((uint64_t)wake_after_s * 1000000ULL);
    if (mode == HIB_MODE_POWEROFF) {
        /* before the cuts: with WIFI_EN cut, esp_hosted's next SDIO write fails and it restarts the
           host, so the pulses go first and the cuts follow only if the board is still on */
        /* `power hib test off` only (PT26) */
        if (!s_early) bsp_display_brightness_set(0);
        if (!rtc_timer_arm(wake_after_s)) ESP_LOGE(TAG, "rtc timer: not armed");
        power_off_pulses();
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* still running: something keeps the board on (or holds the trigger). Deep sleep with the timer */
        ESP_LOGW(TAG, "power-off did not take, deep sleep with the %u s timer", (unsigned)wake_after_s);
        r.fell_back = 1;
        nvs_set_blob_ns("power", "hib", &r, sizeof(r));
    }
    if (!s_early) cut_rails();   /* a probe boot: the rails are still as the last cycle left them */
    if (pprobe) {
        if (!ptest_valid()) memset(&s_ptest, 0, sizeof(s_ptest));   /* stats only: a period of 0 is the default */
        s_ptest.slept++;
        s_ptest.last_ms = esp_log_timestamp();   /* awake this long since the reset (the log's clock, bootloader included) */
        ptest_seal();
    }
    ESP_LOGI(TAG, "down from task %s at %lld ms, stack free %u B", pcTaskGetName(NULL), (long long)(esp_timer_get_time() / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    boot_phase_mark(BP_SLEEP);
    boot_phase_note_sleep(wake_after_s);
    boot_phase_save((uint8_t)s_kind);
    esp_deep_sleep_start();
}

/* PT26: always a deep sleep with a timer (go_down's plan). */
void hibernate_now(uint32_t wake_after_s, power_boot_t next)
{
    go_down(wake_after_s, next, HIB_MODE_DEEPSLEEP);
}

void hibernate_ladder(bool low_battery, int checkin_fails)
{
    hib_plan_t p = hib_ladder_plan(low_battery, hibernate_checkin_s(), checkin_fails, s_test_wake_s, probe_s());
    if (p.next == POWER_BOOT_HIB_POWER_PROBE) {
        /* no check-in will judge a light again: off, so a stale amber can't sit through the sleep */
        ESP_LOGW(TAG, "%s: deep sleep with the power probe every %u s", low_battery ? "battery low" : "check-in off",
                 (unsigned)p.timer_s);
        keyboard_led_lock();
        keyboard_write_led(1, 0, 0, 0, 0);
        go_down(p.timer_s, p.next, HIB_MODE_DEEPSLEEP);
        keyboard_led_unlock();
        return;
    }
    if (p.next == POWER_BOOT_HIB_CHECKIN && checkin_fails > 0)
        ESP_LOGW(TAG, "check-in backoff: %d failed, next in %u s", checkin_fails, (unsigned)p.timer_s);
    s_fails_next = (uint8_t)(checkin_fails > 255 ? 255 : checkin_fails < 0 ? 0 : checkin_fails);
    go_down(p.timer_s, p.next, HIB_MODE_DEEPSLEEP);
}
void hibernate_deep_sleep(uint32_t wake_after_s, power_boot_t next) { go_down(wake_after_s, next, HIB_MODE_DEEPSLEEP); }

/* ---- console ---------------------------------------------------------------- */

static const char *kind_name(power_boot_t k)
{
    switch (k) {
    case POWER_BOOT_HIB_WAKE: return "hib-wake";
    case POWER_BOOT_HIB_CHECKIN: return "hib-checkin";
    case POWER_BOOT_HIB_PROBE: return "hib-probe";
    case POWER_BOOT_HIB_POWER_PROBE: return "hib-power-probe";
    default: return "normal";
    }
}

static const char *mode_name(hib_mode_t m)
{
    return m == HIB_MODE_POWEROFF ? "off" : m == HIB_MODE_DEEPSLEEP ? "deepsleep" : "none";
}

static void sources_text(uint32_t s, char *b, size_t cap)
{
    b[0] = 0;
    if (s & HIB_SRC_TIMER) strlcat(b, "timer,", cap);
    if (s & HIB_SRC_BUTTON) strlcat(b, "button,", cap);
    if (s & HIB_SRC_RESET) strlcat(b, "reset,", cap);
    if (s & HIB_SRC_RTC) strlcat(b, "rtc,", cap);
    size_t n = strlen(b);
    if (n) b[n - 1] = 0;
    else strlcpy(b, "none", cap);
}

int hibernate_chg_en(void)
{
    uint8_t dir = 0, out = 0, hiz = 0;
    /* the output latch, as `power rails` shows it: the input register reads P7 as 0 even while it is driven high */
    if (!reg_read(EXP1, 0x03, &dir) || !reg_read(EXP1, 0x05, &out) || !reg_read(EXP1, 0x07, &hiz)) return -1;
    return (dir & 0x80) && !(hiz & 0x80) && (out & 0x80) ? 1 : 0;
}

static void rails_print(void)
{
    static const uint8_t addrs[2] = { EXP0, EXP1 };
    for (int a = 0; a < 2; a++) {
        uint8_t dir = 0, out = 0, hiz = 0, in = 0;
        if (!reg_read(addrs[a], 0x03, &dir) || !reg_read(addrs[a], 0x05, &out) || !reg_read(addrs[a], 0x07, &hiz) ||
            !reg_read(addrs[a], 0x0F, &in)) {
            printf("0x%02x: unreadable\n", addrs[a]);
            continue;
        }
        for (int p = 0; p < 8; p++) {
            const hib_rail_t *r = NULL;
            for (size_t i = 0; i < sizeof(k_rails) / sizeof(k_rails[0]); i++)
                if (k_rails[i].addr == addrs[a] && k_rails[i].pin == p) r = &k_rails[i];
            /* 0x03 bit set = output; an output with its 0x07 bit set is not driven (hi-z): show the pin's input then */
            bool o = dir & (1 << p), z = hiz & (1 << p);
            const char *how = !o ? "in" : z ? "hi-z" : "out";
            printf("0x%02x P%d %s %s %d %s\n", addrs[a], p, r ? r->name : "?", how, o && !z ? (out >> p) & 1 : (in >> p) & 1,
                   r ? (r->cut ? "cut" : "kept") : "-");
        }
    }
}

#if CONFIG_COMPANION_DEBUG_COMMANDS
static int rail_cmd(const char *name, const char *how)
{
    bool on = strcmp(how, "on") == 0;
    if (!on && strcmp(how, "off") != 0) return 1;
    bsp_feature_t f;
    if (strcmp(name, "speaker") == 0) f = BSP_FEATURE_SPEAKER;
    else if (strcmp(name, "camera") == 0) f = BSP_FEATURE_CAMERA;
    else if (strcmp(name, "usb") == 0) f = BSP_FEATURE_USB;
    else if (strcmp(name, "ext5v") == 0) {
        esp_io_expander_handle_t ex0 = bsp_io_expander_init();
        if (!ex0) return 1;
        esp_io_expander_set_dir(ex0, 1u << 2, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(ex0, 1u << 2, on);
        esp_io_expander_set_output_mode(ex0, 1u << 2, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
        printf("rail ext5v: %s\n", how);
        return 0;
    } else {
        printf("rail: only speaker, camera, usb, ext5v (LCD, touch and Wi-Fi are cut only on the way down)\n");
        return 1;
    }
    esp_err_t e = bsp_feature_enable(f, on);
    printf("rail %s: %s (%s)\n", name, how, esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 0 || v > 7 * 24 * 3600) return false;
    *out = (uint32_t)v;
    return true;
}
#endif

int hibernate_console(int argc, char **argv)
{
    /* argv[0] is "power" */
    if (argc == 2 && strcmp(argv[1], "boot") == 0) {
        printf("boot: reason=%d kind=%s cause=%s wakeup=%d to_live=%u%s%s\n", (int)esp_reset_reason(), kind_name(s_kind), s_cause,
               (int)esp_sleep_get_wakeup_cause(), (unsigned)s_to_live, s_fell_back ? " (power-off fell back to deep sleep)" : "",
               s_dbg_boot ? " (debug check-in)" : "");
        if (s_fails_boot) printf("boot: %u failed check-ins before this one (I3 backoff)\n", s_fails_boot);
        if (ptest_valid() && (s_ptest.slept || s_ptest.s))
            printf("boot: power probe slept again %u times, the last after %u ms awake (test: every %u s, %u forced left)\n",
                   (unsigned)s_ptest.slept, (unsigned)s_ptest.last_ms, (unsigned)s_ptest.s, (unsigned)s_ptest.n);
        if (s_led_ok)   /* the light the record says the keyboard held through the sleep (I3) */
            printf("boot: stored light %u %u %02x %02x %02x%s\n", s_led_boot.mode, s_led_boot.bright, s_led_boot.r,
                   s_led_boot.g, s_led_boot.b, s_src == HIB_SRC_TIMER ? "" : " (not a timer wake: not used)");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "rails") == 0) { rails_print(); return 0; }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 4 && strcmp(argv[1], "rail") == 0) return rail_cmd(argv[2], argv[3]);
#endif
    if (argc == 2 && strcmp(argv[1], "imu") == 0) {
        uint8_t id = 0, ctrl = 0, conf = 0;
        bool ok = imu_chip_id(&id) == ESP_OK && imu_reg_read(0x7D, &ctrl, 1) == ESP_OK && imu_reg_read(0x7C, &conf, 1) == ESP_OK;
        if (!ok) { printf("imu: no answer at 0x68\n"); return 1; }
        printf("imu: chip=0x%02x suspended=%s (pwr_ctrl=0x%02x pwr_conf=0x%02x)\n", id,
               ctrl == 0 && (conf & 0x01) ? "yes" : "no", ctrl, conf);
        return 0;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 3 && strcmp(argv[1], "imu") == 0 && strcmp(argv[2], "suspend") == 0) {
        printf("imu suspend: %s\n", esp_err_to_name(imu_suspend()));
        return 0;
    }
#endif
    if (argc < 2 || strcmp(argv[1], "hib") != 0) return -1;
    if (argc == 2) {
        char b[48];
        sources_text(hibernate_wake_sources(), b, sizeof(b));
        uint32_t ck = hibernate_checkin_s();
        printf("hibernate: mode=%s sources=%s supported=%s checkin=%us probe=%us (PT26: power-off only by `power hib test off`)\n",
               mode_name(hibernate_mode()), b, hibernate_supported() ? "yes" : "no", (unsigned)ck,
               (unsigned)(probe_s() ? probe_s() : HIB_POWER_PROBE_S));
        return 0;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 5 && strcmp(argv[2], "probe") == 0) {
        uint32_t ps, pn;
        if (!parse_u32(argv[3], &ps) || !parse_u32(argv[4], &pn)) return 1;
        hibernate_probe_test_set(ps, pn);
        if (ps) printf("hib probe: every %u s, the next %u probe wakes sleep again (RTC memory, test only)\n", (unsigned)ps, (unsigned)pn);
        else printf("hib probe: every %u s (the default)\n", (unsigned)HIB_POWER_PROBE_S);
        return 0;
    }
#endif
    if (argc == 3 && strcmp(argv[2], "backstop") == 0) {
        if (backstop_armed()) printf("hib backstop: %u s on each of the next %u hibernates\n", (unsigned)s_backstop.s, (unsigned)s_backstop.n);
        else printf("hib backstop: off\n");
        return 0;
    }
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc == 5 && strcmp(argv[2], "backstop") == 0) {
        uint32_t bs, bn;
        if (!parse_u32(argv[3], &bs) || !parse_u32(argv[4], &bn) || (bn && !bs)) return 1;
        hibernate_backstop_set(bs, bn);
        if (bn) printf("hib backstop: %u s on each of the next %u hibernates (RTC memory)\n", (unsigned)bs, (unsigned)bn);
        else printf("hib backstop: off\n");
        return 0;
    }
    if (argc == 4 && strcmp(argv[2], "test-wake") == 0) {
        uint32_t s;
        if (!parse_u32(argv[3], &s) || !s) return 1;
        s_test_wake_s = s;
        printf("hib test-wake: the next hibernate wakes after %u s\n", (unsigned)s);
        return 0;
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[2], "test") == 0) {
        uint32_t s = 30;
        if (argc == 5 && (!parse_u32(argv[4], &s) || !s)) return 1;
        const char *src = argv[3];
        if (strcmp(src, "timer") == 0) {
            printf("hib test: armed timer %u s, sleeping\n", (unsigned)s);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            hibernate_deep_sleep(s, POWER_BOOT_HIB_WAKE);
        } else if (strcmp(src, "light") == 0) {
            /* PT16: amber at the dark brightness (PT24), nothing on the keyboard's supply touched, deep sleep on
               the timer; the next boot's keyboard probe logs what the A164 still holds */
            if (argc == 4) s = 60;
            int pct = pr_dark_light_clamp(g_settings.kbd_dark_light);
            keyboard_led_lock();
            bool ok = keyboard_write_led(1, (uint8_t)pct, 0xf4, 0xb5, 0x45);
            printf("hib test: light amber %d%% (%s), armed timer %u s, sleeping\n", pct, ok ? "written" : "no keyboard",
                   (unsigned)s);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            hibernate_deep_sleep(s, POWER_BOOT_HIB_WAKE);
            keyboard_led_unlock();
        } else if (strcmp(src, "rtc") == 0 || strcmp(src, "off") == 0) {
            if (argc == 4) s = 60;
            /* power-off with the RX8130 timer (the only thing that can switch it back on without a press);
               the ESP timer is armed too, for the deep-sleep fallback */
            printf("hib test: armed rtc %u s, powering off\n", (unsigned)s);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            go_down(s, POWER_BOOT_HIB_WAKE, HIB_MODE_POWEROFF);
        } else {
            return 1;
        }
        printf("hib test: still awake (the sleep failed)\n");
        return 1;
    }
#endif
    return 1;
}
