#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* BMI270 at 0x68 on the BSP I2C bus, raw registers only (PT15/PT16: no motion wake, so no driver and
 * no feature blob). Nothing in the BSP or the fork starts it: after power-on it sits with the accel
 * and gyro off (PWR_CTRL 0) and advanced power save on (PWR_CONF 0x03). */

esp_err_t imu_reg_read(uint8_t reg, uint8_t *buf, size_t n);
esp_err_t imu_chip_id(uint8_t *id);         /* CHIP_ID 0x00, 0x24 on a BMI270 */
/* PWR_CTRL 0x7D = 0 (accel, gyro, aux, temperature off), PWR_CONF 0x7C = 0x03 (adv_power_save,
   fifo self-wakeup); its INT1 drives the power controller's trigger (E_TRG), so nothing may fire */
esp_err_t imu_suspend(void);

#ifdef __cplusplus
}
#endif
