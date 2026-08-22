#pragma once

#include <stddef.h>
#include "esp_err.h"

#define YOTO_WELCOME_PATH "/system/sounds/welcome"

/** Register the stock-compatible read-only /system virtual filesystem. */
esp_err_t yoto_vfs_init(void);

/** Return the embedded welcome asset bytes for decoder-side validation. */
const unsigned char *yoto_vfs_welcome_data(size_t *size);
