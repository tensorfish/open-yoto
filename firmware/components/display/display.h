/*
 * display.h — 16x16 logical display abstraction.
 *
 * Routes the 16x16 logical display to the physical panel: the HT16D35x LED
 * matrix on the rev #05 board, or the GC9306 TFT (stock 12x, 192x192 at
 * (24,27)) on the rev #04 board. The basic API mirrors HT16D35x; callers
 * mutate a 16x16 single-bit framebuffer with display_set_pixel()/
 * display_clear() and push it with display_flush(). display_show_rgba()
 * sends an exact 16x16 RGBA icon through the stock-like colour pipeline.
 * Preconditions: iox_init() must have completed before display_init().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Initialize the physical display for the current board revision.
 *
 * @return ESP_OK on success, or an ESP_ERR_* code from the underlying
 *         display, LEDC, SPI, or IOX layer.
 */
esp_err_t display_init(void);

/** Clear every pixel in the framebuffer. Does not touch the panel. */
void display_clear(void);

/**
 * Set a single pixel in the framebuffer (0,0 is the top-left corner).
 *
 * @param[in] x  Column, 0..15.
 * @param[in] y  Row, 0..15.
 * @param[in] on True to light the pixel, false to clear it.
 */
void display_set_pixel(int x, int y, bool on);

/** Push the framebuffer to the physical panel. */
void display_flush(void);

/**
 * Render a 16x16 RGBA icon through the revision's physical display path.
 * The rev #04 GC9306 path alpha-premultiplies RGB, nearest-neighbour scales
 * 12x to the stock window (24,27)..(215,218), then transmits RGB666.
 *
 * @param[in] rgba 1024-byte 16x16 RGBA frame in row-major order.
 */
void display_show_rgba(const uint8_t rgba[16 * 16 * 4]);
