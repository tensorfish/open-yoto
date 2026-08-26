/*
 * gc9306.c — GC9306 TFT LCD driver (rev #04 board).
 *
 * Register init replicated from the stock factory image (output/factory.bin):
 * the init sequence lives in the factory app's GC9306 driver (init function
 * @ 0x40108946, command writer 0x401083ec, data writer 0x40108454). The
 * sequence opens with the GalaxyCore inter-register enable pair (0xFE 0xEF),
 * then writes vendor registers; COLMOD=0x06 selects 18-bit RGB666.
 *
 * Pin protocol recovered from the same code (hwconfig_04: cs=IOX.0.0,
 * dc=IOX.0.1, reset=IOX.0.2, pwm=GPIO26):
 *   - each init command is a CS-asserted group; parameter bytes are clocked
 *     with DC low (inter-register mode) — replicated exactly;
 *   - window/data writes (CASET/RASET/RAMWR payloads) use DC high, CS held
 *     across the whole rect like the stock draw_rect.
 */
#include "gc9306.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "iox.h"

static const char *TAG = "gc9306";

#define GC9306_SPI_CLK_HZ     80000000
#define GC9306_SPI_MODE       0
#define GC9306_PANEL_W        240
#define GC9306_PANEL_H        320
#define GC9306_CHUNK_PIXELS   1364          /* stock chunk: 0x554 pixels */
#define GC9306_BYTES_PER_PX   3             /* 18-bit RGB666                */

/* Stock init table: command + parameter bytes (all clocked at DC=0). */
typedef struct
{
    uint8_t cmd;
    uint8_t data[6];
    uint8_t n;
} gc9306_init_step_t;

static const gc9306_init_step_t k_init[] = {
    { 0xFE, {                 }, 0 },   /* GalaxyCore inter-register enable */
    { 0xEF, {                 }, 0 },
    { 0x36, { 0x48          }, 1 },   /* MADCTL */
    { 0x3A, { 0x06          }, 1 },   /* COLMOD: 18-bit RGB666 */
    { 0xA4, { 0x44, 0x44    }, 2 },
    { 0xA5, { 0x42, 0x42    }, 2 },
    { 0xAA, { 0x88, 0x88    }, 2 },
    { 0xAE, { 0x2B          }, 1 },
    { 0xE8, { 0x11, 0x0B    }, 2 },
    { 0xE3, { 0x01, 0x10    }, 2 },
    { 0xFF, { 0x61          }, 1 },
    { 0xAC, { 0x00          }, 1 },
    { 0xAD, { 0x33          }, 1 },
    { 0xAF, { 0x77          }, 1 },
    { 0xA6, { 0x1C, 0x1C    }, 2 },
    { 0xA7, { 0x1C, 0x1C    }, 2 },
    { 0xA8, { 0x10, 0x10    }, 2 },
    { 0xA9, { 0x0D, 0x0D    }, 2 },
    { 0xF0, { 0x02, 0x01, 0x00, 0x00, 0x00, 0x05 }, 6 },
    { 0xF1, { 0x01, 0x02, 0x00, 0x06, 0x10, 0x0E }, 6 },
    { 0xF2, { 0x03, 0x11, 0x28, 0x02, 0x00, 0x48 }, 6 },
};
#define GC9306_INIT_STEPS (sizeof(k_init) / sizeof(k_init[0]))

static spi_device_handle_t s_spi = NULL;
static uint8_t s_chunk[GC9306_CHUNK_PIXELS * GC9306_BYTES_PER_PX];
static bool s_color64_active;

/*
 * The panel running the recovered stock MADCTL mode presents the R/B channels
 * reversed for direct 18-bit writes. Keep source colors in ordinary RGB888
 * everywhere else and correct the byte order at this single output boundary.
 */
static inline void gc9306_store_rgb(size_t pixel, uint8_t red, uint8_t green,
                                    uint8_t blue)
{
    s_chunk[pixel * 3 + 0] = blue;
    s_chunk[pixel * 3 + 1] = green;
    s_chunk[pixel * 3 + 2] = red;
}

/*
 * Raw byte transfer with the given D/CX level; CS must already be asserted.
 */
static esp_err_t gc9306_tx_dc(const uint8_t *data, size_t len, bool dc)
{
    esp_err_t err = iox_set_pin(IOX_TFT_DC, dc);
    if (err != ESP_OK)
    {
        return err;
    }
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (size_t)(len * 8);
    t.tx_buffer = data;
    return spi_device_polling_transmit(s_spi, &t);
}

/*
 * One stock controller group: CS asserted, every byte at DC=0, CS released.
 * The GC9306's inter-register init parameters are deliberately DC-low.
 */
static esp_err_t gc9306_stock_group(const uint8_t *data, size_t len)
{
    esp_err_t err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gc9306_tx_dc(data, len, false);
    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

static void gc9306_reset(void)
{
    /* Stock reset sequence (factory image @ 0x40108ede): high 50 ms,
     * low 50 ms, high 120 ms. A shorter/different pulse can leave the
     * panel in an arbitrary colour mode (observed: the transfer function
     * changed every boot with the old 20/120 ms pulse). */
    (void)iox_set_pin(IOX_TFT_RESET, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)iox_set_pin(IOX_TFT_RESET, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)iox_set_pin(IOX_TFT_RESET, true);
    vTaskDelay(pdMS_TO_TICKS(120));
}

esp_err_t gc9306_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_chunk),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = GC9306_SPI_CLK_HZ,
        .mode = GC9306_SPI_MODE,
        .spics_io_num = -1,           /* CS handled through IOX.0.0 */
        .queue_size = 1,
        /* Stock config: mode=0, clock=80 MHz, flags=0x40
         * (SPI_DEVICE_NO_DUMMY), manual CS, queue depth 1. */
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    gc9306_reset();

    for (size_t i = 0; i < GC9306_INIT_STEPS; i++)
    {
        uint8_t group[1 + sizeof(k_init[i].data)] = { k_init[i].cmd };

        memcpy(group + 1, k_init[i].data, k_init[i].n);
        err = gc9306_stock_group(group, (size_t)k_init[i].n + 1);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "init step %u (0x%02x): %s",
                     (unsigned)i, k_init[i].cmd, esp_err_to_name(err));
            return err;
        }
    }

    /* Required stock tail after F2. Each row is a separate DC-low CS group
     * (0x40108e0b-0x40108e85), not one combined transfer. */
    static const uint8_t k_tail_35[] = { 0x35, 0x00 };
    static const uint8_t k_tail_44[] = { 0x44, 0x00, 0x0A };
    static const uint8_t k_tail_21[] = { 0x21 };
    static const uint8_t k_tail_11[] = { 0x11 };
    static const uint8_t k_display_on[] = { 0x29 };

    err = gc9306_stock_group(k_tail_35, sizeof(k_tail_35));
    if (err == ESP_OK)
    {
        err = gc9306_stock_group(k_tail_44, sizeof(k_tail_44));
    }
    if (err == ESP_OK)
    {
        err = gc9306_stock_group(k_tail_21, sizeof(k_tail_21));
    }
    if (err == ESP_OK)
    {
        err = gc9306_stock_group(k_tail_11, sizeof(k_tail_11));
    }
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Stock performs a 240x240 RAMWR here using an unrecovered RGB triple
     * at 0x3ffc7338. Do not invent that payload; the first caller supplies
     * its complete frame, then we issue the stock DISPON command. */
    err = gc9306_stock_group(k_display_on, sizeof(k_display_on));
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "GC9306 init done (stock SPI2 mode 0, 80 MHz, RGB666)");
    return ESP_OK;
}

/*
 * Stock resume/power-on sequence (0x40108741-0x40108797): sleep out in one
 * CS group, 120 ms delay, then 53/00/29 in a single DC-low CS group.
 */
esp_err_t gc9306_display_on(void)
{
    static const uint8_t k_sleep_out[] = { 0x11 };
    static const uint8_t k_power_on[] = { 0x53, 0x00, 0x29 };
    esp_err_t err = gc9306_stock_group(k_sleep_out, sizeof(k_sleep_out));

    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return gc9306_stock_group(k_power_on, sizeof(k_power_on));
}

esp_err_t gc9306_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint32_t color)
{
    uint8_t b[4];
    esp_err_t err;

    if (x1 < x0 || y1 < y0 || x1 >= GC9306_PANEL_W || y1 >= GC9306_PANEL_H)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* CS held across the whole rect (stock draw_rect behaviour). */
    err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK) return err;

    /* CASET */
    b[0] = 0x2A;
    err = gc9306_tx_dc(b, 1, false);
    if (err == ESP_OK)
    {
        b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
        b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
        err = gc9306_tx_dc(b, 4, true);          /* data: DC high */
    }
    /* RASET */
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
        b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
        err = gc9306_tx_dc(b, 4, true);
    }
    /* RAMWR + pixels (18-bit: R G B per pixel) */
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }
    if (err == ESP_OK)
    {
        size_t total = (size_t)(x1 - x0 + 1) * (y1 - y0 + 1);
        size_t sent = 0;
        uint8_t red = (uint8_t)((color >> 16) & 0xFF);
        uint8_t green = (uint8_t)((color >> 8) & 0xFF);
        uint8_t blue = (uint8_t)(color & 0xFF);

        /* The transport is RGB666; gc9306_store_rgb applies this panel's
         * observed R/B correction before each 3-byte write. */
        for (size_t i = 0; i < GC9306_CHUNK_PIXELS; i++)
        {
            gc9306_store_rgb(i, red, green, blue);
        }

        while (sent < total)
        {
            spi_transaction_t t = { 0 };
            spi_transaction_t *completed;
            size_t n = total - sent;

            if (n > GC9306_CHUNK_PIXELS)
            {
                n = GC9306_CHUNK_PIXELS;
            }
            t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
            t.tx_buffer = s_chunk;

            /* Stock queues one 0x554-pixel transaction then waits before
             * reusing the staging buffer (0x40109033-0x40109091). */
            err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
            if (err != ESP_OK)
            {
                break;
            }
            err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
            if (err != ESP_OK)
            {
                break;
            }
            sent += n;
        }
    }

    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

esp_err_t gc9306_draw_rgba16(const uint8_t rgba[16 * 16 * 4])
{
    enum {
        CANVAS_SCALE = 15,
        CANVAS_SIZE = 16 * CANVAS_SCALE,
    };
    uint8_t b[4];
    esp_err_t err;
    size_t sent = 0;
    const size_t total = CANVAS_SIZE * CANVAS_SIZE;

    if (rgba == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }

    b[0] = 0x2A;
    err = gc9306_tx_dc(b, 1, false);
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x00;
        b[3] = CANVAS_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x00;
        b[3] = CANVAS_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }

    while (err == ESP_OK && sent < total)
    {
        spi_transaction_t t = { 0 };
        spi_transaction_t *completed;
        size_t n = total - sent;

        if (n > GC9306_CHUNK_PIXELS)
        {
            n = GC9306_CHUNK_PIXELS;
        }

        for (size_t i = 0; i < n; i++)
        {
            size_t pixel = sent + i;
            size_t sx = 15 - ((pixel % CANVAS_SIZE) / CANVAS_SCALE);
            size_t sy = (pixel / CANVAS_SIZE) / CANVAS_SCALE;
            const uint8_t *src = &rgba[(sy * 16 + sx) * 4];
            uint8_t alpha = src[3];

            /* Alpha-compose in source RGB, then correct channel order only
             * while writing to this panel's RGB666 stream. */
            gc9306_store_rgb(i,
                              (uint8_t)(((uint16_t)src[0] * alpha) / 255),
                              (uint8_t)(((uint16_t)src[1] * alpha) / 255),
                              (uint8_t)(((uint16_t)src[2] * alpha) / 255));
        }

        t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
        t.tx_buffer = s_chunk;
        err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
        if (err == ESP_OK)
        {
            err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
        }
        sent += n;
    }

    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

esp_err_t gc9306_draw_rgb56516_full(const uint16_t pixels[16 * 16])
{
    uint8_t b[4];
    esp_err_t err;
    size_t sent = 0;
    /* The visible rev-04 canvas is square. Keeping 16:16 artwork at 15x15
     * preserves its aspect ratio and avoids driving the cropped lower rows. */
    const size_t total = GC9306_PANEL_W * GC9306_PANEL_W;

    if (pixels == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }

    b[0] = 0x2A;
    err = gc9306_tx_dc(b, 1, false);
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = (GC9306_PANEL_W - 1) >> 8;
        b[3] = (GC9306_PANEL_W - 1) & 0xFF;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = (GC9306_PANEL_W - 1) >> 8;
        b[3] = (GC9306_PANEL_W - 1) & 0xFF;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }

    while (err == ESP_OK && sent < total)
    {
        spi_transaction_t t = { 0 };
        spi_transaction_t *completed;
        size_t n = total - sent;

        if (n > GC9306_CHUNK_PIXELS)
        {
            n = GC9306_CHUNK_PIXELS;
        }
        for (size_t i = 0; i < n; i++)
        {
            size_t pixel = sent + i;
            size_t x = pixel % GC9306_PANEL_W;
            size_t y = pixel / GC9306_PANEL_W;
            uint16_t color = pixels[(y / 15) * 16 + 15 - (x / 15)];

            gc9306_store_rgb(i, (uint8_t)(((color >> 11) & 0x1F) << 3),
                              (uint8_t)(((color >> 5) & 0x3F) << 2),
                              (uint8_t)((color & 0x1F) << 3));
        }
        t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
        t.tx_buffer = s_chunk;
        err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
        if (err == ESP_OK)
        {
            err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
        }
        sent += n;
    }

    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

esp_err_t gc9306_color64_begin(void)
{
    enum {
        COLOR_SCALE = 3,
        COLOR_X = 24,
        COLOR_Y = 24,
        COLOR_SIZE = 64 * COLOR_SCALE,
    };
    uint8_t b[4];
    esp_err_t err = gc9306_fill_rect(0, 0, 239, 319, 0x000000);

    if (err != ESP_OK)
    {
        return err;
    }
    err = iox_set_pin(IOX_TFT_CS, false);
    if (err == ESP_OK)
    {
        b[0] = 0x2A;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = COLOR_X;
        b[2] = 0x00;
        b[3] = COLOR_X + COLOR_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = COLOR_Y;
        b[2] = 0x00;
        b[3] = COLOR_Y + COLOR_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }

    s_color64_active = err == ESP_OK;
    if (!s_color64_active)
    {
        (void)iox_set_pin(IOX_TFT_CS, true);
    }
    return err;
}

esp_err_t gc9306_color64_write_row(const uint16_t pixels[64])
{
    enum {
        SOURCE_WIDTH = 64,
        COLOR_SCALE = 3,
        OUTPUT_WIDTH = SOURCE_WIDTH * COLOR_SCALE,
        OUTPUT_PIXELS = OUTPUT_WIDTH * COLOR_SCALE,
    };
    spi_transaction_t t = { 0 };
    spi_transaction_t *completed;

    if (!s_color64_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < OUTPUT_PIXELS; i++)
    {
        size_t output_x = i % OUTPUT_WIDTH;
        size_t source_x = SOURCE_WIDTH - 1 - output_x / COLOR_SCALE;
        uint16_t color = pixels[source_x];
        uint8_t red5 = (uint8_t)((color >> 11) & 0x1F);
        uint8_t green6 = (uint8_t)((color >> 5) & 0x3F);
        uint8_t blue5 = (uint8_t)(color & 0x1F);

        gc9306_store_rgb(i,
                          (uint8_t)((red5 << 3) | (red5 >> 2)),
                          (uint8_t)((green6 << 2) | (green6 >> 4)),
                          (uint8_t)((blue5 << 3) | (blue5 >> 2)));
    }

    t.length = OUTPUT_PIXELS * GC9306_BYTES_PER_PX * 8;
    t.tx_buffer = s_chunk;
    esp_err_t err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
    if (err == ESP_OK)
    {
        err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
    }
    return err;
}

esp_err_t gc9306_color64_end(void)
{
    esp_err_t err;

    if (!s_color64_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_color64_active = false;
    err = iox_set_pin(IOX_TFT_CS, true);
    return err;
}

esp_err_t gc9306_draw_mask192(const uint8_t mask[192 * 192 / 8],
                              uint32_t foreground, uint32_t background)
{
    enum {
        STOCK_X = 24,
        STOCK_Y = 27,
        STOCK_SIZE = 192,
    };
    uint8_t b[4];
    uint8_t fr = (uint8_t)((foreground >> 16) & 0xFF);
    uint8_t fg = (uint8_t)((foreground >> 8) & 0xFF);
    uint8_t fb = (uint8_t)(foreground & 0xFF);
    uint8_t br = (uint8_t)((background >> 16) & 0xFF);
    uint8_t bg = (uint8_t)((background >> 8) & 0xFF);
    uint8_t bb = (uint8_t)(background & 0xFF);
    esp_err_t err;
    size_t sent = 0;
    const size_t total = STOCK_SIZE * STOCK_SIZE;

    if (mask == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }

    b[0] = 0x2A;
    err = gc9306_tx_dc(b, 1, false);
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = STOCK_X;
        b[2] = 0x00;
        b[3] = STOCK_X + STOCK_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = STOCK_Y;
        b[2] = 0x00;
        b[3] = STOCK_Y + STOCK_SIZE - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }

    while (err == ESP_OK && sent < total)
    {
        spi_transaction_t t = { 0 };
        spi_transaction_t *completed;
        size_t n = total - sent;

        if (n > GC9306_CHUNK_PIXELS)
        {
            n = GC9306_CHUNK_PIXELS;
        }

        for (size_t i = 0; i < n; i++)
        {
            size_t pixel = sent + i;
            bool on = (mask[pixel / 8] & (uint8_t)(1U << (7 - (pixel % 8)))) != 0;

            if (on)
            {
                gc9306_store_rgb(i, fr, fg, fb);
            }
            else
            {
                gc9306_store_rgb(i, br, bg, bb);
            }
        }

        t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
        t.tx_buffer = s_chunk;
        err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
        if (err == ESP_OK)
        {
            err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
        }
        sent += n;
    }

    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

esp_err_t gc9306_draw_mask_full(const uint8_t mask[240 * 320 / 8],
                                uint32_t foreground, uint32_t background)
{
    enum {
        WIDTH = 240,
        HEIGHT = 320,
    };
    uint8_t b[4];
    uint8_t fr = (uint8_t)((foreground >> 16) & 0xFF);
    uint8_t fg = (uint8_t)((foreground >> 8) & 0xFF);
    uint8_t fb = (uint8_t)(foreground & 0xFF);
    uint8_t br = (uint8_t)((background >> 16) & 0xFF);
    uint8_t bg = (uint8_t)((background >> 8) & 0xFF);
    uint8_t bb = (uint8_t)(background & 0xFF);
    esp_err_t err;
    size_t sent = 0;
    const size_t total = WIDTH * HEIGHT;

    if (mask == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }

    b[0] = 0x2A;
    err = gc9306_tx_dc(b, 1, false);
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x00;
        b[3] = WIDTH - 1;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2B;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x01;
        b[3] = 0x3F;
        err = gc9306_tx_dc(b, 4, true);
    }
    if (err == ESP_OK)
    {
        b[0] = 0x2C;
        err = gc9306_tx_dc(b, 1, false);
    }
    if (err == ESP_OK)
    {
        err = iox_set_pin(IOX_TFT_DC, true);
    }

    while (err == ESP_OK && sent < total)
    {
        spi_transaction_t t = { 0 };
        spi_transaction_t *completed;
        size_t n = total - sent;

        if (n > GC9306_CHUNK_PIXELS)
        {
            n = GC9306_CHUNK_PIXELS;
        }

        for (size_t i = 0; i < n; i++)
        {
            size_t pixel = sent + i;
            bool on = (mask[pixel / 8] & (uint8_t)(1U << (7 - (pixel % 8)))) != 0;

            if (on)
            {
                gc9306_store_rgb(i, fr, fg, fb);
            }
            else
            {
                gc9306_store_rgb(i, br, bg, bb);
            }
        }

        t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
        t.tx_buffer = s_chunk;
        err = spi_device_queue_trans(s_spi, &t, portMAX_DELAY);
        if (err == ESP_OK)
        {
            err = spi_device_get_trans_result(s_spi, &completed, portMAX_DELAY);
        }
        sent += n;
    }

    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}
