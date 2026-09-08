/* Shared synchronous I2C transport for the board's fixed peripherals. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Called by iox_init() before peripheral startup. Successful initialization is
 * idempotent and the bus lives until reboot. Concurrent initialization returns
 * ESP_ERR_INVALID_STATE; transfers cannot see a partially initialized bus. */
esp_err_t board_i2c_init(void);

/* Transfers are serialized by the ESP-IDF master driver's bus semaphore.
 * Addresses must be board peripherals registered at initialization. Timeouts
 * are milliseconds and must be positive (unbounded waits are not supported).
 * Buffers remain owned by the caller; all operations finish before returning. */
esp_err_t board_i2c_write(uint8_t address, const uint8_t *data, size_t size,
                          int timeout_ms);

/* One atomic write/repeated-START/read transaction, with no intervening STOP. */
esp_err_t board_i2c_write_read(uint8_t address, const uint8_t *write_data,
                               size_t write_size, uint8_t *read_data,
                               size_t read_size, int timeout_ms);

/* Probe any non-reserved 7-bit address without registering another device. */
esp_err_t board_i2c_probe(uint8_t address, int timeout_ms);
