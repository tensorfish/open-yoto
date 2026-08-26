/*
 * aw88194.c — Factory-matched Yoto rev #04 AW88194A speaker amplifier.
 *
 * Transport, register table, DSP firmware/configuration blobs, reset timing,
 * and startup ordering are recovered from yoto_firmware_clean.bin. The clean
 * dump's factory app is byte-identical to output/factory.bin.
 */
#include "aw88194.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iox.h"

static const char *TAG = "aw88194";


static bool s_identified = false;
#define AW88194_REG_ID          0x00
#define AW88194_REG_SYSST       0x01
#define AW88194_REG_SYSINT      0x02
#define AW88194_REG_SYSINTM     0x03
#define AW88194_REG_SYSCTRL     0x04
#define AW88194_REG_I2SCTRL     0x05
#define AW88194_REG_PWMCTRL     0x08
#define AW88194_REG_VOLUME      0x0F
#define AW88194_REG_SYSCTRL2    0x11
#define AW88194_REG_DSP_ADDR    0x40
#define AW88194_REG_DSP_DATA    0x41
#define AW88194_REG_DSP_STATUS  0x42
#define AW88194_REG_PLLCTRL1    0x65

#define AW88194_CHIP_ID         0x1806
#define AW88194_I2C_TIMEOUT_MS  100
#define AW88194_PLL_RETRIES     5
#define AW88194_DSP_CHUNK       128
#define AW88194_FW_BASE         0x8C00
#define AW88194_CFG_BASE        0x8600

typedef struct
{
    uint8_t reg;
    uint16_t value;
} aw88194_reg_cfg_t;

/* Factory table @ DRAM 0x3ffbee7a, 35 entries. The rev #04 mono-mix path
 * (channel 2) uses the table unchanged; left/right instances override
 * register 0x0f for their channel after loading it. */
static const aw88194_reg_cfg_t k_factory_regs[] = {
    { 0x03, 0xFFFF }, { 0x05, 0x0C07 }, { 0x06, 0x0310 },
    { 0x09, 0x6773 }, { 0x0A, 0x07ED }, { 0x0B, 0x0F0F },
    { 0x0C, 0x5F3C }, { 0x0D, 0x001B }, { 0x0E, 0x0A2D },
    { 0x0F, 0x1200 }, { 0x10, 0x3A10 }, { 0x11, 0x1BE5 },
    { 0x20, 0x0003 }, { 0x21, 0x2101 }, { 0x38, 0x0000 },
    { 0x40, 0x87CE }, { 0x41, 0x0000 }, { 0x43, 0x0000 },
    { 0x44, 0x0000 }, { 0x47, 0x00A0 }, { 0x60, 0x0A09 },
    { 0x61, 0xC163 }, { 0x62, 0x2878 }, { 0x63, 0x2600 },
    { 0x64, 0x0588 }, { 0x65, 0x7F07 }, { 0x66, 0x0002 },
    { 0x67, 0x0503 }, { 0x68, 0x0318 }, { 0x69, 0x2081 },
    { 0x6A, 0x1016 }, { 0x6B, 0x9CC4 }, { 0x6C, 0x12A2 },
    { 0x04, 0x6440 }, { 0x08, 0x300E },
};

/* Exact stock containers: firmware @ 0x3ffbdf4d (0x7f4 bytes), rev #04
 * channel-2 configuration @ 0x3ffbeade (0x39c bytes). */
extern const uint8_t s_factory_fw_start[]
    asm("_binary_factory_aw88194_fw_bin_start");
extern const uint8_t s_factory_fw_end[]
    asm("_binary_factory_aw88194_fw_bin_end");
extern const uint8_t s_factory_cfg_start[]
    asm("_binary_factory_aw88194_cfg_rev04_bin_start");
extern const uint8_t s_factory_cfg_end[]
    asm("_binary_factory_aw88194_cfg_rev04_bin_end");

static esp_err_t aw88194_write(uint8_t address, uint8_t reg, uint16_t value)
{
    const uint8_t data[] = { reg, (uint8_t)(value >> 8), (uint8_t)value };

    return i2c_master_write_to_device(I2C_PORT, address, data, sizeof(data),
                                      pdMS_TO_TICKS(AW88194_I2C_TIMEOUT_MS));
}

static esp_err_t aw88194_read(uint8_t address, uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = { 0 };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err;

    if (cmd == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,
                          (uint8_t)((address << 1) | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,
                          (uint8_t)((address << 1) | I2C_MASTER_READ), true);
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    err = i2c_master_cmd_begin(I2C_PORT, cmd,
                               pdMS_TO_TICKS(AW88194_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    if (err == ESP_OK)
    {
        *value = (uint16_t)((uint16_t)data[0] << 8) | data[1];
    }
    return err;
}

static esp_err_t aw88194_update_bits(uint8_t address, uint8_t reg,
                                     uint16_t mask, uint16_t value)
{
    uint16_t old_value;
    esp_err_t err = aw88194_read(address, reg, &old_value);

    if (err != ESP_OK)
    {
        return err;
    }
    return aw88194_write(address, reg, (uint16_t)((old_value & mask) | value));
}

static esp_err_t aw88194_load_factory_regs(uint8_t address)
{
    for (size_t i = 0; i < sizeof(k_factory_regs) / sizeof(k_factory_regs[0]); i++)
    {
        esp_err_t err = aw88194_write(address, k_factory_regs[i].reg,
                                      k_factory_regs[i].value);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t aw88194_upload_dsp(uint8_t address, uint16_t base,
                                    const uint8_t *data, size_t length)
{
    uint8_t tx[AW88194_DSP_CHUNK + 1];
    TickType_t started = xTaskGetTickCount();
    esp_err_t err = aw88194_write(address, AW88194_REG_DSP_ADDR, base);

    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    for (size_t offset = 0; offset < length; )
    {
        size_t chunk = length - offset;

        if (chunk > AW88194_DSP_CHUNK)
        {
            chunk = AW88194_DSP_CHUNK;
        }
        tx[0] = AW88194_REG_DSP_DATA;
        for (size_t i = 0; i < chunk; i++)
        {
            tx[i + 1] = data[(offset + i) ^ 1U];
        }
        err = i2c_master_write_to_device(I2C_PORT, address, tx, chunk + 1,
                                         pdMS_TO_TICKS(AW88194_I2C_TIMEOUT_MS));
        if (err != ESP_OK)
        {
            return err;
        }
        offset += chunk;
        if (xTaskGetTickCount() - started > pdMS_TO_TICKS(999))
        {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t aw88194_iis_check(uint8_t address)
{
    for (int attempt = 0; attempt < AW88194_PLL_RETRIES; attempt++)
    {
        uint16_t status;
        esp_err_t err = aw88194_read(address, AW88194_REG_SYSST, &status);

        if (err != ESP_OK)
        {
            return err;
        }
        if ((status & 0x0001) != 0)
        {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t aw88194_syspll_check(uint8_t address)
{
    esp_err_t err = aw88194_iis_check(address);
    uint16_t pllctrl;

    if (err == ESP_OK)
    {
        return ESP_OK;
    }
    err = aw88194_read(address, AW88194_REG_PLLCTRL1, &pllctrl);
    if (err != ESP_OK || (pllctrl & 0x4000) == 0)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    err = aw88194_update_bits(address, AW88194_REG_PLLCTRL1, 0xBFFF, 0x0000);
    if (err == ESP_OK)
    {
        (void)aw88194_iis_check(address);
        err = aw88194_update_bits(address, AW88194_REG_PLLCTRL1,
                                  0xBFFF, 0x4000);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    return aw88194_iis_check(address);
}

static esp_err_t aw88194_program_vcalb(uint8_t address)
{
    uint16_t raw_ical;
    uint16_t raw_vcal;
    esp_err_t err = aw88194_read(address, 0x78, &raw_ical);

    if (err == ESP_OK)
    {
        err = aw88194_read(address, 0x79, &raw_vcal);
    }
    if (err != ESP_OK)
    {
        return err;
    }

    int32_t cal0 = (int8_t)(raw_ical & 0xFF);
    int32_t cal1 = (int32_t)((raw_vcal >> 10) & 0x3F);
    if ((cal1 & 0x20) != 0)
    {
        cal1 -= 0x40;
    }
    int32_t icalk = cal0 + 500;
    int32_t vcalk = cal1 * 4 + 1000;
    if (icalk == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t vcalb = (uint16_t)((vcalk * 0x3A37) / (2 * icalk));
    err = aw88194_write(address, AW88194_REG_DSP_ADDR, 0x866D);
    if (err == ESP_OK)
    {
        err = aw88194_write(address, AW88194_REG_DSP_DATA, vcalb);
    }
    return err;
}

static esp_err_t aw88194_factory_cold_start(uint8_t address)
{
    const size_t fw_length = (size_t)(s_factory_fw_end - s_factory_fw_start);
    const size_t cfg_length = (size_t)(s_factory_cfg_end - s_factory_cfg_start);
    uint16_t status;
    esp_err_t err;

    if (fw_length != 0x7F4 || cfg_length != 0x39C)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Outer split init: run_pwd(false), 2ms, mode/volume/hw-params, then
     * smartpa_cfg -> cold_start. The factory register table contains the
     * rev04 mono mode and 44.1kHz/16-bit hardware parameters. */
    err = aw88194_update_bits(address, AW88194_REG_SYSCTRL, 0xFFFE, 0x0000);
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = aw88194_load_factory_regs(address);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "factory register table loaded");
        }
    }
    if (err == ESP_OK)
    {
        err = aw88194_syspll_check(address);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_PWMCTRL,
                                  0xFFFE, 0x0001);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL,
                                  0xFFFB, 0x0004);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL2,
                                  0xEFFF, 0x1000);
    }
    if (err == ESP_OK)
    {
        err = aw88194_upload_dsp(address, AW88194_FW_BASE,
                                 s_factory_fw_start, fw_length);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "factory DSP firmware uploaded (%u bytes)",
                     (unsigned)fw_length);
        }
    }
    if (err == ESP_OK)
    {
        err = aw88194_upload_dsp(address, AW88194_CFG_BASE,
                                 s_factory_cfg_start, cfg_length);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "factory rev04 DSP config uploaded (%u bytes)",
                     (unsigned)cfg_length);
        }
    }
    if (err == ESP_OK)
    {
        err = aw88194_program_vcalb(address);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "factory VCALB programmed");
        }
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL,
                                  0xFFFB, 0x0000);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL,
                                  0xFFFD, 0x0000);
    }
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = aw88194_read(address, AW88194_REG_DSP_STATUS, &status);
        if (err == ESP_OK && status == 0)
        {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "factory DSP status 0x%04x", status);
        }
    }
    if (err == ESP_OK)
    {
        /* Read-to-clear status/interrupt registers, then unmask the factory
         * interrupt set with old & 0xfff4. */
        err = aw88194_read(address, AW88194_REG_SYSST, &status);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_SYSINT, &status);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_SYSINTM, &status);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSINTM,
                                  0xFFF4, 0x0000);
    }
    if (err == ESP_OK)
    {
        /* Factory smartpa_cfg reapplies hardware parameters and DSP enable,
         * then start clears PWD and validates IIS/PLL before outer unmute. */
        err = aw88194_write(address, AW88194_REG_I2SCTRL, 0x0C07);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL,
                                  0xFFFB, 0x0000);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_SYSCTRL,
                                  0xFFFE, 0x0000);
    }
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = aw88194_syspll_check(address);
    }
    if (err == ESP_OK)
    {
        err = aw88194_update_bits(address, AW88194_REG_PWMCTRL,
                                  0xFFFE, 0x0000);
    }
    return err;
}

static esp_err_t aw88194_validate_ready(uint8_t address)
{
    uint16_t sysst;
    uint16_t sysctrl;
    uint16_t i2sctrl;
    uint16_t pwmctrl;
    uint16_t volume;
    uint16_t dsp_status;
    esp_err_t err = aw88194_read(address, AW88194_REG_SYSST, &sysst);

    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_SYSCTRL, &sysctrl);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_I2SCTRL, &i2sctrl);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_PWMCTRL, &pwmctrl);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_VOLUME, &volume);
    }
    if (err == ESP_OK)
    {
        err = aw88194_read(address, AW88194_REG_DSP_STATUS, &dsp_status);
    }
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG,
             "ready state: SYSST=0x%04x SYSCTRL=0x%04x I2SCTRL=0x%04x "
             "PWMCTRL=0x%04x VOL=0x%04x DSP=0x%04x",
             sysst, sysctrl, i2sctrl, pwmctrl, volume, dsp_status);
    if ((sysst & 0x0001) == 0
        || (sysctrl & 0x0007) != 0
        || i2sctrl != 0x0C07
        || (pwmctrl & 0x0001) != 0
        || volume != 0
        || dsp_status == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t aw88194_identify(uint8_t address)
{
    uint16_t chip_id = 0;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < 5; attempt++)
    {
        err = aw88194_read(address, AW88194_REG_ID, &chip_id);
        if (err == ESP_OK && chip_id == AW88194_CHIP_ID)
        {
            ESP_LOGI(TAG, "0x%02x identified (ID=0x%04x)",
                     address, chip_id);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGE(TAG, "0x%02x ID failed after 5 attempts (%s, ID=0x%04x)",
             address, esp_err_to_name(err), chip_id);
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}


#ifdef CONFIG_APP_SPEAKER_TEST_TONE
static void aw88194_log_rails(const char *phase)
{
    uint8_t in0;
    uint8_t in1;
    uint8_t out0;
    uint8_t out1;

    if (iox_read_port(0, 0, &in0) != ESP_OK
        || iox_read_port(0, 1, &in1) != ESP_OK
        || iox_read_output_port(0, 0, &out0) != ESP_OK
        || iox_read_output_port(0, 1, &out1) != ESP_OK)
    {
        ESP_LOGW(TAG, "%s ET6416 state read failed", phase);
        return;
    }
    ESP_LOGI(TAG,
             "%s ET6416: IN0=0x%02x IN1=0x%02x OUT0=0x%02x OUT1=0x%02x "
             "LEVEL=%s HPDETECT=%s",
             phase, in0, in1, out0, out1,
             iox_get_pin(IOX_POWER_LEVELCONVERTOR) ? "HIGH" : "LOW",
             iox_get_pin(IOX_AUDIO_HPDETECT) ? "HIGH" : "LOW");
}
#endif

esp_err_t aw88194_init(void)
{
    esp_err_t err;

    /* Factory aw881xx_hw_reset @ 0x400deea0 only toggles pactrl:
     * LOW -> 2 ms -> HIGH -> 2 ms. Board-wide VINHOLD/PWREN/level-convertor
     * transitions are completed by iox_init() before any peripheral starts. */
    err = iox_set_pin(IOX_AUDIO_PACTRL, false);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    err = iox_set_pin(IOX_AUDIO_PACTRL, true);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

#ifdef CONFIG_APP_SPEAKER_TEST_TONE
    aw88194_log_rails("post-reset");
#endif

    err = aw88194_identify(I2C_ADDR_SPKR_AMP_L);
    s_identified = (err == ESP_OK);
    return err;
}

esp_err_t aw88194_start(void)
{
    esp_err_t err;

    if (!s_identified)
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = aw88194_factory_cold_start(I2C_ADDR_SPKR_AMP_L);
    if (err == ESP_OK)
    {
        /* Factory combo open @ 0x400dedc2 applies speaker volume 100 after
         * SmartPA startup. aw881xx_set_volume maps 100% to register 0x0f=0. */
        err = aw88194_write(I2C_ADDR_SPKR_AMP_L, AW88194_REG_VOLUME, 0x0000);
    }
    if (err == ESP_OK)
    {
        err = aw88194_validate_ready(I2C_ADDR_SPKR_AMP_L);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "0x%02x factory SmartPA start failed: %s",
                 I2C_ADDR_SPKR_AMP_L, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "0x%02x factory SmartPA ready", I2C_ADDR_SPKR_AMP_L);
    return ESP_OK;
}

esp_err_t aw88194_set_muted(bool muted)
{
    esp_err_t err;

    if (!s_identified)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* SYSCTRL.0 is the factory run_pwd control: clear = run, set = power down
     * the speaker output without changing the I2S stream for headphones. */
    err = aw88194_update_bits(I2C_ADDR_SPKR_AMP_L, AW88194_REG_SYSCTRL,
                              0xFFFE, muted ? 0x0001 : 0x0000);
    if (err == ESP_OK && !muted)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return err;
}
