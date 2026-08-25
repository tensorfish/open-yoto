/*
 * Host-test stub for display.h. Mirrors the logical-display API app_main.c
 * uses; the host test provides the definitions (display_show_rgba records the
 * frame pointer, display_draw_volume_overlay records the volume, the rest are
 * no-ops or counters).
 */
#ifndef HOST_STUB_DISPLAY_H
#define HOST_STUB_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_init(void);
void display_clear(void);
void display_set_pixel(int x, int y, bool on);
void display_flush(void);
void display_show_rgba(const uint8_t rgba[16 * 16 * 4]);
void display_draw_volume_overlay(int volume);
esp_err_t display_show_rgb56516(const uint16_t pixels[16 * 16]);
esp_err_t display_color64_begin(void);
esp_err_t display_color64_write_row(uint8_t y, const uint16_t pixels[64]);
esp_err_t display_color64_end(void);

#define DISPLAY_ACCESS_CODE_LEN 6

void display_show_access_code(
    const char code[DISPLAY_ACCESS_CODE_LEN + 1]);

#endif /* HOST_STUB_DISPLAY_H */
