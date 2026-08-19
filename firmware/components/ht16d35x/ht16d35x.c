/*
 * ht16d35x.c — HT16D35B 16x16 monochrome LED matrix driver (SPI + IOX CS).
 *
 * The display is a 16x16 grid made from four HT16D35B controllers arranged as
 * a 2x2 matrix. Each controller drives one 8x8 quadrant, so in BINARY mode one
 * quadrant row is exactly one 8-bit RAM byte.
 *
 * The SPI bus is operated half-duplex (MOSI is the only data line) because the
 * display is write-only; PIN_SPI_MISO stays free for rotary encoder 0. The four
 * chip selects are not owned by the SPI peripheral — they are GPIOs on the
 * second IO expander (IOX_DISP_CSN0..3, active low) driven with iox_set_pin().
 */
#include "ht16d35x.h"

#include <string.h>

#include "board_pins.h"
#include "iox.h"

#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "ht16d35x";

/* ------------------------------------------------------------------ board */
#define HT16D35X_CHIP_COUNT          4
#define HT16D35X_ROWS                16
#define HT16D35X_COLS                16
#define HT16D35X_QUAD_ROWS           8
#define HT16D35X_QUAD_COLS           8

/* -------------------------------------------------------------------- SPI */
#define HT16D35X_SPI_CLK_HZ          1000000   /* fCLK well under the 5 MHz max */
#define HT16D35X_SPI_MODE            0         /* CPOL=0, CPHA=0                */
#define HT16D35X_MAX_TRANSFER_BYTES  64
#define HT16D35X_TX_LEN              (HT16D35X_QUAD_ROWS + 2U)

/*
 * HT16D35B command opcodes, recovered from the stock firmware's init sequence.
 * TODO: verify each opcode's bit field against the Holtek HT16D35B datasheet;
 * the semantic names below reflect the factory ordering, not a datasheet quote.
 */
#define HT16D35X_CMD_COM_EN          0x41   /* enable COM outputs              */
#define HT16D35X_CMD_ROW_EN          0x42   /* enable ROW outputs              */
#define HT16D35X_CMD_DATA_MODE       0x31   /* operating-mode select           */
#define HT16D35X_DATA_BINARY         0x01   /* BINARY mode (1 bit/pixel)       */
#define HT16D35X_CMD_SCAN            0x32   /* COM scan configuration          */
#define HT16D35X_CMD_CURRENT         0x36   /* LED output current              */
#define HT16D35X_CMD_BRIGHTNESS      0x37   /* PWM brightness                  */
#define HT16D35X_CMD_OSC_ON          0x35   /* internal RC oscillator on       */

#define HT16D35X_CMD_WRITE_RAM       0x80   /* write display RAM, auto-increment */
#define HT16D35X_RAM_BASE            0x00   /* first RAM byte of a quadrant    */

/* Chip selects live on expander 1 and are active low. */
static const uint8_t HT16D35X_CS_PINS[HT16D35X_CHIP_COUNT] =
{
    IOX_DISP_CSN0,
    IOX_DISP_CSN1,
    IOX_DISP_CSN2,
    IOX_DISP_CSN3,
};

/* ------------------------------------------------------------------ state */
static spi_device_handle_t s_spi_device = NULL;

/* One 16-bit word per row; bit (x) of s_framebuffer[y] is pixel (x, y). */
static uint16_t s_framebuffer[HT16D35X_ROWS];

/*
 * Transmit bytes over the half-duplex SPI bus (blocking, no CS handling).
 *
 * @param[in] data  Byte buffer to clock out.
 * @param[in] len   Number of bytes in data.
 * @return ESP_OK on success, or an ESP_ERR_* code from the SPI driver.
 */
static esp_err_t ht16d35x_write_bytes(const uint8_t *data, size_t len)
{
    spi_transaction_t trans;

    memset(&trans, 0, sizeof(trans));
    trans.length = (size_t)(len * 8U);
    trans.tx_buffer = data;

    return spi_device_polling_transmit(s_spi_device, &trans);
}

/*
 * Assert (or deassert) a chip select through the IO expander.
 *
 * @param[in] chip    Controller index, 0..3.
 * @param[in] assert  True to select the chip, false to deselect it.
 * @return ESP_OK on success, or an ESP_ERR_* code from iox_set_pin().
 */
static esp_err_t ht16d35x_select(uint8_t chip, bool assert)
{
    /* CS is active low: level false selects the chip. */
    return iox_set_pin(HT16D35X_CS_PINS[chip], !assert);
}

/*
 * Send the BINARY-mode init sequence to one controller.
 *
 * @param[in] chip  Controller index, 0..3.
 * @return ESP_OK on success, or an ESP_ERR_* code from the SPI/IOX layers.
 */
static esp_err_t ht16d35x_init_chip(uint8_t chip)
{
    static const uint8_t INIT_SEQUENCE[] =
    {
        HT16D35X_CMD_COM_EN,
        HT16D35X_CMD_ROW_EN,
        HT16D35X_CMD_DATA_MODE,
        HT16D35X_DATA_BINARY,
        HT16D35X_CMD_SCAN,
        HT16D35X_CMD_CURRENT,
        HT16D35X_CMD_BRIGHTNESS,
        HT16D35X_CMD_OSC_ON,
    };
    esp_err_t err;

    err = ht16d35x_select(chip, true);
    if (err != ESP_OK)
    {
        return err;
    }

    err = ht16d35x_write_bytes(INIT_SEQUENCE, sizeof(INIT_SEQUENCE));
    if (err != ESP_OK)
    {
        (void)ht16d35x_select(chip, false);
        return err;
    }

    return ht16d35x_select(chip, false);
}

esp_err_t ht16d35x_init(void)
{
    esp_err_t err;
    uint8_t chip;

    if (s_spi_device != NULL)
    {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg =
    {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HT16D35X_MAX_TRANSFER_BYTES,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_MOSI |
                 SPICOMMON_BUSFLAG_SCLK,
    };

    err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg =
    {
        .clock_speed_hz = HT16D35X_SPI_CLK_HZ,
        .mode = HT16D35X_SPI_MODE,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_device);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    for (chip = 0; chip < HT16D35X_CHIP_COUNT; chip++)
    {
        err = ht16d35x_init_chip(chip);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "chip %u init failed: %s", (unsigned)chip,
                     esp_err_to_name(err));
            return err;
        }
    }

    ht16d35x_clear();
    return ESP_OK;
}

void ht16d35x_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void ht16d35x_set_pixel(int x, int y, bool on)
{
    uint16_t mask;

    if ((x < 0) || (x >= HT16D35X_COLS) || (y < 0) || (y >= HT16D35X_ROWS))
    {
        return;
    }

    mask = (uint16_t)(1U << x);
    if (on)
    {
        s_framebuffer[y] |= mask;
    }
    else
    {
        s_framebuffer[y] &= (uint16_t)~mask;
    }
}

void ht16d35x_flush(void)
{
    uint8_t tx[HT16D35X_TX_LEN];
    uint8_t chip;
    uint8_t row;

    if (s_spi_device == NULL)
    {
        return;
    }

    for (chip = 0; chip < HT16D35X_CHIP_COUNT; chip++)
    {
        /* Quadrant origin: chips 0/1 are the top row, 2/3 the bottom. */
        uint8_t col_offset = (uint8_t)((chip & 1U) * HT16D35X_QUAD_COLS);
        uint8_t row_offset = (uint8_t)(((chip & 2U) >> 1U) * HT16D35X_QUAD_ROWS);

        tx[0] = HT16D35X_CMD_WRITE_RAM;
        tx[1] = HT16D35X_RAM_BASE;

        for (row = 0; row < HT16D35X_QUAD_ROWS; row++)
        {
            uint8_t col;
            uint8_t byte = 0U;

            for (col = 0; col < HT16D35X_QUAD_COLS; col++)
            {
                uint16_t bit = (uint16_t)(1U << (col_offset + col));

                /* TODO: confirm bit-to-SEG order against the datasheet; this
                 * maps local column 0 to the byte LSB. */
                if ((s_framebuffer[row_offset + row] & bit) != 0U)
                {
                    byte |= (uint8_t)(1U << col);
                }
            }

            tx[row + 2U] = byte;
        }

        (void)ht16d35x_select(chip, true);
        (void)ht16d35x_write_bytes(tx, sizeof(tx));
        (void)ht16d35x_select(chip, false);
    }
}
