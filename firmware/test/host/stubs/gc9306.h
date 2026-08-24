/*
 * Host-test stub for gc9306.h. Mirrors the real header's prototypes so the
 * display component compiles unchanged; the host test provides definitions
 * (gc9306_fill_rect records its calls, the rest succeed).
 */
#ifndef HOST_STUB_GC9306_H
#define HOST_STUB_GC9306_H

#include "esp_err.h"
#include <stdint.h>

esp_err_t gc9306_init(void);
esp_err_t gc9306_display_on(void);
esp_err_t gc9306_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint32_t color);
esp_err_t gc9306_draw_rgba16(const uint8_t rgba[16 * 16 * 4]);
esp_err_t gc9306_color64_begin(void);
esp_err_t gc9306_color64_write_row(const uint16_t pixels[64]);
esp_err_t gc9306_color64_end(void);
esp_err_t gc9306_draw_mask192(const uint8_t mask[192 * 192 / 8],
                              uint32_t foreground, uint32_t background);
esp_err_t gc9306_draw_mask_full(const uint8_t mask[240 * 320 / 8],
                                uint32_t foreground, uint32_t background);

#endif /* HOST_STUB_GC9306_H */
