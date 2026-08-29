#ifndef HOST_STUB_ESP_SLEEP_H
#define HOST_STUB_ESP_SLEEP_H

#include "esp_err.h"

esp_err_t esp_sleep_enable_ext0_wakeup(int gpio_num, int level);
void esp_deep_sleep_start(void);

#endif /* HOST_STUB_ESP_SLEEP_H */
