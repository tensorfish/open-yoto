/*
 * volume_overlay_test.c — host-side geometry and draw-call-budget test for
 * the rev04 volume overlay.
 *
 * This is a plain host program (no ESP-IDF). It compiles the real
 * ../../components/display/display.c into this translation unit so the
 * geometry constants and the fill_rect call budget under test are the shipped
 * ones. The GC9306 driver is stubbed: gc9306_fill_rect() records every call,
 * and the test reconstructs the 168-column bar to assert:
 *
 *   1. at most 4 fill_rect calls per display_draw_volume_overlay() call;
 *   2. every rect is 16 px tall and inside panel x 36..203, y 195..210;
 *   3. the volume-100 bar is centred: its rect union is exactly x 36..203;
 *   4. volume 0 is all black; volume 100 has no black and the three band
 *      colours 0x168BFF / 0x20D060 / 0xFF3B30;
 *   5. the coloured column count is monotonic and equals volume * 168 / 100;
 *   6. volume 25 grows from panel x 203 leftwards, all 0x168BFF;
 *   7. drawing 100 then 25 covers the previously lit columns in black.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CONFIG_BOARD_REV_04

#include "../../components/display/display.c"

/* Panel geometry under test (derived from DISPLAY_TFT_* in display.c). */
#define BAR_X0     36
#define BAR_X1     203
#define BAR_Y0     195
#define BAR_Y1     210
#define BAR_WIDTH  168

#define MAX_RECTS 4096

typedef struct
{
    int x0;
    int y0;
    int x1;
    int y1;
    uint32_t color;
} fill_rect_t;

static fill_rect_t s_all[MAX_RECTS];
static int s_all_count;
static int s_draw_start;

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

/* ---- GC9306 / LEDC / err stubs (record rects, otherwise succeed) ---- */

esp_err_t gc9306_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint32_t color)
{
    if (s_all_count < MAX_RECTS)
    {
        fill_rect_t *r = &s_all[s_all_count];

        r->x0 = (int)x0;
        r->y0 = (int)y0;
        r->x1 = (int)x1;
        r->y1 = (int)y1;
        r->color = color;
        s_all_count++;
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
    return ESP_OK;
}

esp_err_t gc9306_draw_rgb56516_full(const uint16_t pixels[16 * 16])
{
    (void)pixels;
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

/* ---- test helpers ---- */

static int last_draw_rects(void)
{
    return s_all_count - s_draw_start;
}

static void draw(int volume)
{
    s_draw_start = s_all_count;
    display_draw_volume_overlay(volume);
}

/*
 * Rebuild the 144-column bar colour array from the rects recorded for the
 * most recent overlay draw. A column is "painted" by the last rect that
 * covers its full 12-row height; later rects win.
 */
static void reconstruct(uint32_t cols[BAR_WIDTH], bool covered[BAR_WIDTH])
{
    for (int c = 0; c < BAR_WIDTH; c++)
    {
        int panel_x = BAR_X0 + c;

        cols[c] = 0x000000;
        covered[c] = false;
        for (int i = s_all_count - 1; i >= s_draw_start; i--)
        {
            fill_rect_t *r = &s_all[i];

            if (r->x0 <= panel_x && panel_x <= r->x1
                && r->y0 <= BAR_Y0 && BAR_Y1 <= r->y1)
            {
                cols[c] = r->color;
                covered[c] = true;
                break;
            }
        }
    }
}

int main(void)
{
    uint32_t cols[BAR_WIDTH];
    bool covered[BAR_WIDTH];

    /* Assertion 1: at most 4 fill_rect calls per overlay draw. */
    {
        static const int volumes[] = { 0, 1, 25, 33, 50, 66, 75, 99, 100 };

        for (size_t i = 0; i < sizeof(volumes) / sizeof(volumes[0]); i++)
        {
            draw(volumes[i]);
            CHECK(last_draw_rects() <= 4,
                  "volume %d issued %d fill_rect calls (limit 4)",
                  volumes[i], last_draw_rects());
        }
    }

    /* Assertion 2: every recorded rect is inside the bar and 12 px tall. */
    for (int i = 0; i < s_all_count; i++)
    {
        fill_rect_t *r = &s_all[i];

        CHECK(r->x0 >= BAR_X0 && r->x1 <= BAR_X1,
              "rect y %d..%d x %d..%d escapes panel x %d..%d",
              r->y0, r->y1, r->x0, r->x1, BAR_X0, BAR_X1);
        CHECK(r->y0 == BAR_Y0 && r->y1 == BAR_Y1,
              "rect y %d..%d is not exactly %d..%d",
              r->y0, r->y1, BAR_Y0, BAR_Y1);
    }

    /* Assertion 3: the volume-100 bar is centred (union exactly x 48..191). */
    {
        bool covered_x[BAR_WIDTH];
        int min_x = BAR_X1 + 1;
        int max_x = BAR_X0 - 1;

        for (int c = 0; c < BAR_WIDTH; c++)
        {
            covered_x[c] = false;
        }

        draw(100);
        for (int i = s_draw_start; i < s_all_count; i++)
        {
            fill_rect_t *r = &s_all[i];

            for (int x = r->x0; x <= r->x1; x++)
            {
                if (x >= BAR_X0 && x <= BAR_X1)
                {
                    covered_x[x - BAR_X0] = true;
                }
                if (x < min_x)
                {
                    min_x = x;
                }
                if (x > max_x)
                {
                    max_x = x;
                }
            }
        }

        int missing = 0;
        for (int c = 0; c < BAR_WIDTH; c++)
        {
            if (!covered_x[c])
            {
                missing++;
            }
        }
        CHECK(missing == 0, "volume 100 union misses %d column(s) in x %d..%d",
              missing, BAR_X0, BAR_X1);
        CHECK(min_x == BAR_X0 && max_x == BAR_X1,
              "volume 100 union is x %d..%d, expected exactly %d..%d",
              min_x, max_x, BAR_X0, BAR_X1);
    }

    /* Assertion 4: volume 0 is all black; volume 100 is full colour. */
    {
        draw(0);
        reconstruct(cols, covered);
        int bad = 0;
        for (int c = 0; c < BAR_WIDTH; c++)
        {
            if (!covered[c] || cols[c] != 0x000000)
            {
                bad++;
            }
        }
        CHECK(bad == 0, "volume 0 has %d non-black/uncovered column(s)", bad);

        draw(100);
        reconstruct(cols, covered);
        int black = 0;
        int blue = 0;
        int green = 0;
        int red = 0;
        for (int c = 0; c < BAR_WIDTH; c++)
        {
            if (!covered[c])
            {
                black++;
            }
            else if (cols[c] == 0x000000)
            {
                black++;
            }
            else if (cols[c] == 0x168BFF)
            {
                blue++;
            }
            else if (cols[c] == 0x20D060)
            {
                green++;
            }
            else if (cols[c] == 0xFF3B30)
            {
                red++;
            }
            else
            {
                black++; /* unexpected colour -> flagged below */
            }
        }
        CHECK(black == 0, "volume 100 has %d black/uncovered column(s)", black);
        CHECK(blue == 56 && green == 56 && red == 56,
              "volume 100 band split blue=%d green=%d red=%d (want 56 each)",
              blue, green, red);
    }

    /* Assertion 5: coloured column count is monotonic and volume-proportional. */
    {
        int prev = -1;

        for (int v = 0; v <= 100; v += 10)
        {
            int count = 0;
            int want = v * BAR_WIDTH / 100;

            draw(v);
            reconstruct(cols, covered);
            for (int c = 0; c < BAR_WIDTH; c++)
            {
                if (covered[c] && cols[c] != 0x000000)
                {
                    count++;
                }
            }
            CHECK(count == want, "volume %d coloured count %d != %d",
                  v, count, want);
            CHECK(count >= prev, "volume %d coloured count %d < previous %d",
                  v, count, prev);
            prev = count;
        }
    }

    /* Assertion 6: volume 25 grows from the right edge, all blue. */
    {
        int rightmost = -1;
        int bad = 0;

        draw(25);
        reconstruct(cols, covered);
        for (int c = 0; c < BAR_WIDTH; c++)
        {
            if (covered[c] && cols[c] != 0x000000)
            {
                if (cols[c] != 0x168BFF)
                {
                    bad++;
                }
                rightmost = c;
            }
        }
        CHECK(rightmost == BAR_WIDTH - 1,
              "volume 25 coloured span ends at column %d (panel x %d), want %d",
              rightmost, rightmost < 0 ? -1 : BAR_X0 + rightmost, BAR_X1);
        CHECK(bad == 0, "volume 25 has %d non-blue coloured column(s)", bad);
    }

    /* Assertion 7: shrinking from 100 to 25 blacks out the earlier columns. */
    {
        uint32_t cols100[BAR_WIDTH];
        bool cov100[BAR_WIDTH];
        int filled = 25 * BAR_WIDTH / 100;
        int uncovered = 0;
        int not_black = 0;
        int lit_then_dark = 0;
        int black_covers = 0;

        draw(100);
        reconstruct(cols100, cov100);

        draw(25);
        reconstruct(cols, covered);

        for (int c = 0; c < BAR_WIDTH; c++)
        {
            if (cov100[c] && cols100[c] != 0x000000
                && !(covered[c] && cols[c] != 0x000000))
            {
                lit_then_dark++;
                if (!covered[c])
                {
                    uncovered++;
                }
                else if (cols[c] != 0x000000)
                {
                    not_black++;
                }
            }
        }
        CHECK(lit_then_dark > 0,
              "volume 100->25 found no column lit at 100 and dark at 25");
        CHECK(uncovered == 0 && not_black == 0,
              "volume 100->25 leaves %d uncovered, %d non-black column(s)",
              uncovered, not_black);

        for (int i = s_draw_start; i < s_all_count; i++)
        {
            fill_rect_t *r = &s_all[i];

            if (r->color == 0x000000 && r->x0 <= BAR_X0
                && r->x1 >= BAR_X1 - filled)
            {
                black_covers = 1;
            }
        }
        CHECK(black_covers == 1,
              "volume 25 black rect does not cover x %d..%d",
              BAR_X0, BAR_X1 - filled);
    }

    if (s_failures != 0)
    {
        fprintf(stderr, "volume overlay test: %d assertion(s) failed\n",
                s_failures);
        return 1;
    }

    printf("volume overlay test: all assertions passed\n");
    return 0;
}
