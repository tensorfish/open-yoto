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

#define GC9306_SPI_CLK_HZ     40000000
#define GC9306_SPI_MODE       0
#define GC9306_PANEL_W        240
#define GC9306_PANEL_H        320
#define GC9306_CHUNK_PIXELS   341           /* 341 px x 3 bytes = 1023 B/tx  */
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
 * One init group: CS asserted, command + parameters all at DC=0 (stock
 * inter-register behaviour), CS released.
 */
static esp_err_t gc9306_init_step(const gc9306_init_step_t *step)
{
    esp_err_t err = iox_set_pin(IOX_TFT_CS, false);
    if (err != ESP_OK)
    {
        return err;
    }
    err = gc9306_tx_dc(&step->cmd, 1, false);
    if (err == ESP_OK && step->n > 0)
    {
        err = gc9306_tx_dc(step->data, step->n, false);
    }
    (void)iox_set_pin(IOX_TFT_CS, true);
    return err;
}

static void gc9306_reset(void)
{
    (void)iox_set_pin(IOX_TFT_RESET, false);
    vTaskDelay(pdMS_TO_TICKS(20));
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
        .spics_io_num = -1,           /* CS handled via IOX */
        .queue_size = 1,
        /* Write-only panel (MISO unwired on #04): half-duplex is required
         * for 40 MHz over the GPIO matrix (full-duplex would need read
         * dummy cycles that the driver rejects with ESP_ERR_NOT_SUPPORTED). */
        .flags = SPI_DEVICE_HALFDUPLEX,
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
        err = gc9306_init_step(&k_init[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "init step %u (0x%02x): %s",
                     (unsigned)i, k_init[i].cmd, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "GC9306 init done (SPI2 mosi=22 sclk=23, cs=IOX.0.0 dc=IOX.0.1 reset=IOX.0.2, 18-bit RGB666)");
    return ESP_OK;
}

/*
 * Stock display-on sequence: sleep out, delay, 0x53/0x00, display on.
 */
esp_err_t gc9306_display_on(void)
{
    esp_err_t err;

    err = gc9306_init_step(&(gc9306_init_step_t){ 0x11, { }, 0 });
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));

    err = gc9306_init_step(&(gc9306_init_step_t){ 0x53, { 0x00 }, 1 });
    if (err != ESP_OK) return err;

    return gc9306_init_step(&(gc9306_init_step_t){ 0x29, { }, 0 });
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

        /* Panel colour pre-transform: this GC9306 renders the 18-bit stream
         * as displayed = (XNOR(R,G), G, G XOR B) per 6-bit channel —
         * recovered empirically from two 4-stripe tests (8 clean data
         * points, all fit exactly). Sending the inverse
         * (XNOR(Rd,Gd), Gd, Gd XOR Bd) makes the panel display the
         * requested colour (Rd, Gd, Bd). */
        uint8_t rd = (uint8_t)(((color >> 16) & 0xFF) >> 2);  /* top 6 bits */
        uint8_t gd = (uint8_t)(((color >> 8) & 0xFF) >> 2);
        uint8_t bd = (uint8_t)((color & 0xFF) >> 2);
        uint8_t rs = (uint8_t)(~(rd ^ gd) & 0x3F);
        uint8_t gs = gd;
        uint8_t bs = (uint8_t)((gd ^ bd) & 0x3F);

        for (size_t i = 0; i < GC9306_CHUNK_PIXELS; i++)
        {
            s_chunk[i * 3 + 0] = (uint8_t)(rs << 2);
            s_chunk[i * 3 + 1] = (uint8_t)(gs << 2);
            s_chunk[i * 3 + 2] = (uint8_t)(bs << 2);
        }

        while (sent < total)
        {
            size_t n = total - sent;
            if (n > GC9306_CHUNK_PIXELS)
            {
                n = GC9306_CHUNK_PIXELS;
            }
            spi_transaction_t t;
            memset(&t, 0, sizeof(t));
            t.length = (size_t)(n * GC9306_BYTES_PER_PX * 8);
            t.tx_buffer = s_chunk;
            err = spi_device_polling_transmit(s_spi, &t);
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
