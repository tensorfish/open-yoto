/*
 * display.h — 16x16 logical display abstraction.
 *
 * Routes the 16x16 logical display to the physical panel: the HT16D35x LED
 * matrix on rev #05, or the visible 240x240 GC9306 canvas on rev #04. The
 * basic API mirrors HT16D35x; callers mutate a 16x16 single-bit framebuffer
 * with display_set_pixel()/display_clear() and push it with display_flush().
 * display_show_rgba() sends an exact 16x16 RGBA icon through the colour
 * pipeline. Preconditions: iox_init() must have completed before display_init().
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
 * Enable or disable the persistent Bluetooth status indicator.
 *
 * Enabling draws immediately and subsequent renders preserve it. Disabling
 * removes it from subsequent renders; the caller repaints the current base.
 */
void display_set_bluetooth_indicator(bool enabled);

/**
 * Render a 16x16 RGBA icon through the revision's physical display path.
 * The rev #04 GC9306 path alpha-premultiplies RGB, nearest-neighbour scales
 * 15× to the visible `(0,0)..(239,239)` canvas, then transmits RGB666 with
 * the device-specific channel-order correction.
 *
 * The icon covers that canvas completely, so rev #04 blanks the rest of the
 * panel only when an earlier render could have lit it (a 1-bit bitmap flush or
 * a full 240x320 raster). Successive icon frames therefore cost the canvas
 * alone, which is what keeps an animation free of black flashes.
 *
 * @param[in] rgba 1024-byte 16x16 RGBA frame in row-major order.
 */
void display_show_rgba(const uint8_t rgba[16 * 16 * 4]);

/** Draw a transient blue-to-green-to-red volume bar over the current frame. */
void display_draw_volume_overlay(int volume);

/**
 * Render a 16x16 RGB565 icon in ordinary source orientation and RGB order.
 * Rev #04 scales it 15× to the visible square canvas; rev #05 converts it to
 * luminance.
 */
esp_err_t display_show_rgb56516(const uint16_t pixels[16 * 16]);

/**
 * Stream a 64x64 RGB565 image without allocating a full frame.
 *
 * Call begin once, write rows 0..63 in order, then call end even after a row
 * error. RGB565 values use ordinary source orientation and colour order.
 */
esp_err_t display_color64_begin(void);
esp_err_t display_color64_write_row(uint8_t y,
                                    const uint16_t pixels[64]);
esp_err_t display_color64_end(void);

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

#define DISPLAY_ACCESS_CODE_LEN 6

/**
 * Render a six-character alphanumeric access code.
 *
 * Rev #04 uses a native 5x7 font on the 240x320 TFT. Rev #05 uses a compact
 * two-row fallback on the 16x16 LED matrix.
 */
void display_show_access_code(
    const char code[DISPLAY_ACCESS_CODE_LEN + 1]);
