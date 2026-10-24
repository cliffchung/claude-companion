#include "display_ctl.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "bsp/m5stack_tab5.h"
#include "lvgl.h"

static const char *TAG = "display_ctl";

/* esp_lvgl_port 2.8.0~1, src/lvgl9: the first fields of lvgl_port_display_ctx_t and
   lvgl_port_touch_ctx_t (the display's and the touch indev's driver data). */
typedef struct {
    int disp_type;                          /* lvgl_port_disp_type_t */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} port_disp_head_t;
typedef struct {
    esp_lcd_touch_handle_t handle;
} port_touch_head_t;

/* esp_lcd_touch_st7123.h and esp_lcd_touch_gt911.h are private to the BSP */
#define ST7123_ADDR 0x55
#define GT911_ADDR_BACKUP 0x14

esp_lcd_panel_handle_t display_ctl_panel(void)
{
    lv_display_t *d = lv_display_get_default();
    const port_disp_head_t *ctx = d ? lv_display_get_driver_data(d) : NULL;
    return ctx ? ctx->panel_handle : NULL;
}

esp_lcd_touch_handle_t display_ctl_touch(void)
{
    lv_indev_t *in = bsp_display_get_input_dev();
    const port_touch_head_t *ctx = in ? lv_indev_get_driver_data(in) : NULL;
    return ctx ? ctx->handle : NULL;
}

esp_err_t display_ctl_panel_on(bool on)
{
    esp_lcd_panel_handle_t p = display_ctl_panel();
    if (!p) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_disp_on_off(p, on);
}

int display_ctl_board_version(void)
{
    static int s_ver = -1;
    if (s_ver >= 0) return s_ver;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    int ver = 0;
    if (bus && i2c_master_probe(bus, ST7123_ADDR, 100) == ESP_OK) {
        const esp_lcd_panel_io_i2c_config_t cfg = {
            .scl_speed_hz = 100000,
            .dev_addr = ST7123_ADDR,
            .control_phase_bytes = 1,
            .lcd_cmd_bits = 16,
            .flags = { .disable_control_phase = 1 },
        };
        esp_lcd_panel_io_handle_t io = NULL;
        uint8_t fw = 0;
        if (esp_lcd_new_panel_io_i2c(bus, &cfg, &io) == ESP_OK) {
            if (esp_lcd_panel_io_rx_param(io, 0x0000, &fw, 1) == ESP_OK) ver = fw == 3 ? 2 : fw == 1 ? 3 : 0;
            esp_lcd_panel_io_del(io);
        }
        ESP_LOGI(TAG, "ST712x touch FW %u: board v%d", fw, ver);
    } else if (bus && i2c_master_probe(bus, GT911_ADDR_BACKUP, 100) == ESP_OK) {
        ver = 1;
        ESP_LOGI(TAG, "GT911 touch: board v1");
    } else {
        ESP_LOGW(TAG, "no known touch controller: board unknown");
    }
    s_ver = ver;
    return ver;
}

void display_ctl_touch_int_init(void (*isr)(void *))
{
    int v = display_ctl_board_version();
    if (v != 1) {
        ESP_LOGI(TAG, "touch wake: polling on board v%d", v);
        return;
    }
    /* as generative-tab5 prov_device.c touch_interrupt_init: the BSP leaves it output-low */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_LCD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_isr_handler_add(BSP_LCD_TOUCH_INT, isr, NULL);
    ESP_LOGI(TAG, "touch wake: GT911 INT on GPIO%d", BSP_LCD_TOUCH_INT);
}

esp_err_t display_ctl_touch_poll(bool *pressed)
{
    *pressed = false;
    esp_lcd_touch_handle_t tp = display_ctl_touch();
    if (!tp) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_lcd_touch_read_data(tp);
    if (e != ESP_OK) return e;
    esp_lcd_touch_point_data_t pt[1];
    uint8_t cnt = 0;
    e = esp_lcd_touch_get_data(tp, pt, &cnt, 1);
    if (e != ESP_OK) return e;
    *pressed = cnt > 0;
    return ESP_OK;
}
