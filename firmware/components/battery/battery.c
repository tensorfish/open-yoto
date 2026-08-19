/*
 * battery.c — CW2215B fuel gauge + SGM41513 charger + ADC monitoring.
 *
 * The shared I2C bus is installed by iox_init() (called first from app_main),
 * so this component only probes its own devices on that bus. Battery voltage
 * is sampled on ADC1 channel 3 (VBAT sense) through the oneshot driver.
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

/* ADC1 VBAT sense: 12-bit @ 12 dB attenuation. The ADC full-scale reference is
 * ~3.3 V, but the ESP32 SAR ADC saturates near ~3.1 V; treat the result as an
 * approximate pin voltage until the external divider is characterized. */
#define BATTERY_ADC_ATTEN     ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH  ADC_BITWIDTH_12
#define BATTERY_ADC_MAX_RAW   4095
#define BATTERY_ADC_REF_MV    3300

#define BATTERY_I2C_TIMEOUT_MS 100

/*
 * Register maps (not yet read — see TODO notes below):
 *
 * CW2215B fuel gauge (CellWise CW2215B datasheet):
 *   0x00        REG_ID    chip ID, reads 0xA0
 *   0x02..0x03  VCELL     14-bit cell voltage (high, low)
 *   0x04        SOC       state of charge, 0..100 %
 *   (VCELL LSB/unit is per the datasheet — verify before decoding.)
 *
 * SGM41513 charger (SGMicro SGM41513 datasheet):
 *   charge status / enable registers — verify addresses from the datasheet.
 */
static adc_oneshot_unit_handle_t s_adc1_handle;

static int adc_read_mv(adc_channel_t channel)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc1_handle, channel, &raw) != ESP_OK) {
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
    if (err != ESP_OK) {
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
    if (vbat_mv >= 0) {
        ESP_LOGI(TAG, "VBAT ADC (ch%d, GPIO%d) = %d mV",
                 ADC_CH_BAT, (int)PIN_ADC_BAT_VBAT, vbat_mv);
        /* TODO: true battery voltage = this reading * VBAT divider ratio; the
         * ADC sees the divided sense node, ratio not yet recovered. */
    } else {
        ESP_LOGW(TAG, "VBAT ADC read failed");
    }

    /* ---- I2C probe: CW2215B fuel gauge + SGM41513 charger ---- */
    bool gauge = i2c_device_probe(I2C_ADDR_FUEL_GAUGE);
    bool charger = i2c_device_probe(I2C_ADDR_CHARGER);
    ESP_LOGI(TAG, "CW2215B fuel gauge (0x%02x): %s",
             (unsigned)I2C_ADDR_FUEL_GAUGE, gauge ? "found" : "NOT found");
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
    /* TODO: read CW2215B VCELL (0x02..0x03, 14-bit) once the fuel-gauge
     * register protocol is implemented; decode per the datasheet LSB. Until
     * then return the ADC VBAT sense reading (undivided). */
    if (s_adc1_handle == NULL) {
        return 0.0f;
    }
    int mv = adc_read_mv(ADC_CH_BAT);
    return (mv >= 0) ? (float)mv : 0.0f;
}

int battery_soc(void)
{
    /* TODO: read CW2215B SOC (0x04) once the fuel-gauge register protocol is
     * implemented; returns 0..100 %. */
    return -1;
}
