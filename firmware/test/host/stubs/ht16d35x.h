/*
 * Host-test stub for ht16d35x.h. Mirrors the real header's prototypes so the
 * display component's rev05 path compiles unchanged; the host test provides
 * the definitions (set_pixel writes a shadow framebuffer and records its
 * calls, flush counts invocations).
 */
#ifndef HOST_STUB_HT16D35X_H
#define HOST_STUB_HT16D35X_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t ht16d35x_init(void);
void ht16d35x_clear(void);
void ht16d35x_set_pixel(int x, int y, bool on);
void ht16d35x_flush(void);

#endif /* HOST_STUB_HT16D35X_H */
