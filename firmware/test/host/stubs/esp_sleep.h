#ifndef HOST_STUB_ESP_SLEEP_H
#define HOST_STUB_ESP_SLEEP_H

#include <stdint.h>
#include "esp_err.h"

typedef enum
{
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_EXT1 = 3,
} esp_sleep_wakeup_cause_t;

typedef enum
{
    ESP_EXT1_WAKEUP_ALL_LOW = 0,
} esp_sleep_ext1_wakeup_mode_t;

esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask,
                                         esp_sleep_ext1_wakeup_mode_t mode);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
esp_err_t esp_light_sleep_start(void);
void esp_deep_sleep_start(void);

#endif /* HOST_STUB_ESP_SLEEP_H */
