#include "ssh/device_key.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "settings/settings.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "ui/ui.h"
#include "ui/ui_boot.h"
#include "sys/sysinfo.h"
#include "net/wifi_mgr.h"
#include "input/keyboard.h"
#include "servers/server_store.h"
#include "crypto/secure_store.h"
#include "sys/screenshot.h"
#include "sys/debug_console.h"
#include "sys/power_log.h"
#include "sys/power_bench.h"
#include "sys/power_console.h"
#include "sys/power.h"
#include "sys/hibernate.h"
#include "sys/checkin.h"
#include "sys/imu.h"
#include "sys/display_ctl.h"
#include "sys/usb_mgr.h"
#include "net/tailscale.h"
#include "companion/cc_link.h"
#include "sys/boot_phase.h"
#include "sys/link_down.h"

static void wifi_global_cb(wifi_ev_t ev, void *user)
{
    (void)user;
    if (ev == WIFI_EV_CONNECTED) {
        /* a Radio-off check-in (fast resume): no rebind (it handshakes first) and no SNTP restart every
           few minutes (the RTC keeps the time across a park) */
        bool fast = tailscale_fast_resume();
        if (tailscale_enabled()) tailscale_on_wifi(true);   /* the switch is off: MicroLink never starts */
        if (!fast) sysinfo_start_sntp();
        cc_link_on_wifi(true);
    } else if (ev == WIFI_EV_DISCONNECTED) {
        cc_link_on_wifi(false);
    }
}

static const char *TAG = "app";
static lv_display_t *s_display;
static int64_t s_boot_logo_visible_at;
static bool s_dark_boot;   /* a Hibernate check-in boot (PT16): the screen stays dark throughout */

#define BOOT_LOGO_MIN_VISIBLE_MS 1000

static bool display_init_early(void)
{
    int64_t display_started = esp_timer_get_time();
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 16384,
            .task_affinity = 0,
            .task_max_sleep_ms = 500,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
            .timer_period_ms = 5,
        },
        /* Partial draw buffers (50 lines, double) in PSRAM: internal RAM is too
         * tight on this board once esp_hosted and libssh are in, and DMA2D can
         * read PSRAM on the P4. */
        .buffer_size = BSP_LCD_H_RES * 50,
        .double_buffer = true,
        .flags = { .buff_dma = false, .buff_spiram = true, .sw_rotate = true },
    };
    s_display = bsp_display_start_with_config(&cfg);
    if (!s_display) { ESP_LOGE(TAG, "display init failed"); return false; }
    /* Settings have deliberately not been loaded yet.  Use the board's normal
     * landscape orientation for the earliest possible boot logo, then apply
     * the saved orientation after NVS is ready. */
    bsp_display_lock(0);
    lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_90);
    int64_t logo_started = esp_timer_get_time();
    ui_boot_show();
    bsp_display_unlock();
    /* Draw before lighting the panel to avoid a blank flash. */
    bsp_display_brightness_set(s_dark_boot ? 0 : 80);
    s_boot_logo_visible_at = esp_timer_get_time();
    ESP_LOGI(TAG, "display ready in %lld ms; boot logo submitted in %lld ms",
             (esp_timer_get_time() - display_started) / 1000,
             (esp_timer_get_time() - logo_started) / 1000);
    return true;
}

static void display_apply_settings(void)
{
    bsp_display_lock(0);
    lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_90);   /* landscape, USB-C on the left (Plan 4: fixed) */
    lv_refr_now(s_display);
    bsp_display_unlock();
    bsp_display_brightness_set(s_dark_boot ? 0 : g_settings.brightness);
}

static void boot_logo_wait_minimum(void)
{
    if (s_dark_boot) return;   /* nobody sees the logo */
    int64_t elapsed_us = esp_timer_get_time() - s_boot_logo_visible_at;
    int64_t remaining_us = (int64_t)BOOT_LOGO_MIN_VISIBLE_MS * 1000 - elapsed_us;
    if (remaining_us > 0)
        vTaskDelay(pdMS_TO_TICKS((remaining_us + 999) / 1000));
}

static void idle_task(void *arg)
{
    (void)arg;
    power_poll_task_set(xTaskGetCurrentTaskHandle());
    for (;;) {
        sysinfo_poll();
        checkin_on_sample();   /* a check-in's mWh */
        uint8_t tag = power_bench_tag();
        if (!tag) tag = checkin_log_tag();   /* 0x83, 0x84 or 0x85 while a check-in runs */
        power_log_sample(tag ? tag : (uint8_t)power_state());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(power_poll_period_ms()));
    }
}

void app_main(void)
{
    boot_phase_init();
    int64_t boot_started = esp_timer_get_time();
    ESP_LOGI(TAG, "Claude Companion starting");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);   /* keyboard/INA226 probes are expected to NACK when absent */
    /* Bring up only the hardware required by the panel.  Touch must be powered
     * before bsp_display_start_with_config(): the BSP identifies the Tab5 board
     * revision by probing that controller and asserts if it is still starting. */
    bsp_i2c_init();
    hibernate_boot_early();   /* before any rail: a bench probe boot samples the INA226 and sleeps again here */
    s_dark_boot = hibernate_boot_kind() == POWER_BOOT_HIB_CHECKIN;
    bsp_io_expander_init();
    {   /* PT22: CHG_EN (0x44 P7, IP2326 EN via R59) high: the charger is off at the expander's pull-down. Polarity
           from M5Tab5-UserDemo bsp_set_charge_en(true) and M5Unified Power_Class::setBatteryCharge (P7 high =
           charge); measured -462 mA into the pack on USB (brownout-report.md). The driver's reset in
           bsp_io_expander1_init() has already made every pin an output (hi-Z, latch 0), so the level goes into
           the latch first and push-pull then drives it: no glitch, and set_dir is a no-op (chgfix M3).
           Hibernate leaves it alone (hibernate.c k_rails) and, a deep sleep (PT26), never drops it. */
        esp_io_expander_handle_t ex1 = bsp_io_expander1_init();
        bool ok = ex1 && esp_io_expander_set_level(ex1, 1u << 7, 1) == ESP_OK;
        ok = ok && esp_io_expander_set_output_mode(ex1, 1u << 7, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) == ESP_OK;
        ok = ok && esp_io_expander_set_dir(ex1, 1u << 7, IO_EXPANDER_OUTPUT) == ESP_OK;
        if (!ok) ESP_LOGW(TAG, "CHG_EN (0x44 P7): not driven high, the charger may be off");
    }
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    vTaskDelay(pdMS_TO_TICKS(50));   /* stagger the rails: four at once is a current spike on battery */
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Keep the board's proven rail-enable sequence.  These are only expander
     * writes; the Wi-Fi and USB software stacks still start after the logo. */
    bsp_feature_enable(BSP_FEATURE_WIFI, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_feature_enable(BSP_FEATURE_USB, true);
    if (!display_init_early()) return;

    /* Everything below runs while the already-rendered logo remains visible. */
    settings_init();
    power_log_boot();
    display_apply_settings();
    secure_store_init();
    sysinfo_init();
    imu_suspend();   /* nothing uses the BMI270 (PT15); it keeps whatever an earlier firmware left running */
    /* No sd_mount here (spec §7.1, P4-7): the card is mounted on demand (Load from SD, a key file on the card) */
    server_store_init();   /* NVS only */
    settings_post_load_migrate(s_dark_boot);   /* CFG 23: the hosts an older build kept on the card (not on a check-in) */

    bsp_display_lock(0);
    fonts_init();
    keyboard_init();
    bsp_display_unlock();

    wifi_mgr_set_global_cb(wifi_global_cb, NULL);
    link_down_init();   /* final review C1: every link-down path closes SSH first */
    wifi_mgr_init();
    usb_mgr_init();
    tailscale_init();
    /* Services are initialized; connection establishment continues in their
     * background tasks. Keep the logo for one second from backlight-on, with
     * all initialization time above counting toward that interval. */
    boot_logo_wait_minimum();
    bsp_display_lock(0);
    ui_init();
    lv_display_trigger_activity(NULL);
    ui_nav_home();
    bsp_display_unlock();
    debug_console_start();
    key_console_init();
    device_key_console_init();
    cc_console_init();
    power_console_init();
    if (s_dark_boot && (boot_phase_exp() & BPX_NO_HOSTCACHE)) cc_link_host_cache_enable(false);
    if (cc_link_start() != ESP_OK) ESP_LOGE(TAG, "companion link failed to start");
    hibernate_init();        /* boot-to-live after a hibernate */
    power_bench_boot();      /* a probe run that just finished */
    display_ctl_touch_int_init(power_touch_isr);   /* v1 only; v2 and v3 poll the touch in Off */
    power_init();
    xTaskCreate(idle_task, "idle_poll", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "boot complete in %lld ms, free heap %lu",
             (esp_timer_get_time() - boot_started) / 1000, (unsigned long)esp_get_free_heap_size());
}
