/*
 * display.c — 16x16 logical display abstraction.
 *
 * Rev #04: GC9306 TFT. The 16x16 one-bit framebuffer is upscaled 15x on
 * flush: 16*15 = 240, matching the panel width, and the 320-tall panel is
 * centered with a vertical offset of 40 ((320 - 240) / 2). The backlight
 * rail is AC-coupled, so it must be PWM-driven at 40 kHz (stock firmware
 * literal 0x9C40 @ 0x400d4fd4); a plain GPIO high passes nothing and a
 * lower frequency is attenuated by the coupling cap.
 *
 * Rev #05: HT16D35x LED matrix — pure forwarding to the ht16d35x driver.
 */
#include "display.h"

#ifdef CONFIG_BOARD_REV_04

#include <string.h>

#include "driver/ledc.h"

#include "board_pins.h"
#include "gc9306.h"

#define DISPLAY_TFT_SCALE   15
#define DISPLAY_TFT_OFFSET_Y 40

static bool s_fb[16][16];

esp_err_t display_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 40000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK)
    {
        return err;
    }

    ledc_channel_config_t ledc_chan = {
        .gpio_num   = PIN_DISPLAY_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 255,   /* full brightness */
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ledc_chan);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gc9306_init();
    if (err != ESP_OK)
    {
        return err;
    }
    return gc9306_display_on();
}

void display_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void display_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= 16 || y < 0 || y >= 16)
    {
        return;
    }
    s_fb[y][x] = on;
}

void display_flush(void)
{
    gc9306_fill_rect(0, 0, 239, 319, 0x000000);
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            if (s_fb[y][x])
            {
                gc9306_fill_rect(x * DISPLAY_TFT_SCALE,
                                 y * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_Y,
                                 x * DISPLAY_TFT_SCALE + DISPLAY_TFT_SCALE - 1,
                                 y * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_Y
                                     + DISPLAY_TFT_SCALE - 1,
                                 0xFFFFFF);
            }
        }
    }
}

#else /* CONFIG_BOARD_REV_04 */

#include "ht16d35x.h"

esp_err_t display_init(void)
{
    return ht16d35x_init();
}

void display_clear(void)
{
    ht16d35x_clear();
}

void display_set_pixel(int x, int y, bool on)
{
    ht16d35x_set_pixel(x, y, on);
}

void display_flush(void)
{
    ht16d35x_flush();
}

#endif /* CONFIG_BOARD_REV_04 */
