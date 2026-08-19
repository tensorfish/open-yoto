/*
 * battery.h — CW2215B fuel gauge + SGM41513 charger + ADC monitoring.
 *
 * Reads the battery voltage and state of charge from the CW2215B fuel gauge
 * over the shared I2C bus (installed by iox_init), falling back to the ADC1
 * VBAT sense channel when the gauge is absent or unreadable. Also surfaces
 * charger / battery-alert status from the IO expander.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize ADC1 (VBAT / light / IR-temp channels), read the CW2215B fuel
 * gauge chip ID, VCELL, and SOC, probe the SGM41513 charger, and read the
 * charger + battery-alert status from the IO expander. Logs each peripheral's
 * state.
 */
esp_err_t battery_init(void);

/**
 * Battery voltage in millivolts. Prefers the CW2215B VCELL register; falls
 * back to the ADC1 VBAT sense channel if the gauge is absent or fails.
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
