#include "imu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "imu";

#define BMI270_ADDR 0x68
#define REG_PWR_CONF 0x7C
#define REG_PWR_CTRL 0x7D

static i2c_master_dev_handle_t s_dev;

static esp_err_t dev(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BMI270_ADDR, .scl_speed_hz = 400000 };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

esp_err_t imu_reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t e = dev();
    if (e != ESP_OK) return e;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t reg_write(uint8_t reg, uint8_t v)
{
    esp_err_t e = dev();
    if (e != ESP_OK) return e;
    uint8_t w[2] = { reg, v };
    e = i2c_master_transmit(s_dev, w, 2, 50);
    esp_rom_delay_us(450);   /* with adv_power_save set the BMI270 needs 450 us between writes */
    return e;
}

esp_err_t imu_chip_id(uint8_t *id) { return imu_reg_read(0x00, id, 1); }

esp_err_t imu_suspend(void)
{
    esp_err_t e = reg_write(REG_PWR_CTRL, 0x00);
    if (e == ESP_OK) e = reg_write(REG_PWR_CONF, 0x03);
    if (e != ESP_OK) ESP_LOGW(TAG, "suspend: %s", esp_err_to_name(e));
    return e;
}
