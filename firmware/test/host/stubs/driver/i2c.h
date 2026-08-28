/* Host-test stub for the legacy ESP-IDF I2C master helpers used by battery.c. */
#ifndef HOST_STUB_DRIVER_I2C_H
#define HOST_STUB_DRIVER_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int i2c_port_t;

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
