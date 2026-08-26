/*
 * Host-test stub for iox.h. app_main.c uses the power-rail control API; the
 * host test provides no-op definitions.
 */
#ifndef HOST_STUB_IOX_H
#define HOST_STUB_IOX_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t iox_init(void);
esp_err_t iox_set_peripherals_powered(bool powered);

#endif /* HOST_STUB_IOX_H */
