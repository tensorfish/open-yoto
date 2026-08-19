/*
 * audio.c — Yoto Player audio output driver.
 *
 * I2S (i2s_std) master TX -> ES8156 headphone DAC (I2C 0x09), with the
 * speaker amp rail (aw881xx L/R) gated through the IO expander's PA_CTRL pin.
 *
 * The shared I2C bus is owned by iox.c (iox_init), which app_main runs first,
 * so we only probe devices here — we do not re-install the I2C driver.
 */
#include "audio.h"

#include "board_pins.h"
#include "iox.h"

#include "driver/i2c.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio";

/* TX channel handle for the i2s_std driver. */
static i2s_chan_handle_t s_tx_chan = NULL;

/* ------------------------------------------------------------------- I2C -- */

/* Probe: write only the sub-address byte. A device that ACKs its own 7-bit
 * address will ACK this; nothing is mutated (no register is written). */
static esp_err_t probe_i2c_addr(uint8_t addr)
{
    uint8_t reg = 0x00;
    return i2c_master_write_to_device(I2C_PORT, addr, &reg, 1,
                                      pdMS_TO_TICKS(100));
}

/* --------------------------------------------------------------- ES8156 -- */
/* ES8156 (I2C_ADDR_HP_DAC) headphone DAC register init.
 *
 * TODO (ES8156 datasheet): the full bring-up sequence is not yet verified
 * against the chip register map, so it is left for later:
 *   - soft reset
 *   - power-up (analog + DAC power-down bits cleared)
 *   - interface format = I2S/Philips, 16-bit, slave mode (256fs from MCLK)
 *   - enable the DAC (and, if used, the on-chip headphone/mixer path)
 * Until these registers are written the DAC will stay in its reset/default
 * state and no audio will leave the ES8156. */
static esp_err_t es8156_init(void)
{
    esp_err_t err = probe_i2c_addr(I2C_ADDR_HP_DAC);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8156 (0x%02x) not responding: %s",
                 I2C_ADDR_HP_DAC, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ES8156 DAC present at 0x%02x (reg init TODO)",
             I2C_ADDR_HP_DAC);
    /* TODO: write ES8156 reset/power/format/DAC-enable registers here. */
    return ESP_OK;
}

/* --------------------------------------------------------------- aw881xx -- */
/* aw881xx speaker amps (I2C_ADDR_SPKR_AMP_L / _R).
 *
 * TODO (aw881xx datasheet): the I2C init sequence could not be cracked by the
 * Adafruit effort, so the speaker path is AT RISK. Left + right amps share the
 * I2C bus but have distinct addresses; both need their own register init
 * before the PA_CTRL rail is meaningful. */
static esp_err_t aw881xx_init(void)
{
    const uint8_t addrs[] = { I2C_ADDR_SPKR_AMP_L, I2C_ADDR_SPKR_AMP_R };

    for (size_t i = 0; i < sizeof(addrs); i++) {
        esp_err_t err = probe_i2c_addr(addrs[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "aw881xx amp (0x%02x) not responding: %s",
                     addrs[i], esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "aw881xx amp present at 0x%02x (init TODO)", addrs[i]);
        /* TODO: write aw881xx I2C init sequence here (at-risk). */
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------- I2S -- */

static esp_err_t i2s_init(void)
{
    esp_err_t err;

    /* Master TX channel on I2S_PORT; no RX channel. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT,
                                                            I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 44100 Hz, 16-bit, Philips/I2S, 2 slots (stereo). MCLK = 256 * fs. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S TX enabled (mclk=%d bclk=%d lrclk=%d dout=%d, %d Hz)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT,
             I2S_SAMPLE_RATE_HZ);
    return ESP_OK;
}

/* ------------------------------------------------------------------- API -- */

esp_err_t audio_init(void)
{
    esp_err_t err;

    /* Speaker amp rail enable (PA_CTRL), then report headphone detect. */
    err = iox_set_pin(IOX_AUDIO_PACTRL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iox_set_pin(PA_CTRL) failed: %s", esp_err_to_name(err));
        return err;
    }
    bool hp_detect = iox_get_pin(IOX_AUDIO_HPDETECT);
    ESP_LOGI(TAG, "headphone detect: %s", hp_detect ? "plugged" : "not plugged");

    /* I2S master TX. */
    err = i2s_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Probe the codec + amps (non-fatal; register init is still TODO). */
    es8156_init();
    aw881xx_init();

    return ESP_OK;
}

esp_err_t audio_play(const int16_t *samples, size_t n)
{
    /* TODO: write the PCM buffer to the I2S TX DMA, e.g.
     *   size_t written = 0;
     *   return i2s_channel_write(s_tx_chan, samples, n * sizeof(int16_t),
     *                            &written, portMAX_DELAY);
     * The ES8156/aw881xx register init in the TODOs above must land first for
     * audio to actually leave the codec. */
    (void)samples;
    (void)n;
    (void)s_tx_chan;
    return ESP_ERR_NOT_SUPPORTED;
}
