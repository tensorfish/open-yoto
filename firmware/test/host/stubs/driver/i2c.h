/* Host-test stub for the legacy ESP-IDF I2C master helpers used by battery.c. */
#ifndef HOST_STUB_DRIVER_I2C_H
#define HOST_STUB_DRIVER_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int i2c_port_t;

typedef struct
{
    int mode;
    int sda_io_num;
    int scl_io_num;
    int sda_pullup_en;
    int scl_pullup_en;
    struct
    {
        uint32_t clk_speed;
    } master;
} i2c_config_t;

#define I2C_MODE_MASTER 1
#define GPIO_PULLUP_ENABLE 1

esp_err_t i2c_param_config(i2c_port_t port, const i2c_config_t *config);
esp_err_t i2c_driver_install(i2c_port_t port, int mode,
                             size_t rx_buf_len, size_t tx_buf_len,
                             int intr_alloc_flags);

esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t address,
                                       const uint8_t *write_buffer,
                                       size_t write_size,
                                       uint8_t *read_buffer,
                                       size_t read_size,
                                       TickType_t timeout);
esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t address,
                                     const uint8_t *write_buffer,
                                     size_t write_size,
                                     TickType_t timeout);

#endif /* HOST_STUB_DRIVER_I2C_H */
