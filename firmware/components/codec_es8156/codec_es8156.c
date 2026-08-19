/*
 * codec_es8156.c — ES8156 headphone DAC driver (I2C).
 *
 * Register bring-up ported from Espressif's esp_codec_dev / esp-adf ES8156
 * driver (device/es8156/es8156.c, es8156_open + es8156_start) and checked
 * against the ES8156 datasheet (Rev 6.0) plus ESPHome's documented driver.
 *
 * The chip boots in hardware mode; bit 2 of register 0x02 switches it to
 * software (I2C) mode so that volume and mute are I2C-controlled. The shared
 * I2C bus is installed by iox_init(); this driver only writes registers over
 * the existing bus with the legacy i2c_master_write_to_device API, matching
 * iox.c.
 */
#include "codec_es8156.h"
#include "board_pins.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "codec_es8156";

/* ES8156 register map (subset used by this driver). */
#define ES8156_REG_RESET         0x00
#define ES8156_REG_SCLK_MODE     0x02
#define ES8156_REG_CLOCK_ON_OFF  0x08
#define ES8156_REG_MISC_CTRL2    0x09
#define ES8156_REG_TIME_CTRL1    0x0A
#define ES8156_REG_TIME_CTRL2    0x0B
#define ES8156_REG_P2S_CTRL      0x0D
#define ES8156_REG_DAC_SDP       0x11
#define ES8156_REG_DAC_MUTE      0x13
#define ES8156_REG_VOLUME        0x14
#define ES8156_REG_MISC_CTRL3    0x18
#define ES8156_REG_EQ_CTRL1      0x19
#define ES8156_REG_ANALOG_SYS1   0x20
#define ES8156_REG_ANALOG_SYS2   0x21
#define ES8156_REG_ANALOG_SYS3   0x22
#define ES8156_REG_ANALOG_SYS4   0x23
#define ES8156_REG_ANALOG_LP     0x24
#define ES8156_REG_ANALOG_SYS5   0x25

/* Volume register 0x14: 0x00 = -95.5 dB .. 0xFF = +32 dB, in 0.5 dB steps. */
#define ES8156_VOL_DEFAULT       179   /* ~70 %, matches esp_codec_dev */

/* Digital DAC mute (reg 0x13) and analog output mute (reg 0x22) bit masks. */
#define ES8156_MUTE_DIG_MASK     0x06   /* bits 1..2 of reg 0x13 */
#define ES8156_MUTE_ANA_MASK     0x02   /* bit 1 (OUT_MUTE) of reg 0x22 */

#define ES8156_I2C_TIMEOUT_MS    100

typedef struct
{
    uint8_t reg;
    uint8_t val;
} codec_es8156_reg_t;

/*
 * Software-mode bring-up sequence: es8156_open followed by es8156_start from
 * esp_codec_dev, in the reference driver's order. Reg 0x11 = 0x30 selects the
 * 16-bit I2S (Philips) slave input (the board streams 16-bit PCM), where the
 * reference driver leaves 0x00 (default). Reg 0x00 = 0x02/0x03 asserts then
 * releases the digital reset and enables the chip state machine.
 */
static const codec_es8156_reg_t k_es8156_init_regs[] = {
    { ES8156_REG_SCLK_MODE,    0x04 },                 /* bit2: software (I2C) mode */
    { ES8156_REG_ANALOG_SYS1,  0x2A },                 /* analog signal path */
    { ES8156_REG_ANALOG_SYS2,  0x3C },                 /* VSEL bias ~120 %, normal VREF ramp */
    { ES8156_REG_ANALOG_SYS3,  0x00 },                 /* line-out, OUT_MUTE=0 */
    { ES8156_REG_ANALOG_LP,    0x07 },                 /* low-power VREFBUF/HPCOM/DACVRP */
    { ES8156_REG_ANALOG_SYS4,  0x00 },                 /* normal bias/impedance */
    { ES8156_REG_TIME_CTRL1,   0x01 },                 /* fast state-machine transitions */
    { ES8156_REG_TIME_CTRL2,   0x01 },
    { ES8156_REG_DAC_SDP,      0x30 },                 /* I2S, 16-bit, slave */
    { ES8156_REG_VOLUME,       ES8156_VOL_DEFAULT },   /* 0.5 dB step, ~70 % */
    { ES8156_REG_P2S_CTRL,     0x14 },                 /* parallel-to-serial converter */
    { ES8156_REG_MISC_CTRL3,   0x00 },                 /* L->L / R->R, no inversion */
    { ES8156_REG_CLOCK_ON_OFF, 0x3F },                 /* enable all internal clocks */
    { ES8156_REG_RESET,        0x02 },                 /* assert digital reset */
    { ES8156_REG_RESET,        0x03 },                 /* exit reset, enable chip state machine */
    { ES8156_REG_ANALOG_SYS5,  0x20 },                 /* power up analog blocks */
    { ES8156_REG_MISC_CTRL2,   0x00 },
    { ES8156_REG_EQ_CTRL1,     0x20 },                 /* EQ disabled */
};

#define ES8156_INIT_REG_COUNT \
    (sizeof(k_es8156_init_regs) / sizeof(k_es8156_init_regs[0]))

/**
 * Write one 8-bit register over the shared I2C bus.
 *
 * reg register address, val value to write.
 *
 * return ESP_OK on success, otherwise the I2C transaction error.
 */
static esp_err_t codec_es8156_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    return i2c_master_write_to_device(I2C_PORT, I2C_ADDR_HP_DAC, buf,
                                      sizeof(buf),
                                      pdMS_TO_TICKS(ES8156_I2C_TIMEOUT_MS));
}

/**
 * Apply or release both the digital DAC mute (reg 0x13) and the analog output
 * mute (reg 0x22).
 *
 * mute true mutes, false unmutes.
 *
 * return ESP_OK on success, otherwise the first I2C error encountered.
 */
static esp_err_t codec_es8156_set_mute(bool mute)
{
    esp_err_t err;

    err = codec_es8156_write_reg(ES8156_REG_DAC_MUTE,
                                 mute ? ES8156_MUTE_DIG_MASK : 0x00);
    if (err != ESP_OK)
    {
        return err;
    }

    return codec_es8156_write_reg(ES8156_REG_ANALOG_SYS3,
                                  mute ? ES8156_MUTE_ANA_MASK : 0x00);
}

esp_err_t codec_es8156_init(void)
{
    esp_err_t err;
    size_t i;

    for (i = 0; i < ES8156_INIT_REG_COUNT; i++)
    {
        err = codec_es8156_write_reg(k_es8156_init_regs[i].reg,
                                     k_es8156_init_regs[i].val);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "init write reg 0x%02x failed: %s",
                     k_es8156_init_regs[i].reg, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "ES8156 initialized (addr 0x%02x, 16-bit I2S slave)",
             I2C_ADDR_HP_DAC);
    return ESP_OK;
}

esp_err_t codec_es8156_set_volume(int vol)
{
    uint8_t reg;

    if (vol < 0)
    {
        vol = 0;
    }
    else if (vol > 100)
    {
        vol = 100;
    }

    /* Map 0..100 % linearly onto register 0x00..0xFF, rounding to nearest. */
    reg = (uint8_t)((vol * 255 + 50) / 100);

    return codec_es8156_write_reg(ES8156_REG_VOLUME, reg);
}

esp_err_t codec_es8156_mute(bool mute)
{
    return codec_es8156_set_mute(mute);
}

esp_err_t codec_es8156_unmute(void)
{
    return codec_es8156_set_mute(false);
}
