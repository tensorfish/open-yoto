/*
 * panel_clear_test.c — host-side regression test for the rev #04 panel-clear
 * elision in display_show_rgba().
 *
 * This is a plain host program (no ESP-IDF). It compiles the real
 * ../../components/display/display.c into this translation unit so the
 * s_panel_margin_clean state machine under test is the shipped one. The GC9306
 * driver is stubbed: gc9306_fill_rect() records every rect (and counts the
 * full-panel 0,0..239,319 / 0x000000 clears), gc9306_draw_rgba16() and
 * gc9306_draw_mask_full() count their calls.
 *
 * The anti-flicker guarantee this test locks in: after the first icon frame
 * clears the panel margin, consecutive icon frames cost only the 192x192
 * window write. Clearing the full panel on every frame flashes it black
 * between animation frames.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CONFIG_BOARD_REV_04

#include "../../components/display/display.c"

static int s_fill_count;
static int s_clear_count;
static int s_rgba_count;
static int s_mask_full_count;

static int s_failures;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            fprintf(stderr, "FAIL: ");                      \
            fprintf(stderr, __VA_ARGS__);                   \
            fprintf(stderr, "\n");                          \
            s_failures++;                                   \
        }                                                   \
    } while (0)

/* ---- GC9306 / LEDC / err stubs (record calls, otherwise succeed) ---- */

/*
 * Recognise the full-panel clear: a gc9306_fill_rect() covering exactly
 * 0,0..239,319 with colour 0x000000.
 */
static bool is_full_panel_clear(uint16_t x0, uint16_t y0, uint16_t x1,
                                uint16_t y1, uint32_t color)
{
    return x0 == 0 && y0 == 0 && x1 == 239 && y1 == 319
        && color == 0x000000;
}

esp_err_t gc9306_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint32_t color)
{
    s_fill_count++;
    if (is_full_panel_clear(x0, y0, x1, y1, color))
    {
        s_clear_count++;
    }
    return ESP_OK;
}

esp_err_t gc9306_init(void)
{
    return ESP_OK;
}

esp_err_t gc9306_display_on(void)
{
    return ESP_OK;
}

esp_err_t gc9306_draw_rgba16(const uint8_t rgba[16 * 16 * 4])
{
    (void)rgba;
    s_rgba_count++;
    return ESP_OK;
}

esp_err_t gc9306_color64_begin(void)
{
    return ESP_OK;
}

esp_err_t gc9306_color64_write_row(const uint16_t pixels[64])
{
    (void)pixels;
    return ESP_OK;
}

esp_err_t gc9306_color64_end(void)
{
    return ESP_OK;
}

esp_err_t gc9306_draw_mask192(const uint8_t mask[192 * 192 / 8],
                              uint32_t foreground, uint32_t background)
{
    (void)mask;
    (void)foreground;
    (void)background;
    return ESP_OK;
}

esp_err_t gc9306_draw_mask_full(const uint8_t mask[240 * 320 / 8],
                                uint32_t foreground, uint32_t background)
{
    (void)mask;
    (void)foreground;
    (void)background;
    s_mask_full_count++;
    return ESP_OK;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    (void)config;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

int main(void)
{
    static uint8_t frame[16 * 16 * 4];
    int clear_before;
    int rgba_before;

    /* Assertion 1: the first frame after startup clears once and draws once. */
    {
        display_show_rgba(frame);
        CHECK(s_clear_count == 1,
              "first show_rgba issued %d full-panel clear(s), want exactly 1",
              s_clear_count);
        CHECK(s_rgba_count == 1,
              "first show_rgba issued %d draw_rgba16 call(s), want exactly 1",
              s_rgba_count);
    }

    /* Assertion 2: the next four frames clear nothing and draw once each. */
    for (int i = 0; i < 4; i++)
    {
        clear_before = s_clear_count;
        rgba_before = s_rgba_count;
        display_show_rgba(frame);
        CHECK(s_clear_count == clear_before,
              "show_rgba #%d issued a full-panel clear; clearing every frame "
              "flashes the panel black between animation frames", i + 2);
        CHECK(s_rgba_count == rgba_before + 1,
              "show_rgba #%d issued %d draw_rgba16 call(s), want exactly 1",
              i + 2, s_rgba_count - rgba_before);
    }

    /* Assertion 3: after display_flush() the next frame clears once, the one
     * after that does not. */
    {
        display_flush();
        clear_before = s_clear_count;
        rgba_before = s_rgba_count;
        display_show_rgba(frame);
        CHECK(s_clear_count - clear_before == 1,
              "after flush, show_rgba issued %d full-panel clear(s), want 1",
              s_clear_count - clear_before);
        CHECK(s_rgba_count - rgba_before == 1,
              "after flush, show_rgba issued %d draw_rgba16 call(s), want 1",
              s_rgba_count - rgba_before);

        clear_before = s_clear_count;
        display_show_rgba(frame);
        CHECK(s_clear_count == clear_before,
              "second show_rgba after flush issued a full-panel clear");
    }

    /* Assertion 4: after show_mask_full (all-zero mask) the next frame clears
     * once. */
    {
        static uint8_t mask[240 * 320 / 8];  /* all zero */
        int mask_before = s_mask_full_count;

        display_show_mask_full(mask, 0xFFFFFF, 0x000000);
        CHECK(s_mask_full_count == mask_before + 1,
              "show_mask_full issued %d draw_mask_full call(s), want 1",
              s_mask_full_count - mask_before);

        clear_before = s_clear_count;
        display_show_rgba(frame);
        CHECK(s_clear_count - clear_before == 1,
              "after show_mask_full, show_rgba issued %d clear(s), want 1",
              s_clear_count - clear_before);
    }

    /* Assertion 5: after show_access_code("ABC123") the next frame does not
     * clear (the mask already blacked the panel). */
    {
        int mask_before = s_mask_full_count;

        display_show_access_code("ABC123");
        CHECK(s_mask_full_count == mask_before + 1,
              "show_access_code issued %d draw_mask_full call(s), want 1",
              s_mask_full_count - mask_before);

        clear_before = s_clear_count;
        display_show_rgba(frame);
        CHECK(s_clear_count == clear_before,
              "after show_access_code, show_rgba issued a full-panel clear; "
              "the mask already blacked the panel");
    }

    /* Assertion 6: a volume overlay between two frames neither clears on the
     * following frame nor counts its own rects as full-panel clears. */
    {
        int fill_before;
        int overlay_clears;

        display_show_rgba(frame);   /* first frame: margin stays clean */

        fill_before = s_fill_count;
        clear_before = s_clear_count;
        display_draw_volume_overlay(50);
        overlay_clears = s_clear_count - clear_before;
        CHECK(s_fill_count - fill_before > 0,
              "volume overlay(50) issued no fill_rect call");
        CHECK(overlay_clears == 0,
              "volume overlay(50) rects counted as %d full-panel clear(s)",
              overlay_clears);

        clear_before = s_clear_count;
        display_show_rgba(frame);
        CHECK(s_clear_count == clear_before,
              "volume overlay(50) caused the following show_rgba to clear");
    }

    /* Assertion 7: a NULL frame draws nothing and leaves the clear/no-clear
     * state untouched for the following real frame, in both states. */
    {
        /* Dirty state: NULL must not draw, and must leave it dirty. */
        display_flush();
        rgba_before = s_rgba_count;
        clear_before = s_clear_count;
        display_show_rgba(NULL);
        CHECK(s_rgba_count == rgba_before,
              "NULL frame issued %d draw_rgba16 call(s), want 0",
              s_rgba_count - rgba_before);
        CHECK(s_clear_count == clear_before,
              "NULL frame issued %d full-panel clear(s), want 0",
              s_clear_count - clear_before);
        display_show_rgba(frame);
        CHECK(s_clear_count - clear_before == 1,
              "NULL frame changed dirty state: following frame cleared "
              "%d time(s), want 1", s_clear_count - clear_before);

        /* Clean state: NULL must not draw, and must leave it clean. */
        rgba_before = s_rgba_count;
        clear_before = s_clear_count;
        display_show_rgba(NULL);
        CHECK(s_rgba_count == rgba_before,
              "NULL frame (clean) issued %d draw_rgba16 call(s), want 0",
              s_rgba_count - rgba_before);
        CHECK(s_clear_count == clear_before,
              "NULL frame (clean) issued %d full-panel clear(s), want 0",
              s_clear_count - clear_before);
        display_show_rgba(frame);
        CHECK(s_clear_count == clear_before,
              "NULL frame changed clean state: following frame issued a clear");
    }

    if (s_failures != 0)
    {
        fprintf(stderr, "panel clear test: %d assertion(s) failed\n",
                s_failures);
        return 1;
    }

    printf("panel clear test: all assertions passed\n");
    return 0;
}
