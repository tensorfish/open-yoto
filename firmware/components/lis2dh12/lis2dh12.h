/*
 * lis2dh12.h — Stock Yoto LIS2DH12 accelerometer startup.
 */
#pragma once

#include "esp_err.h"

/** Apply the register state used by the factory firmware during board startup. */
esp_err_t lis2dh12_init(void);
