/*
 * Host-test stub for battery.h. Mirrors the real prototypes; the host test
 * provides test-settable definitions for the four read functions plus a no-op
 * battery_init().
 */
#ifndef HOST_STUB_BATTERY_H
#define HOST_STUB_BATTERY_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t battery_init(void);
float battery_voltage(void);
int battery_soc(void);
bool battery_is_low(void);
bool battery_is_charging(void);

#endif /* HOST_STUB_BATTERY_H */
