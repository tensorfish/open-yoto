/*
 * iox.h — PI4IOE5V6416 16-bit I2C IO expander driver (2 chips).
 *
 * The stock firmware drives buttons, the display chip-selects, audio amp
 * enable, and the power-control rails through these expanders.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize the I2C master bus and both PI4IOE5V6416 expanders, applying the
 * factory direction/data defaults recovered from the stock firmware.
 */
esp_err_t iox_init(void);

/** Set a single expander pin (use IOX_PIN(...) from board_pins.h). */
esp_err_t iox_set_pin(uint8_t pin, bool level);

/**
 * Disconnect or restore the board's downstream peripheral rails while keeping
 * the IO expander and power button available to the ESP32.
 */
esp_err_t iox_set_peripherals_powered(bool powered);

/** Read a single expander pin (use IOX_PIN(...) from board_pins.h). */
bool iox_get_pin(uint8_t pin);

/** Read a whole 8-bit port of an expander (0..1). */
esp_err_t iox_read_port(uint8_t expander, uint8_t port, uint8_t *value);

/** Read an output latch for diagnostics (expander and port are 0..1). */
esp_err_t iox_read_output_port(uint8_t expander, uint8_t port, uint8_t *value);
