/*
 * display.c — 16x16 logical display abstraction.
 *
 * Rev #04: GC9306 TFT. The 16x16 one-bit framebuffer is upscaled by 12
 * (192x192, the stock firmware's geometry) and drawn at (24, 27). The
 * backlight rail is AC-coupled, so it must be PWM-driven at 40 kHz (stock
 * firmware literal 0x9C40 @ 0x400d4fd4); a plain GPIO high passes nothing
 * and a lower frequency is attenuated by the coupling cap.
 *
 * Rev #05: HT16D35x LED matrix — pure forwarding to the ht16d35x driver.
 */
#include "display.h"

#ifdef CONFIG_BOARD_REV_04

#include <string.h>

#include "esp_log.h"
#include "driver/ledc.h"

#include "board_pins.h"
#include "gc9306.h"

static const char *TAG = "display";

/* Stock rendering model (recovered from the factory image, display-module
 * init @ 0x40107e11): the 16x16 RGBA icon is upscaled by 12 (192x192) and
 * drawn at (24, 27) — a2[16/18]=192, a2[20]=24, a2[22]=27, a2[88]=12
 * (192>>4). The upscale adds the offsets to its window (0x40108fa7). */
#define DISPLAY_TFT_SCALE    12
#define DISPLAY_TFT_OFFSET_X 24
#define DISPLAY_TFT_OFFSET_Y 27

static bool s_fb[16][16];

esp_err_t display_init(void)
{
    esp_err_t err = gc9306_init();

    if (err != ESP_OK)
    {
        return err;
    }

    /* Stock startup enables the backlight after hard init and DISPON.
     * gc9306_display_on() is retained for explicit later resume only. */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 40000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&ledc_timer);
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
        .duty       = 255,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ledc_chan);
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
    esp_err_t err = gc9306_fill_rect(0, 0, 239, 319, 0x000000);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "flush: background fill failed: %s", esp_err_to_name(err));
        return;
    }
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            if (s_fb[y][x])
            {
                err = gc9306_fill_rect(x * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_X,
                                       y * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_Y,
                                       x * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_X
                                           + DISPLAY_TFT_SCALE - 1,
                                       y * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_Y
                                           + DISPLAY_TFT_SCALE - 1,
                                       0xFFFFFF);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "flush: pixel (%d,%d) fill failed: %s",
                             x, y, esp_err_to_name(err));
                }
            }
        }
    }
}

void display_show_rgba(const uint8_t rgba[16 * 16 * 4])
{
    esp_err_t err;

    if (rgba == NULL)
    {
        return;
    }

    err = gc9306_fill_rect(0, 0, 239, 319, 0x000000);
    if (err == ESP_OK)
    {
        err = gc9306_draw_rgba16(rgba);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RGBA frame failed: %s", esp_err_to_name(err));
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

void display_show_rgba(const uint8_t rgba[16 * 16 * 4])
{
    if (rgba == NULL)
    {
        return;
    }

    display_clear();
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            const uint8_t *src = &rgba[(y * 16 + x) * 4];
            uint16_t luma = (uint16_t)src[0] * 77
                            + (uint16_t)src[1] * 150
                            + (uint16_t)src[2] * 29;
            uint16_t alpha_luma = (uint16_t)((luma / 256) * src[3] / 255);

            display_set_pixel(x, y, alpha_luma >= 96);
        }
    }
    display_flush();
}

#endif /* CONFIG_BOARD_REV_04 */
