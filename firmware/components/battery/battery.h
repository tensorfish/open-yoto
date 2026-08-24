/*
 * battery.h — CW2215B fuel gauge + SGM41513 charger monitoring.
 *
 * Reads battery voltage and state of charge from the CW2215B over the shared
 * I2C bus installed by iox_init(). Rev #04/#05 have no ADC battery pin.
 * Charger and battery-alert status come from the IO expander.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize and validate the CW2215B fuel gauge, then probe the optional
 * SGM41513 charger and read charger + battery-alert status from IOX.
 *
 * @return ESP_OK only when the expected gauge is ready and its initial VCELL
 *         and SOC readings are valid; ESP_ERR_NOT_FOUND when the gauge is
 *         absent or mismatched; ESP_ERR_INVALID_STATE for invalid readings;
 *         otherwise the gauge initialization error.
 */
esp_err_t battery_init(void);

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
 * True while the battery is actively charging (charger STAT line asserted).
 *
 * Reads the charger status pin on the IO expander. The active level is
 * assumed active-low (open-drain STAT) until the schematic confirms polarity.
 */
bool battery_is_charging(void);
