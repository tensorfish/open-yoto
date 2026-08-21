/*
 * lis2dh12.c — Stock Yoto LIS2DH12 accelerometer startup.
 *
 * Factory app function 0x400daf40 writes CTRL_REG1=0x47 and CTRL_REG4=0x08
 * to the device at 7-bit address 0x18 before audio-board initialization.
 */
#include "lis2dh12.h"

#include <stdint.h>

#include "board_pins.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LIS2DH12_REG_CTRL1 0x20
#define LIS2DH12_REG_CTRL4 0x23
#define LIS2DH12_I2C_TIMEOUT_MS 100

static const char *TAG = "lis2dh12";

static esp_err_t lis2dh12_write(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = { reg, value };

    return i2c_master_write_to_device(I2C_PORT, I2C_ADDR_ACCELEROMETER,
                                      data, sizeof(data),
                                      pdMS_TO_TICKS(LIS2DH12_I2C_TIMEOUT_MS));
}

esp_err_t lis2dh12_init(void)
{
    esp_err_t err = lis2dh12_write(LIS2DH12_REG_CTRL1, 0x47);

    if (err == ESP_OK)
    {
        err = lis2dh12_write(LIS2DH12_REG_CTRL4, 0x08);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "stock register setup failed at 0x%02x: %s",
                 I2C_ADDR_ACCELEROMETER, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized at 0x%02x (CTRL1=0x47 CTRL4=0x08)",
             I2C_ADDR_ACCELEROMETER);
    return ESP_OK;
}
