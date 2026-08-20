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
