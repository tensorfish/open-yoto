/*
 * battery.c — CW2215B fuel gauge + SGM41513 charger + ADC monitoring.
 *
 * The shared I2C bus is installed by iox_init() (called first from app_main),
 * so this component only reads its own devices on that bus. Battery voltage
 * is sampled on ADC1 channel 3 (VBAT sense) through the oneshot driver, and
 * the CW2215B fuel gauge is read over I2C for a more accurate VCELL / SOC.
 */
#include "battery.h"
#include "board_pins.h"
#include "iox.h"

#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

/* ---- ADC1 (VBAT / light / IR-temp) ---- */
/* 12-bit @ 12 dB attenuation. The ADC full-scale reference is ~3.3 V, but the
 * ESP32 SAR ADC saturates near ~3.1 V; treat the result as an approximate pin
 * voltage until the external divider is characterized. */
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH    ADC_BITWIDTH_12
#define BATTERY_ADC_MAX_RAW     4095
#define BATTERY_ADC_REF_MV      3300

#define BATTERY_I2C_TIMEOUT_MS  100

/* ---- CW2215B fuel gauge (CellWise CW2215B datasheet) ---- */
#define CW2215B_REG_ID          0x00    /* chip ID, reads 0xA0               */
#define CW2215B_REG_VCELL       0x02    /* cell voltage, big-endian 16-bit   */
#define CW2215B_REG_SOC         0x04    /* state of charge, 0..100 %         */
#define CW2215B_CHIP_ID         0xA0

/* TODO: confirm the VCELL reserved-bit position and LSB scale from the
 * CW2215B datasheet. The gauge reports a 14-bit field; the top two bits of
 * the 16-bit read are reserved and masked off. The 0.3125 mV/LSB scale below
 * gives a ~5.12 V full scale over 14 bits (4.2 V at 0x3480). */
#define CW2215B_VCELL_MASK      0x3FFFu
#define CW2215B_VCELL_LSB_MV    0.3125f
#define CW2215B_SOC_MAX         100

/* ---- low-battery policy ---- */
#define BATTERY_LOW_SOC_PCT     15
/* TODO: confirm the low-voltage cutoff from the battery discharge curve /
 * schematic. 3300 mV is a conservative single-cell "low" threshold. */
#define BATTERY_LOW_VOLTAGE_MV  3300

static adc_oneshot_unit_handle_t s_adc1_handle;
static bool s_gauge_present = false;

/* Read a single 8-bit register from the CW2215B over the shared I2C bus. */
static esp_err_t cw2215b_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(I2C_PORT, I2C_ADDR_FUEL_GAUGE,
                                        &reg, 1, val, 1,
                                        pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
}

/* Read a big-endian 16-bit register pair (reg, reg + 1). */
static esp_err_t cw2215b_read_reg16(uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, I2C_ADDR_FUEL_GAUGE,
                                                 &reg, 1, buf, sizeof(buf),
                                                 pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
    if (err == ESP_OK)
    {
        *val = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    }
    return err;
}

/* Read the chip ID (0x00) and confirm it matches the expected value. */
static bool cw2215b_detect(void)
{
    uint8_t chip_id = 0;
    if (cw2215b_read_reg(CW2215B_REG_ID, &chip_id) != ESP_OK)
    {
        return false;
    }
    return (chip_id == CW2215B_CHIP_ID);
}

/* Read VCELL (0x02..0x03) and decode the 14-bit cell voltage into millivolts. */
static bool cw2215b_read_vcell_mv(int *mv)
{
    uint16_t raw = 0;
    if (cw2215b_read_reg16(CW2215B_REG_VCELL, &raw) != ESP_OK)
    {
        return false;
    }
    uint16_t vcell = raw & CW2215B_VCELL_MASK;
    *mv = (int)(((float)vcell * CW2215B_VCELL_LSB_MV) + 0.5f);
    return true;
}

/* Read SOC (0x04). Rejects out-of-range values (0xFF = not-yet-valid). */
static bool cw2215b_read_soc(int *soc)
{
    uint8_t soc8 = 0;
    if (cw2215b_read_reg(CW2215B_REG_SOC, &soc8) != ESP_OK)
    {
        return false;
    }
    if (soc8 > CW2215B_SOC_MAX)
    {
        return false;
    }
    *soc = (int)soc8;
    return true;
}

static int adc_read_mv(adc_channel_t channel)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc1_handle, channel, &raw) != ESP_OK)
    {
        return -1;
    }
    return (raw * BATTERY_ADC_REF_MV) / BATTERY_ADC_MAX_RAW;
}

/* Probe a device by writing its register pointer (device ACKs the address). */
static bool i2c_device_probe(uint8_t addr)
{
    uint8_t reg = 0x00;
    esp_err_t err = i2c_master_write_to_device(I2C_PORT, addr, &reg, 1,
                                               pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
    return (err == ESP_OK);
}

esp_err_t battery_init(void)
{
    esp_err_t err;

    /* ---- ADC1 oneshot: VBAT + light + IR-temp ---- */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc1_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc1_handle, ADC_CH_BAT, &chan_cfg);
    if (err != ESP_OK) return err;
    err = adc_oneshot_config_channel(s_adc1_handle, ADC_CH_LIGHT, &chan_cfg);
    if (err != ESP_OK) return err;
    err = adc_oneshot_config_channel(s_adc1_handle, ADC_CH_IR_TEMP, &chan_cfg);
    if (err != ESP_OK) return err;

    int vbat_mv = adc_read_mv(ADC_CH_BAT);
    if (vbat_mv >= 0)
    {
        ESP_LOGI(TAG, "VBAT ADC (ch%d, GPIO%d) = %d mV",
                 ADC_CH_BAT, (int)PIN_ADC_BAT_VBAT, vbat_mv);
        /* TODO: true battery voltage = this reading * VBAT divider ratio; the
         * ADC sees the divided sense node, ratio not yet recovered. */
    }
    else
    {
        ESP_LOGW(TAG, "VBAT ADC read failed");
    }

    /* ---- CW2215B fuel gauge over I2C: chip ID + VCELL + SOC ---- */
    s_gauge_present = cw2215b_detect();
    if (s_gauge_present)
    {
        int vcell_mv = 0;
        int soc = 0;
        ESP_LOGI(TAG, "CW2215B fuel gauge (0x%02x): chip id 0x%02x OK",
                 (unsigned)I2C_ADDR_FUEL_GAUGE, (unsigned)CW2215B_CHIP_ID);
        if (cw2215b_read_vcell_mv(&vcell_mv))
        {
            ESP_LOGI(TAG, "CW2215B VCELL = %d mV", vcell_mv);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B VCELL read failed");
        }
        if (cw2215b_read_soc(&soc))
        {
            ESP_LOGI(TAG, "CW2215B SOC = %d %%", soc);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B SOC read failed");
        }
    }
    else
    {
        ESP_LOGW(TAG, "CW2215B fuel gauge (0x%02x) not found / chip id mismatch",
                 (unsigned)I2C_ADDR_FUEL_GAUGE);
    }

    /* ---- SGM41513 charger probe ---- */
    bool charger = i2c_device_probe(I2C_ADDR_CHARGER);
    ESP_LOGI(TAG, "SGM41513 charger (0x%02x): %s",
             (unsigned)I2C_ADDR_CHARGER, charger ? "found" : "NOT found");

    /* ---- charger + battery-alert status via IO expander ---- */
    bool chg_stat = iox_get_pin(IOX_CHG_STAT);
    bool bat_alert = iox_get_pin(IOX_BAT_ALERT);
    /* TODO: confirm active-level polarity for both pins from the schematic. */
    ESP_LOGI(TAG, "charger status (IOX_CHG_STAT) = %s", chg_stat ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "battery alert (IOX_BAT_ALERT) = %s", bat_alert ? "HIGH" : "LOW");

    return ESP_OK;
}

float battery_voltage(void)
{
    int mv = 0;
    if (s_gauge_present && cw2215b_read_vcell_mv(&mv))
    {
        return (float)mv;
    }

    /* Fall back to the ADC1 VBAT sense channel (undivided). */
    if (s_adc1_handle == NULL)
    {
        return 0.0f;
    }
    int adc_mv = adc_read_mv(ADC_CH_BAT);
    return (adc_mv >= 0) ? (float)adc_mv : 0.0f;
}

int battery_soc(void)
{
    int soc = 0;
    if (s_gauge_present && cw2215b_read_soc(&soc))
    {
        return soc;
    }
    return -1;
}

bool battery_is_low(void)
{
    int soc = battery_soc();
    if (soc >= 0 && soc < BATTERY_LOW_SOC_PCT)
    {
        return true;
    }
    return (battery_voltage() < (float)BATTERY_LOW_VOLTAGE_MV);
}
