#ifndef HOST_STUB_BLUETOOTH_H
#define HOST_STUB_BLUETOOTH_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    esp_err_t init_result;
    esp_err_t stop_result;
    unsigned init_calls;
    unsigned stop_calls;
} bluetooth_host_stub_state_t;

extern bluetooth_host_stub_state_t bluetooth_host_stub;
void bluetooth_host_stub_reset(void);

esp_err_t bluetooth_init(void);
esp_err_t bluetooth_stop(void);
bool bluetooth_is_connected(void);

#endif /* HOST_STUB_BLUETOOTH_H */
