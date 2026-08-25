/*
 * Host-test stub for ESP-IDF's nvs.h. Mirrors only the non-volatile-storage
 * symbols app_main.c's boot-recovery path references; the host test provides
 * no-op definitions so the real app_main.c compiles without ESP-IDF.
 */
#ifndef HOST_STUB_NVS_H
#define HOST_STUB_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#endif /* HOST_STUB_NVS_H */
