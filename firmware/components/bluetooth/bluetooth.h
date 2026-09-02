#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Initialize the Classic Bluetooth A2DP sink. */
esp_err_t bluetooth_init(void);

/** Stop the A2DP sink and release the Bluetooth host and controller. */
esp_err_t bluetooth_stop(void);

/** Return true while an A2DP source is connected. */
bool bluetooth_is_connected(void);
