/*
 * display.c — HT16D35x 16x16 LED matrix driver.
 *
 * The four HT16D35x panels share one SPI bus (mosi/miso/sclk) and are selected
 * via the PI4IOE5V6416 IO expander (IOX_DISP_CSN0..3). The ESP32's hardware CS
 * pin is therefore unused: the SPI device is configured without a chip select
 * (.spics_io_num = -1) and the expander pins are toggled around transfers.
 */
#include "display.h"

#include "board_pins.h"
#include "iox.h"

#include "driver/spi_master.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "display";

/* Single SPI device handle for the HT16D35x chain (CS via IO expander). */
static spi_device_handle_t s_spi = NULL;

/* 16x16 framebuffer, one byte per pixel holding a 6-bit grayscale value (0..63). */
static uint8_t s_fb[16 * 16];

/* TODO(datasheet HT16D35x): verify max SPI clock. Conservative value used. */
#define DISPLAY_SPI_CLK_HZ     (1 * 1000 * 1000)

/*
 * TODO(datasheet HT16D35x): register map + 6-bit-gray frame format.
 *
 *   - The HT16D35x stores a 16x16 panel at 6 bits/pixel => 192 bytes/frame.
 *     The exact register addresses, the 6-bit packing order (bit/byte/panel
 *     interleave), and any on/off + current-control command sequence have NOT
 *     been recovered from the repo yet.
 *   - The 4-panel CS fan-out (IOX_DISP_CSN0..3) and whether the panels are
 *     addressed individually or daisy-chained is likewise still TODO.
 */
#define DISPLAY_FRAME_BYTES    (16 * 16 * 6 / 8) /* 192 */

esp_err_t display_init(void)
{
    esp_err_t err;

    /* Shared SPI bus: MOSI/MISO/SCLK only; CS is not a bus-level signal here. */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_SPI_MOSI,
        .miso_io_num     = PIN_SPI_MISO,
        .sclk_io_num     = PIN_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_FRAME_BYTES,
    };

    err = spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* CS is asserted via the IO expander, so the ESP32 CS line is unused. */
    spi_device_interface_config_t devcfg = {
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .mode           = 0, /* TODO(datasheet HT16D35x): confirm SPI mode. */
        .clock_speed_hz = DISPLAY_SPI_CLK_HZ,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };

    err = spi_bus_add_device(SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Deassert all four panel chip-selects (active-low) as the safe idle state. */
    iox_set_pin(IOX_DISP_CSN0, true);
    iox_set_pin(IOX_DISP_CSN1, true);
    iox_set_pin(IOX_DISP_CSN2, true);
    iox_set_pin(IOX_DISP_CSN3, true);

    display_clear();

    ESP_LOGI(TAG, "HT16D35x display ready (MOSI=%d MISO=%d SCLK=%d, CS via IO expander)",
             PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_SCLK);
    return ESP_OK;
}

void display_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void display_set_pixel(int x, int y, uint8_t gray)
{
    if (x < 0 || x >= 16 || y < 0 || y >= 16) {
        return;
    }
    s_fb[y * 16 + x] = gray & 0x3F; /* 6-bit grayscale */
}

void display_flush(void)
{
    if (s_spi == NULL) {
        ESP_LOGW(TAG, "flush before init");
        return;
    }

    /*
     * TODO(datasheet HT16D35x): encode s_fb[] into the chip's real register /
     * frame format (192 bytes of 6-bit gray) and fan out across the 4 panels
     * via IOX_DISP_CSN0..3. Until that protocol is verified, this sends a raw
     * placeholder transfer on CS0 to exercise the bus end-to-end.
     */
    uint8_t tx[DISPLAY_FRAME_BYTES];
    memcpy(tx, s_fb, sizeof(tx)); /* TODO: placeholder, not the real encoding */

    spi_transaction_t t = {
        .length    = sizeof(tx) * 8,
        .tx_buffer = tx,
        .rx_buffer = NULL,
    };

    iox_set_pin(IOX_DISP_CSN0, false); /* assert (active-low) */
    esp_err_t err = spi_device_transmit(s_spi, &t);
    iox_set_pin(IOX_DISP_CSN0, true);  /* deassert */

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(err));
    }
}
