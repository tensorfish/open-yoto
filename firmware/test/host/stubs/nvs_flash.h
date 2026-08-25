/*
 * Host-test stub for ESP-IDF's nvs_flash.h. The host test provides no-op
 * definitions so app_main.c's init/erase path compiles without ESP-IDF.
 */
#ifndef HOST_STUB_NVS_FLASH_H
#define HOST_STUB_NVS_FLASH_H

#include "esp_err.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

#endif /* HOST_STUB_NVS_FLASH_H */
