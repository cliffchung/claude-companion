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
    /* INT1 drives the power controller's trigger (Q8 -> E_TRG): outputs off and nothing mapped first, then
       the latch read clear, so no stray level can switch the board on or hold the trigger (ruling P67) */
    static const uint8_t k_quiet[][2] = {
        { 0x53, 0x00 },   /* INT1_IO_CTRL: output_en off */
        { 0x54, 0x00 },   /* INT2_IO_CTRL */
        { 0x55, 0x00 },   /* INT_LATCH: non-latched */
        { 0x56, 0x00 },   /* INT1_MAP_FEAT */
        { 0x57, 0x00 },   /* INT2_MAP_FEAT */
        { 0x58, 0x00 },   /* INT_MAP_DATA */
    };
    esp_err_t e = ESP_OK;
    for (size_t i = 0; i < sizeof(k_quiet) / sizeof(k_quiet[0]) && e == ESP_OK; i++) e = reg_write(k_quiet[i][0], k_quiet[i][1]);
    uint8_t st[2];
    if (e == ESP_OK) e = imu_reg_read(0x1C, st, 2);   /* INT_STATUS_0/1: read clears */
    if (e == ESP_OK) e = reg_write(REG_PWR_CTRL, 0x00);
    if (e == ESP_OK) e = reg_write(REG_PWR_CONF, 0x03);
    if (e != ESP_OK) ESP_LOGW(TAG, "suspend: %s", esp_err_to_name(e));
    return e;
}
