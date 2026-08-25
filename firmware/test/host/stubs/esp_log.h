/*
 * Host-test stub for ESP-IDF's esp_log.h. Log output is dropped, but the
 * arguments are still evaluated — exactly as real ESP-IDF does — so symbols
 * that appear only inside a logging call (e.g. boot_stage_name() or
 * esp_err_to_name() in app_main.c) still count as "used" under -Werror.
 */
#ifndef HOST_STUB_ESP_LOG_H
#define HOST_STUB_ESP_LOG_H

static inline void host_log_discard(const char *tag, ...)
{
    (void)tag;
}

#define ESP_LOGI(tag, ...) host_log_discard((tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log_discard((tag), ##__VA_ARGS__)
#define ESP_LOGE(tag, ...) host_log_discard((tag), ##__VA_ARGS__)

#endif /* HOST_STUB_ESP_LOG_H */
