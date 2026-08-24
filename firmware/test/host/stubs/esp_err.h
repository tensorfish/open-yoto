/*
 * Host-test stub for ESP-IDF's esp_err.h. Defines only the symbols the
 * display component's rev04 path references so the real display.c can be
 * compiled as a plain host translation unit (no ESP-IDF).
 */
#ifndef HOST_STUB_ESP_ERR_H
#define HOST_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK              0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

const char *esp_err_to_name(esp_err_t err);

#endif /* HOST_STUB_ESP_ERR_H */
