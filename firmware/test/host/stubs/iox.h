/*
 * Host-test stub for the I/O-expander calls used by app_main.c and battery.c.
 * Each host test supplies its own implementation.
 */
#ifndef HOST_STUB_IOX_H
#define HOST_STUB_IOX_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t iox_init(void);
esp_err_t iox_set_peripherals_powered(bool powered);
esp_err_t iox_set_pin(uint8_t pin, bool level);
esp_err_t iox_prepare_power_button_wake(void);
bool iox_get_pin(uint8_t pin);
esp_err_t iox_read_port(uint8_t expander, uint8_t port, uint8_t *value);

#endif /* HOST_STUB_IOX_H */
