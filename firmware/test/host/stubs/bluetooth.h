#ifndef HOST_STUB_BLUETOOTH_H
#define HOST_STUB_BLUETOOTH_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t bluetooth_init(void);
esp_err_t bluetooth_stop(void);
bool bluetooth_is_connected(void);

#endif /* HOST_STUB_BLUETOOTH_H */
