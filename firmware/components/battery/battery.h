/*
 * battery.h — CW2215B fuel gauge + SGM41513 charger control.
 *
 * Reads battery voltage and state of charge from the CW2215B over the shared
 * I2C bus installed by iox_init(). Rev #04/#05 have no ADC battery pin.
 * External-power and charging state come from the SGM41513 status register;
 * the charger is configured whenever its power-good state appears.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize and validate the CW2215B fuel gauge, then probe the optional
 * SGM41513 charger and read the raw IO-expander battery signals for diagnostics.
 *
 * @return ESP_OK only when the expected gauge is ready and its initial VCELL
 *         and SOC readings are valid; ESP_ERR_NOT_FOUND when the gauge is
 *         absent or mismatched; ESP_ERR_INVALID_STATE for invalid readings;
 *         otherwise the gauge initialization error.
 */
esp_err_t battery_init(void);

/**
 * Detect SGM41513 power-good changes and configure it after each plug-in.
 *
 * Safe to call periodically. REG08 is sampled each time; losing power-good
 * clears the cached configuration so a later plug-in restores the exact
 * board-specific settings.
 */
esp_err_t battery_service(void);

/**
 * Battery voltage in millivolts from the CW2215B VCELL register. Returns 0
 * when the gauge is absent or its data is invalid.
 */
float battery_voltage(void);

/**
 * Battery state of charge, 0..100 %. Returns -1 when the gauge is absent or
 * the SOC register is not valid.
 */
int battery_soc(void);

/**
 * True when the battery is low: SOC below 15 % or cell voltage below the
 * low-voltage threshold.
 */
bool battery_is_low(void);

/**
 * True while the SGM41513 reports pre-charge or fast-charge state.
 *
 * Uses the cached REG08 power-good and charging-state bits refreshed by
 * battery_service(), rather than the board's unreliable STAT/plug pins.
 */
bool battery_is_charging(void);
