/*
 * Host-test stub for content.h. Mirrors only the symbols app_main.c uses; the
 * host test provides no-op/false definitions.
 */
#ifndef HOST_STUB_CONTENT_H
#define HOST_STUB_CONTENT_H

#include <stddef.h>

#include "esp_err.h"

#define CONTENT_MOUNT_POINT "/sdcard"

esp_err_t content_init(void);
int content_get_track_count(const char *url);
esp_err_t content_get_track(const char *url, int index,
                            char *sound_path, size_t sp);
esp_err_t content_get_track_image(const char *url, int index,
                                  char *image_path, size_t ip);

#endif /* HOST_STUB_CONTENT_H */
