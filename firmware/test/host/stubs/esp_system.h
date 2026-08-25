/*
 * Host-test stub for ESP-IDF's esp_system.h. esp_reset_reason() reports a
 * non-software reset so boot_recovery_prepare() takes its plain-clears path,
 * and esp_restart() is a no-op (the boot-recovery restart path is never run).
 */
#ifndef HOST_STUB_ESP_SYSTEM_H
#define HOST_STUB_ESP_SYSTEM_H

#include "esp_err.h"

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);

/* Evaluate the checked expression (which has a side effect) and ignore the
 * result; the host test never wants the real abort-on-error behaviour. */
#define ESP_ERROR_CHECK(x)                                   \
    do                                                       \
    {                                                        \
        esp_err_t _err_ = (x);                               \
        (void)_err_;                                         \
    } while (0)

#endif /* HOST_STUB_ESP_SYSTEM_H */
