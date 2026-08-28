/*
 * Host-test stub for battery.h. Mirrors the real prototypes; display tests
 * provide test-settable read functions and no-op lifecycle functions.
 */
#ifndef HOST_STUB_BATTERY_H
#define HOST_STUB_BATTERY_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t battery_init(void);
esp_err_t battery_service(void);
float battery_voltage(void);
int battery_soc(void);
bool battery_is_low(void);
bool battery_is_charging(void);

#endif /* HOST_STUB_BATTERY_H */
