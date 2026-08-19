/*
 * battery.h — CW2215B fuel gauge + SGM41513 charger + ADC monitoring.
 *
 * Reads the raw battery sense voltage through ADC1, probes the fuel gauge and
 * charger on the shared I2C bus (installed by iox_init), and surfaces charger
 * / battery-alert status from the IO expander.
 */
#pragma once

#include "esp_err.h"

/**
 * Initialize ADC1 (VBAT / light / IR-temp channels), probe the CW2215B fuel
 * gauge and SGM41513 charger, and read the charger + battery-alert status
 * from the IO expander. Logs the state of each peripheral.
 */
esp_err_t battery_init(void);

/** Battery voltage in millivolts (stub: ADC-derived, real VCELL read TODO). */
float battery_voltage(void);

/** Battery state of charge, 0..100 % (stub: real SOC register read TODO). */
int battery_soc(void);
