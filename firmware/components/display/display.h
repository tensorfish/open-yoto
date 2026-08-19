/*
 * display.h — HT16D35x 16x16 LED matrix driver (SPI + IO-expander CS).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/** Initialize the SPI bus + HT16D35x device and clear the framebuffer. */
esp_err_t display_init(void);

/** Clear the 16x16 grayscale framebuffer (all pixels off). */
void display_clear(void);

/** Set one pixel's 6-bit grayscale value (0..63). Coordinates are 0..15. */
void display_set_pixel(int x, int y, uint8_t gray);

/** Encode the framebuffer and push it to the HT16D35x over SPI. */
void display_flush(void);
