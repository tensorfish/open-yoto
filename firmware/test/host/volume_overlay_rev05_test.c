/*
 * volume_overlay_rev05_test.c — host-side geometry and draw-call test for
 * the rev05 (HT16D35x) volume overlay.
 *
 * This is a plain host program (no ESP-IDF). It compiles the real
 * ../../components/display/display.c into this translation unit WITHOUT
 * defining CONFIG_BOARD_REV_04, so the HT16D35x half builds. The HT16D35x
 * driver is stubbed: ht16d35x_set_pixel() records every call and writes a
 * 16x16 shadow framebuffer, and ht16d35x_flush() counts invocations. The
 * test then asserts:
 *
 *   1. exactly one flush per display_draw_volume_overlay() call;
 *   2. only logical x 2..13 on rows 14 and 15 are ever written;
 *   3. rows 14 and 15 are identical for every volume;
 *   4. the lit-cell count equals volume * 12 / 100 and never decreases;
 *   5. lit cells form a contiguous run from x 2, and every bar cell is
 *      explicitly written (on or off) on every call;
 *   6. out-of-range volume clamps (negative behaves like 0, >100 like 100);
 *   7. the overlay composes over an already-lit frame, leaving rows 0..13
 *      untouched.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../../components/display/display.c"

/* Logical bar geometry under test (derived from the VOLUME_BAR_LOGICAL_*
 * constants in display.c's rev05 half). */
#define BAR_X0    2
#define BAR_X1    13
#define BAR_ROW0  14
#define BAR_ROW1  15
#define BAR_WIDTH 12
/* Every overlay draw writes 12 columns x 2 rows = 24 set_pixel calls; the
 * whole test performs ~230 draws, so this buffer holds every call with room
 * to spare and never truncates the record. */
#define MAX_SET 16384

static int s_set_x[MAX_SET];
static int s_set_y[MAX_SET];
static int s_set_count;
static int s_set_start;

static bool s_shadow[16][16];

static int s_flush_count;
static int s_flush_start;

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

/* ---- HT16D35x stubs (record calls, write shadow framebuffer) ---- */

esp_err_t ht16d35x_init(void)
{
    return ESP_OK;
}

void ht16d35x_clear(void)
{
    memset(s_shadow, 0, sizeof(s_shadow));
}

void ht16d35x_set_pixel(int x, int y, bool on)
{
    if (s_set_count < MAX_SET)
    {
        s_set_x[s_set_count] = x;
        s_set_y[s_set_count] = y;
        s_set_count++;
    }
    if (x >= 0 && x < 16 && y >= 0 && y < 16)
    {
        s_shadow[y][x] = on;
    }
}

void ht16d35x_flush(void)
{
    s_flush_count++;
}

/* ---- test helpers ---- */

static int last_flush_calls(void)
{
    return s_flush_count - s_flush_start;
}

static void draw(int volume)
{
    s_set_start = s_set_count;
    s_flush_start = s_flush_count;
    display_draw_volume_overlay(volume);
}

static void seed_all(bool on)
{
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            s_shadow[y][x] = on;
        }
    }
}

static void snapshot(bool snap[16][16])
{
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            snap[y][x] = s_shadow[y][x];
        }
    }
}

/* Mark every cell written by the most recent overlay draw. */
static void collect_written(bool written[16][16])
{
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            written[y][x] = false;
        }
    }
    for (int i = s_set_start; i < s_set_count; i++)
    {
        if (s_set_y[i] >= 0 && s_set_y[i] < 16
            && s_set_x[i] >= 0 && s_set_x[i] < 16)
        {
            written[s_set_y[i]][s_set_x[i]] = true;
        }
    }
}

int main(void)
{
    bool written[16][16];

    /* Assertion 1: exactly one flush per overlay invocation. */
    {
        static const int volumes[] = { 0, 1, 25, 33, 50, 66, 75, 99, 100 };

        for (size_t i = 0; i < sizeof(volumes) / sizeof(volumes[0]); i++)
        {
            draw(volumes[i]);
            CHECK(last_flush_calls() == 1,
                  "volume %d flushed %d times (want 1)",
                  volumes[i], last_flush_calls());
        }
    }

    /* Assertion 3: rows 14 and 15 are identical for every volume. */
    for (int v = 0; v <= 100; v++)
    {
        int diffs = 0;

        seed_all(false);
        draw(v);
        for (int x = 0; x < 16; x++)
        {
            if (s_shadow[BAR_ROW0][x] != s_shadow[BAR_ROW1][x])
            {
                diffs++;
            }
        }
        CHECK(diffs == 0, "volume %d rows %d/%d differ in %d cell(s)",
              v, BAR_ROW0, BAR_ROW1, diffs);
    }

    /* Assertion 4: lit-cell count is volume-proportional and monotonic. */
    {
        int prev = -1;

        for (int v = 0; v <= 100; v += 10)
        {
            int filled = v * BAR_WIDTH / 100;
            int lit = 0;

            seed_all(false);
            draw(v);
            for (int x = BAR_X0; x <= BAR_X1; x++)
            {
                if (s_shadow[BAR_ROW0][x])
                {
                    lit++;
                }
            }
            CHECK(lit == filled, "volume %d lit %d cell(s), want %d",
                  v, lit, filled);
            CHECK(lit >= prev, "volume %d lit %d < previous %d",
                  v, lit, prev);
            prev = lit;
        }
    }

    /* Assertion 5: contiguous run from x 2, every bar cell written. */
    for (int v = 0; v <= 100; v++)
    {
        int filled = v * BAR_WIDTH / 100;
        int bad_pattern = 0;
        int missing_write = 0;

        seed_all(false);
        draw(v);

        for (int x = BAR_X0; x <= BAR_X1; x++)
        {
            bool want_on = (x - BAR_X0) < filled;

            if (s_shadow[BAR_ROW0][x] != want_on
                || s_shadow[BAR_ROW1][x] != want_on)
            {
                bad_pattern++;
            }
        }

        collect_written(written);
        for (int x = BAR_X0; x <= BAR_X1; x++)
        {
            if (!written[BAR_ROW0][x] || !written[BAR_ROW1][x])
            {
                missing_write++;
            }
        }

        CHECK(bad_pattern == 0,
              "volume %d: %d bar cell(s) not a contiguous run from x %d",
              v, bad_pattern, BAR_X0);
        CHECK(missing_write == 0,
              "volume %d left %d bar cell(s) unwritten on rows %d/%d",
              v, missing_write, BAR_ROW0, BAR_ROW1);
    }

    /* Assertion 6: out-of-range input clamps to 0 and 100. */
    {
        bool neg[16][16];
        bool zero[16][16];
        bool over[16][16];
        bool full[16][16];
        int neg_diff = 0;
        int over_diff = 0;

        seed_all(false);
        draw(-50);
        snapshot(neg);

        seed_all(false);
        draw(0);
        snapshot(zero);

        seed_all(false);
        draw(500);
        snapshot(over);

        seed_all(false);
        draw(100);
        snapshot(full);

        for (int y = 0; y < 16; y++)
        {
            for (int x = 0; x < 16; x++)
            {
                if (neg[y][x] != zero[y][x])
                {
                    neg_diff++;
                }
                if (over[y][x] != full[y][x])
                {
                    over_diff++;
                }
            }
        }
        CHECK(neg_diff == 0, "volume -50 differs from 0 in %d cell(s)",
              neg_diff);
        CHECK(over_diff == 0, "volume 500 differs from 100 in %d cell(s)",
              over_diff);
    }

    /* Assertion 7: overlay composes over a pre-lit frame. */
    {
        int cleared_above = 0;

        seed_all(true);
        draw(25);

        for (int y = 0; y < BAR_ROW0; y++)
        {
            for (int x = 0; x < 16; x++)
            {
                if (!s_shadow[y][x])
                {
                    cleared_above++;
                }
            }
        }
        CHECK(cleared_above == 0,
              "volume 25 overlay cleared %d cell(s) above row %d",
              cleared_above, BAR_ROW0);
    }

    /* Assertion 2: every set_pixel call across the run stays in the bar. */
    for (int i = 0; i < s_set_count; i++)
    {
        CHECK(s_set_x[i] >= BAR_X0 && s_set_x[i] <= BAR_X1
              && (s_set_y[i] == BAR_ROW0 || s_set_y[i] == BAR_ROW1),
              "set_pixel(%d,%d) escapes bar x %d..%d rows %d/%d",
              s_set_x[i], s_set_y[i], BAR_X0, BAR_X1, BAR_ROW0, BAR_ROW1);
    }

    /* The rev-05 Bluetooth indicator is the physical top-right logical LED
     * and remains lit across every public path that rewrites the framebuffer. */
    {
        static uint8_t rgba[16 * 16 * 4];
        static uint16_t rgb565[16 * 16];
        static uint16_t color64_row[64];
        static uint8_t full_mask[240 * 320 / 8];

        display_set_pixel(15, 0, false);
        display_flush();
        s_set_start = s_set_count;
        s_flush_start = s_flush_count;
        display_set_bluetooth_indicator(true);
        CHECK(s_set_count - s_set_start == 1,
              "enabling Bluetooth wrote %d LEDs, want 1",
              s_set_count - s_set_start);
        CHECK(s_set_x[s_set_count - 1] == 15
              && s_set_y[s_set_count - 1] == 0,
              "Bluetooth indicator is (%d,%d), want top-right (15,0)",
              s_set_x[s_set_count - 1], s_set_y[s_set_count - 1]);
        CHECK(s_shadow[0][15],
              "Bluetooth indicator is dark immediately after enable");
        CHECK(last_flush_calls() == 1,
              "enabling Bluetooth flushed %d times, want 1",
              last_flush_calls());

        display_clear();
        display_flush();
        CHECK(s_shadow[0][15], "clear/flush erased Bluetooth indicator");

        display_show_rgba(rgba);
        CHECK(s_shadow[0][15], "RGBA render erased Bluetooth indicator");

        CHECK(display_show_rgb56516(rgb565) == ESP_OK,
              "RGB565 render failed");
        CHECK(s_shadow[0][15], "RGB565 render erased Bluetooth indicator");

        CHECK(display_color64_begin() == ESP_OK, "color64 begin failed");
        for (uint8_t y = 0; y < 64; y++)
        {
            CHECK(display_color64_write_row(y, color64_row) == ESP_OK,
                  "color64 row %u failed", (unsigned)y);
        }
        CHECK(display_color64_end() == ESP_OK, "color64 end failed");
        CHECK(s_shadow[0][15], "color64 render erased Bluetooth indicator");

        display_show_mask_full(full_mask, 0xFFFFFF, 0x000000);
        CHECK(s_shadow[0][15], "full-mask render erased Bluetooth indicator");

        display_show_access_code(NULL);
        CHECK(s_shadow[0][15],
              "access-code render erased Bluetooth indicator");

        display_draw_volume_overlay(50);
        CHECK(s_shadow[0][15],
              "volume overlay erased Bluetooth indicator");

        display_set_pixel(15, 0, false);
        display_flush();
        CHECK(s_shadow[0][15],
              "top-right base write erased Bluetooth indicator");

        s_set_start = s_set_count;
        s_flush_start = s_flush_count;
        display_set_bluetooth_indicator(false);
        CHECK(s_set_count == s_set_start && last_flush_calls() == 0,
              "disabling Bluetooth must wait for the caller's base repaint");

        display_show_rgba(rgba);
        CHECK(!s_shadow[0][15],
              "post-disable base repaint left the indicator lit");

        display_set_pixel(15, 0, true);
        display_flush();
        CHECK(s_shadow[0][15],
              "disabled indicator intercepted the base top-right LED");

        display_set_pixel(15, 0, false);
        display_flush();
    }

    if (s_failures != 0)
    {
        fprintf(stderr, "volume overlay rev05 test: %d assertion(s) failed\n",
                s_failures);
        return 1;
    }

    printf("volume overlay rev05 test: all assertions passed\n");
    return 0;
}
