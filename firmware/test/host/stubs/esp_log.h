/*
 * Host-test stub for ESP-IDF's esp_log.h. The logging calls compile away,
 * but the tag argument is still referenced so display.c's static TAG remains
 * "used" under -Werror (otherwise -Wunused-variable rejects the build).
 */
#ifndef HOST_STUB_ESP_LOG_H
#define HOST_STUB_ESP_LOG_H

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

#endif /* HOST_STUB_ESP_LOG_H */
