/*
 * gc9306.h — GC9306 TFT LCD driver (rev #04 board).
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t gc9306_init(void);
esp_err_t gc9306_display_on(void);
esp_err_t gc9306_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint32_t color);

/**
 * Render a 16x16 RGBA frame with alpha-premultiplication and 12×
 * nearest-neighbour scaling. The replacement places its 192px test frame at
 * (24,24) after the measured vertical calibration, then writes RGB666.
 */
esp_err_t gc9306_draw_rgba16(const uint8_t rgba[16 * 16 * 4]);

/** Stream a 64x64 RGB565 frame scaled 3x into the 192x192 content window. */
esp_err_t gc9306_color64_begin(void);
esp_err_t gc9306_color64_write_row(const uint16_t pixels[64]);
esp_err_t gc9306_color64_end(void);

/**
 * Render a packed 1-bit 192x192 mask into the stock window
 * (24,27)..(215,218), using direct RGB666 foreground/background pixels.
 */
esp_err_t gc9306_draw_mask192(const uint8_t mask[192 * 192 / 8],
                              uint32_t foreground, uint32_t background);

/** Render a packed 1-bit full 240x320 physical raster. */
esp_err_t gc9306_draw_mask_full(const uint8_t mask[240 * 320 / 8],
                                uint32_t foreground, uint32_t background);
