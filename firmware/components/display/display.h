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
 * 12× to its calibrated `(24,24)..(215,215)` test window, then transmits
 * RGB666 with the device-specific channel-order correction.
 *
 * @param[in] rgba 1024-byte 16x16 RGBA frame in row-major order.
 */
void display_show_rgba(const uint8_t rgba[16 * 16 * 4]);

/**
 * Render a packed 1-bit 192x192 raster at the stock GC9306 window. This is
 * intended for native-resolution display tests; the #05 path downsamples it.
 *
 * @param[in] mask Packed MSB-first raster, 192*192/8 bytes.
 * @param[in] foreground RGB888 colour for set bits.
 * @param[in] background RGB888 colour for clear bits.
 */
void display_show_mask192(const uint8_t mask[192 * 192 / 8],
                          uint32_t foreground, uint32_t background);

/**
 * Render a packed 1-bit full 240x320 physical raster without logical
 * downscaling. The #04 path writes it directly to the panel.
 */
void display_show_mask_full(const uint8_t mask[240 * 320 / 8],
                            uint32_t foreground, uint32_t background);
