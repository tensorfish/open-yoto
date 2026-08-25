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

#include <string.h>

/* Native 5x7 rows for 0-9 and A-Z. Low five bits are left-to-right pixels. */
static const uint8_t ACCESS_FONT[36][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
    { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E },
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F },
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F },
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F },
    { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
};

static int access_glyph_index(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

#ifdef CONFIG_BOARD_REV_04

#include <stdlib.h>
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

/*
 * True while the panel outside the 192x192 icon window is known black. An icon
 * frame covers that window completely, so once the margin is clean successive
 * frames skip the 230 KB full-panel fill — paying it per frame flashes the
 * panel black between animation frames. Content is undefined after reset, so
 * the first frame always clears.
 */
static bool s_panel_margin_clean;

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
    /* Bitmap pixels reach panel row 218, three rows below the icon window, so
     * an icon frame cannot erase them. */
    s_panel_margin_clean = false;
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
                int panel_x = (15 - x) * DISPLAY_TFT_SCALE
                            + DISPLAY_TFT_OFFSET_X;
                err = gc9306_fill_rect(panel_x,
                                       y * DISPLAY_TFT_SCALE + DISPLAY_TFT_OFFSET_Y,
                                       panel_x + DISPLAY_TFT_SCALE - 1,
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

    if (!s_panel_margin_clean)
    {
        err = gc9306_fill_rect(0, 0, 239, 319, 0x000000);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "RGBA frame failed: %s", esp_err_to_name(err));
            return;
        }
        s_panel_margin_clean = true;
    }

    /* gc9306_draw_rgba16() writes every pixel of the icon window, including the
     * volume bar's rows, so it needs no clear of its own. */
    err = gc9306_draw_rgba16(rgba);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RGBA frame failed: %s", esp_err_to_name(err));
    }
}

esp_err_t display_show_rgb56516(const uint16_t pixels[16 * 16])
{
    uint8_t rgba[16 * 16 * 4];
    esp_err_t err;

    if (pixels == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 16 * 16; i++)
    {
        uint16_t color = pixels[i];
        uint8_t red5 = (uint8_t)((color >> 11) & 0x1F);
        uint8_t green6 = (uint8_t)((color >> 5) & 0x3F);
        uint8_t blue5 = (uint8_t)(color & 0x1F);

        rgba[i * 4] = (uint8_t)((red5 << 3) | (red5 >> 2));
        rgba[i * 4 + 1] = (uint8_t)((green6 << 2) | (green6 >> 4));
        rgba[i * 4 + 2] = (uint8_t)((blue5 << 3) | (blue5 >> 2));
        rgba[i * 4 + 3] = 0xFF;
    }

    err = gc9306_fill_rect(0, 0, 239, 319, 0x000000);
    if (err == ESP_OK)
    {
        s_panel_margin_clean = true;
        err = gc9306_draw_rgba16(rgba);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RGB565 frame failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t display_color64_begin(void)
{
    return gc9306_color64_begin();
}

esp_err_t display_color64_write_row(uint8_t y,
                                    const uint16_t pixels[64])
{
    if (y >= 64)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return gc9306_color64_write_row(pixels);
}

esp_err_t display_color64_end(void)
{
    return gc9306_color64_end();
}

void display_show_mask_full(const uint8_t mask[240 * 320 / 8],
                            uint32_t foreground, uint32_t background)
{
    if (mask == NULL)
    {
        return;
    }

    esp_err_t err = gc9306_draw_mask_full(mask, foreground, background);
    /* A caller-supplied raster can light any pixel on the panel. */
    s_panel_margin_clean = false;
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "full raster failed: %s", esp_err_to_name(err));
    }
}


#define ACCESS_MASK_BYTES (240 * 320 / 8)
#define ACCESS_GLYPH_SCALE 9
#define ACCESS_ROW_WIDTH   (3 * 5 * ACCESS_GLYPH_SCALE + 2 * ACCESS_GLYPH_SCALE)


static void access_mask_pixel(uint8_t *mask, int x, int y)
{
    if (x >= 0 && x < 240 && y >= 0 && y < 320)
    {
        /* MADCTL 0x48 mirrors RAMWR horizontally on the physical panel. */
        int panel_x = 239 - x;
        size_t pixel = (size_t)y * 240 + (size_t)panel_x;
        mask[pixel / 8] |= (uint8_t)(1U << (7 - (pixel % 8)));
    }
}

static void access_draw_native_glyph(uint8_t *mask, int x, int y, char ch)
{
    int index = access_glyph_index(ch);

    if (index < 0)
    {
        return;
    }
    for (int row = 0; row < 7; row++)
    {
        uint8_t bits = ACCESS_FONT[index][row];
        for (int col = 0; col < 5; col++)
        {
            if ((bits & (uint8_t)(1U << (4 - col))) == 0)
            {
                continue;
            }
            for (int sy = 0; sy < ACCESS_GLYPH_SCALE; sy++)
            {
                for (int sx = 0; sx < ACCESS_GLYPH_SCALE; sx++)
                {
                    access_mask_pixel(mask,
                                      x + col * ACCESS_GLYPH_SCALE + sx,
                                      y + row * ACCESS_GLYPH_SCALE + sy);
                }
            }
        }
    }
}

void display_show_access_code(
    const char code[DISPLAY_ACCESS_CODE_LEN + 1])
{
    int start_x = (240 - ACCESS_ROW_WIDTH) / 2;
    uint8_t *mask = calloc(1, ACCESS_MASK_BYTES);

    if (mask == NULL)
    {
        ESP_LOGE(TAG, "access-code mask allocation failed");
        return;
    }
    if (code != NULL)
    {
        for (int i = 0; i < DISPLAY_ACCESS_CODE_LEN; i++)
        {
            int x = start_x + (i % 3)
                  * (5 * ACCESS_GLYPH_SCALE + ACCESS_GLYPH_SCALE);
            int y = i < 3 ? 40 : 145;
            access_draw_native_glyph(mask, x, y, code[i]);
        }
    }
    esp_err_t err = gc9306_draw_mask_full(mask, 0xF5A623, 0x000000);
    free(mask);
    if (err == ESP_OK)
    {
        /* The mask blacks the whole panel and keeps its glyphs inside the icon
         * window (rows 40..208). */
        s_panel_margin_clean = true;
    }
    else
    {
        ESP_LOGW(TAG, "access-code render failed: %s", esp_err_to_name(err));
    }
}

/*
 * The volume bar is 144 px wide and 12 px tall, centred horizontally in the
 * 192 px icon window, with its bottom edge on DISPLAY_TFT_OFFSET_Y + 183.
 * It spans panel x 48..191 and y 199..210, entirely inside the icon window
 * (panel y 24..215), so gc9306_draw_rgba16() erases it on the next full
 * frame.
 *
 * The bar used to be drawn one 1-px column at a time, but gc9306_fill_rect()
 * toggles the TFT CS/DC pins through the IOX over I2C around every rect, so
 * up to 144 rects made the volume feedback lag badly. Drawing at most four
 * full-height rects (three colour bands plus a black remainder) removes the
 * I2C chatter.
 */
#define VOLUME_BAR_WIDTH  144
#define VOLUME_BAR_HEIGHT 12
#define VOLUME_BAR_X0 \
    (DISPLAY_TFT_OFFSET_X + (16 * DISPLAY_TFT_SCALE - VOLUME_BAR_WIDTH) / 2)
#define VOLUME_BAR_X1 (VOLUME_BAR_X0 + VOLUME_BAR_WIDTH - 1)
#define VOLUME_BAR_Y1 (DISPLAY_TFT_OFFSET_Y + 183)
#define VOLUME_BAR_Y0 (VOLUME_BAR_Y1 - VOLUME_BAR_HEIGHT + 1)

/*
 * Fill one bar-local span [lo, hi) with a single gc9306_fill_rect(). Logical
 * bar-local x maps to panel x = VOLUME_BAR_X1 - x, so the bar grows from the
 * panel's right edge towards its left, matching the original overlay.
 */
static void volume_fill_rect(int lo, int hi, uint32_t color)
{
    uint16_t x0;
    uint16_t x1;
    esp_err_t err;

    if (lo >= hi)
    {
        return;
    }
    x0 = (uint16_t)(VOLUME_BAR_X1 - (hi - 1));
    x1 = (uint16_t)(VOLUME_BAR_X1 - lo);
    err = gc9306_fill_rect(x0, VOLUME_BAR_Y0, x1, VOLUME_BAR_Y1, color);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "volume bar [%d,%d) fill failed: %s",
                 lo, hi, esp_err_to_name(err));
    }
}

void display_draw_volume_overlay(int volume)
{
    int filled;
    static const uint32_t volume_band_color[3] = {
        0x168BFF, 0x20D060, 0xFF3B30
    };

    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 100)
    {
        volume = 100;
    }
    filled = volume * VOLUME_BAR_WIDTH / 100;

    /*
     * Bands are keyed to bar-local x and split the bar into equal thirds
     * (48 px each at this width). Intersect each with [0, filled) and emit
     * only non-empty spans.
     */
    for (int i = 0; i < 3; i++)
    {
        int lo = i * VOLUME_BAR_WIDTH / 3;
        int hi = (i + 1) * VOLUME_BAR_WIDTH / 3;

        if (lo >= filled)
        {
            continue;
        }
        if (hi > filled)
        {
            hi = filled;
        }
        volume_fill_rect(lo, hi, volume_band_color[i]);
    }

    /* Black remainder [filled, VOLUME_BAR_WIDTH) so lowering the volume
     * shrinks the bar. */
    volume_fill_rect(filled, VOLUME_BAR_WIDTH, 0x000000);
}

#else /* CONFIG_BOARD_REV_04 */

#include "ht16d35x.h"



static uint32_t s_color64_luma[16];
static uint8_t s_color64_next_row;
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

#define VOLUME_BAR_LOGICAL_X      2
#define VOLUME_BAR_LOGICAL_WIDTH  12
#define VOLUME_BAR_LOGICAL_ROW0   14
#define VOLUME_BAR_LOGICAL_ROW1   15

void display_draw_volume_overlay(int volume)
{
    int filled;

    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 100)
    {
        volume = 100;
    }
    filled = volume * VOLUME_BAR_LOGICAL_WIDTH / 100;

    /*
     * The bar is two rows thick (y 14 and 15) and 12 cells wide (x 2..13),
     * already centred in the 16x16 grid. Set the filled cells in both rows,
     * clear the remainder so a lower volume shrinks the bar, and flush once.
     */
    for (int x = VOLUME_BAR_LOGICAL_X;
         x < VOLUME_BAR_LOGICAL_X + VOLUME_BAR_LOGICAL_WIDTH; x++)
    {
        bool on = (x - VOLUME_BAR_LOGICAL_X) < filled;

        display_set_pixel(x, VOLUME_BAR_LOGICAL_ROW0, on);
        display_set_pixel(x, VOLUME_BAR_LOGICAL_ROW1, on);
    }
    display_flush();
}

esp_err_t display_show_rgb56516(const uint16_t pixels[16 * 16])
{
    if (pixels == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    display_clear();
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            uint16_t color = pixels[y * 16 + x];
            uint16_t red = (uint16_t)(((color >> 11) & 0x1F) * 255 / 31);
            uint16_t green = (uint16_t)(((color >> 5) & 0x3F) * 255 / 63);
            uint16_t blue = (uint16_t)((color & 0x1F) * 255 / 31);
            uint16_t luma = (red * 77 + green * 150 + blue * 29) / 256;

            display_set_pixel(x, y, luma >= 96);
        }
    }
    display_flush();
    return ESP_OK;
}

esp_err_t display_color64_begin(void)
{
    memset(s_color64_luma, 0, sizeof(s_color64_luma));
    s_color64_next_row = 0;
    display_clear();
    return ESP_OK;
}

esp_err_t display_color64_write_row(uint8_t y,
                                    const uint16_t pixels[64])
{
    if (pixels == NULL || y != s_color64_next_row || y >= 64)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int x = 0; x < 64; x++)
    {
        uint16_t color = pixels[x];
        uint16_t red = (uint16_t)(((color >> 11) & 0x1F) * 255 / 31);
        uint16_t green = (uint16_t)(((color >> 5) & 0x3F) * 255 / 63);
        uint16_t blue = (uint16_t)((color & 0x1F) * 255 / 31);

        s_color64_luma[x / 4] += (red * 77 + green * 150 + blue * 29) / 256;
    }
    if ((y & 3) == 3)
    {
        int target_y = y / 4;

        for (int target_x = 0; target_x < 16; target_x++)
        {
            display_set_pixel(target_x, target_y,
                              s_color64_luma[target_x] >= 96 * 16);
            s_color64_luma[target_x] = 0;
        }
    }
    s_color64_next_row++;
    return ESP_OK;
}

esp_err_t display_color64_end(void)
{
    if (s_color64_next_row != 64)
    {
        return ESP_ERR_INVALID_STATE;
    }
    display_flush();
    return ESP_OK;
}

void display_show_mask_full(const uint8_t mask[240 * 320 / 8],
                            uint32_t foreground, uint32_t background)
{
    (void)foreground;
    (void)background;

    if (mask == NULL)
    {
        return;
    }

    display_clear();
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            size_t pixel = (size_t)(y * 20 + 10) * 240 + x * 15 + 7;
            bool on = (mask[pixel / 8] & (uint8_t)(1U << (7 - (pixel % 8)))) != 0;

            display_set_pixel(x, y, on);
        }
    }
    display_flush();
}


void display_show_access_code(
    const char code[DISPLAY_ACCESS_CODE_LEN + 1])
{
    ht16d35x_clear();
    if (code != NULL)
    {
        for (int i = 0; i < DISPLAY_ACCESS_CODE_LEN; i++)
        {
            int index = access_glyph_index(code[i]);
            int x0 = (i % 3) * 5;
            int y0 = i < 3 ? 0 : 9;

            if (index < 0)
            {
                continue;
            }
            for (int row = 0; row < 7; row++)
            {
                uint8_t bits = ACCESS_FONT[index][row];
                for (int col = 0; col < 5; col++)
                {
                    if ((bits & (uint8_t)(1U << (4 - col))) != 0)
                    {
                        ht16d35x_set_pixel(x0 + col, y0 + row, true);
                    }
                }
            }
        }
    }
    ht16d35x_flush();
}

#endif /* CONFIG_BOARD_REV_04 */
