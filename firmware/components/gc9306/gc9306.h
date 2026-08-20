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
 * Render a 16x16 RGBA frame exactly as the stock TFT scaler: alpha-premultiply
 * each source RGB channel, nearest-neighbour scale 12x, and write RGB666 to
 * the stock window (24,27)..(215,218).
 */
esp_err_t gc9306_draw_rgba16(const uint8_t rgba[16 * 16 * 4]);
