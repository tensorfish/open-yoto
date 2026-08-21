/*
 * battery.c — CW2215B fuel gauge + SGM41513 charger monitoring.
 *
 * The rev #04/#05 stock configurations use the CW2215B on I2C and define no
 * ADC battery pin. GPIO39 belongs to encoder 0 on rev #04, so sampling the
 * unrelated ADC1 channel there would disturb a real board input.
 */
#include "battery.h"
#include "board_pins.h"
#include "iox.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";


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

/* Charger STAT line: open-drain, active-low while charging. TODO: confirm
 * polarity against the schematic. */
#define BATTERY_CHG_STAT_ACTIVE_LOW 1

static bool s_gauge_present = false;
static bool s_gauge_data_valid = false;
static bool s_charger_present = false;

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
    /* CW2215B fuel gauge over I2C: chip ID + VCELL + SOC. */
    s_gauge_present = cw2215b_detect();
    if (s_gauge_present)
    {
        int vcell_mv = 0;
        int soc = 0;
        bool vcell_ok;
        bool soc_ok;

        ESP_LOGI(TAG, "CW2215B fuel gauge (0x%02x): chip id 0x%02x OK",
                 (unsigned)I2C_ADDR_FUEL_GAUGE, (unsigned)CW2215B_CHIP_ID);
        vcell_ok = cw2215b_read_vcell_mv(&vcell_mv);
        soc_ok = cw2215b_read_soc(&soc);
        if (vcell_ok)
        {
            ESP_LOGI(TAG, "CW2215B VCELL = %d mV", vcell_mv);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B VCELL read failed");
        }
        if (soc_ok)
        {
            ESP_LOGI(TAG, "CW2215B SOC = %d %%", soc);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B SOC read failed");
        }

        s_gauge_data_valid = vcell_ok && vcell_mv > 0 && soc_ok;
        if (!s_gauge_data_valid)
        {
            ESP_LOGW(TAG, "CW2215B data inactive; voltage and SOC unavailable");
        }
    }
    else
    {
        ESP_LOGW(TAG, "CW2215B fuel gauge (0x%02x) not found / chip id mismatch",
                 (unsigned)I2C_ADDR_FUEL_GAUGE);
    }

    s_charger_present = i2c_device_probe(I2C_ADDR_CHARGER);
    ESP_LOGI(TAG, "SGM41513 charger (0x%02x): %s",
             (unsigned)I2C_ADDR_CHARGER,
             s_charger_present ? "found" : "NOT found");

    /* ---- charger + battery-alert status via IO expander ---- */
    bool plug_stat = iox_get_pin(IOX_PLUG_STAT);
    bool chg_stat = iox_get_pin(IOX_CHG_STAT);
    bool bat_alert = iox_get_pin(IOX_BAT_ALERT);
    /* Levels are logged raw until their schematic polarity is verified. */
    ESP_LOGI(TAG, "USB plug status (IOX_PLUG_STAT) = %s",
             plug_stat ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "charger status (IOX_CHG_STAT) = %s",
             chg_stat ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "battery alert (IOX_BAT_ALERT) = %s",
             bat_alert ? "HIGH" : "LOW");

    return ESP_OK;
}

float battery_voltage(void)
{
    int mv = 0;

    if (s_gauge_present && s_gauge_data_valid && cw2215b_read_vcell_mv(&mv))
    {
        return (float)mv;
    }

    return 0.0f;
}

int battery_soc(void)
{
    int soc = 0;

    if (s_gauge_present && s_gauge_data_valid && cw2215b_read_soc(&soc))
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

bool battery_is_charging(void)
{
    /*
     * The SGM41513 I2C peripheral is absent on this unit, but USB connected
     * and the charge-status line both read low. SGM's CHG output is open-drain
     * active-low, so this hardware status is the authoritative charging signal.
     */
    bool level = iox_get_pin(IOX_CHG_STAT);

#if BATTERY_CHG_STAT_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
}
