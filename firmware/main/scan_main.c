/*
 * scan_main.c — I2C bus scanner bring-up firmware.
 *
 * Brings up the I2C bus (SDA=21, SCL=25) and logs every 7-bit address that
 * ACKs. No display, audio, NFC, or battery init — the scan itself is the
 * diagnostic, and the device idles forever afterwards (no aborts).
 *
 * Expected results by unit revision (see docs/hardware.md):
 *   #04 (TFT, single IOX):  0x08 (ES8156), 0x20 (IOX), 0x34 (aw881xx),
 *                           0x51 (RTC), 0x64 (CW2215B)
 *   #05 (LED, dual IOX):    0x09 (ES8156), 0x20 + 0x21 (IOX x2),
 *                           0x34 + 0x37 (aw881xx), 0x51 (RTC), 0x64 (CW2215B)
 *   SGM41513 charger (0x6b) only answers when VBUS is present (USB plugged).
 */
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

#include "board_pins.h"

static const char *TAG = "scan";

void app_main(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    ESP_LOGI(TAG, "=== I2C SCAN v1 (SDA=21 SCL=25) ===");
    for (int addr = 0x08; addr <= 0x77; addr++)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "0x%02x ACK", addr);
        }
    }
    ESP_LOGI(TAG, "=== I2C SCAN DONE; idling ===");

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
