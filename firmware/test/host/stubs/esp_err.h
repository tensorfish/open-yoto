/*
 * Host-test stub for ESP-IDF's esp_err.h. Defines only the symbols the
 * display component's rev04 path references so the real display.c can be
 * compiled as a plain host translation unit (no ESP-IDF).
 */
#ifndef HOST_STUB_ESP_ERR_H
#define HOST_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK              0
#define ESP_FAIL           (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND  0x105
#define ESP_ERR_INVALID_RESPONSE 0x10a
#define ESP_ERR_TIMEOUT      0x107
#define ESP_ERR_NOT_SUPPORTED 0x108
#define ESP_ERR_NVS_NOT_FOUND      0x1100
#define ESP_ERR_NVS_NO_FREE_PAGES  0x1101
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1102

const char *esp_err_to_name(esp_err_t err);

#endif /* HOST_STUB_ESP_ERR_H */
