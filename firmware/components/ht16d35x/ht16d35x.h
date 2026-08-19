/*
 * ht16d35x.h — HT16D35B 16x16 monochrome LED matrix driver.
 *
 * The Yoto display is a 16x16 LED matrix built from four HT16D35B controllers,
 * each wired to one 8x8 quadrant. The four controllers share a single SPI bus
 * (MOSI + SCLK only; the display is write-only) and are selected through the
 * second IO expander (IOX_DISP_CSN0..3) rather than the SPI peripheral.
 *
 * The driver keeps a 16x16 single-bit framebuffer. Callers mutate it with
 * ht16d35x_set_pixel()/ht16d35x_clear() and push it to the glass with
 * ht16d35x_flush().
 *
 * Preconditions: iox_init() must have completed before ht16d35x_init().
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize the shared SPI bus (half-duplex) and all four HT16D35B chips,
 * placing each in BINARY (1 bit/pixel) mode with its oscillator running.
 *
 * @return ESP_OK on success, or an ESP_ERR_* code from the SPI or IOX layer.
 */
esp_err_t ht16d35x_init(void);

/** Clear every pixel in the framebuffer. Does not touch the glass. */
void ht16d35x_clear(void);

/**
 * Set a single pixel in the framebuffer (0,0 is the top-left corner).
 *
 * @param[in] x  Column, 0..15.
 * @param[in] y  Row, 0..15.
 * @param[in] on True to light the pixel, false to clear it.
 */
void ht16d35x_set_pixel(int x, int y, bool on);

/** Push the framebuffer to all four HT16D35B chips over SPI. */
void ht16d35x_flush(void);
