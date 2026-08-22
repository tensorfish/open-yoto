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
#define CW2215B_REG_ID            0x00
#define CW2215B_REG_VCELL         0x02
#define CW2215B_REG_SOC           0x04
#define CW2215B_REG_MODE_CONFIG   0x08
#define CW2215B_REG_GPIO_CONFIG   0x0A
#define CW2215B_REG_SOC_ALERT     0x0B
#define CW2215B_REG_BAT_PROFILE   0x10
#define CW2215B_REG_IC_STATE      0xA7
#define CW2215B_CHIP_ID           0xA0

#define CW2215B_MODE_RESTART      0x30
#define CW2215B_MODE_ACTIVE       0x00
#define CW2215B_MODE_SLEEP        0xF0
#define CW2215B_UPDATE_FLAG       0x80
#define CW2215B_IC_READY_MASK     0x0C
#define CW2215B_PROFILE_SIZE      80
#define CW2215B_READY_ATTEMPTS    50

#define CW2215B_VCELL_MASK        0x3FFFu
#define CW2215B_VCELL_LSB_MV      0.3125f
#define CW2215B_SOC_MAX           100

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

static esp_err_t cw2215b_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, I2C_ADDR_FUEL_GAUGE,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
}

/* Exact 80-byte profiles embedded in the stock image. hwconfig_04 selects
 * UTL-FD70X-2000; hwconfig_05 selects LJDX30X-4500. */
#ifdef CONFIG_BOARD_REV_04
static const uint8_t CW2215B_PROFILE[CW2215B_PROFILE_SIZE] = {
    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB1, 0xB0, 0xBE, 0xB7, 0xB6, 0xAB, 0xCC, 0xB2,
    0xA2, 0xFF, 0xFF, 0xD8, 0x9E, 0x6D, 0x59, 0x4C,
    0x43, 0x3E, 0x35, 0x93, 0x77, 0xD2, 0x2D, 0xD7,
    0xC3, 0xC1, 0x92, 0xE1, 0xA7, 0x86, 0xA4, 0xA6,
    0xA3, 0xA4, 0x97, 0x81, 0x6E, 0x64, 0x58, 0x4F,
    0x47, 0x5B, 0x71, 0x8F, 0xA6, 0x76, 0x4E, 0x53,
    0x20, 0x00, 0xAB, 0x10, 0x00, 0xA1, 0x8D, 0x00,
    0x00, 0x00, 0x64, 0x15, 0xB0, 0xA8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26,
};
#else
static const uint8_t CW2215B_PROFILE[CW2215B_PROFILE_SIZE] = {
    0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC1, 0xBD, 0xB1, 0xA5, 0x99, 0x94, 0xFA, 0xFA,
    0xFB, 0xBA, 0xA2, 0x8A, 0x67, 0x59, 0x4D, 0x45,
    0x42, 0x41, 0x3E, 0xA1, 0xAB, 0xD1, 0xA0, 0xF7,
    0xA7, 0x8C, 0xBA, 0xCE, 0xCB, 0xC3, 0xB9, 0xC0,
    0xC8, 0xD0, 0xD5, 0xC7, 0xAD, 0x9A, 0x92, 0x92,
    0x95, 0xA1, 0xBD, 0xCB, 0xD7, 0xD0, 0xB2, 0x53,
    0x20, 0x00, 0xAB, 0x10, 0x00, 0xB0, 0xCE, 0x00,
    0x00, 0x00, 0x64, 0x3B, 0xC0, 0x07, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD5,
};
#endif

static esp_err_t cw2215b_set_active(void)
{
    esp_err_t err = cw2215b_write_reg(CW2215B_REG_MODE_CONFIG,
                                      CW2215B_MODE_RESTART);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    err = cw2215b_write_reg(CW2215B_REG_MODE_CONFIG, CW2215B_MODE_ACTIVE);
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

static esp_err_t cw2215b_set_sleep(void)
{
    esp_err_t err = cw2215b_write_reg(CW2215B_REG_MODE_CONFIG,
                                      CW2215B_MODE_RESTART);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    err = cw2215b_write_reg(CW2215B_REG_MODE_CONFIG, CW2215B_MODE_SLEEP);
    if (err == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

static esp_err_t cw2215b_wait_ready(void)
{
    uint8_t state;

    for (int attempt = 0; attempt < CW2215B_READY_ATTEMPTS; attempt++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t err = cw2215b_read_reg(CW2215B_REG_IC_STATE, &state);
        if (err != ESP_OK)
        {
            return err;
        }
        if ((state & CW2215B_IC_READY_MASK) == CW2215B_IC_READY_MASK)
        {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cw2215b_profile_matches(bool *matches)
{
    *matches = true;
    for (size_t i = 0; i < CW2215B_PROFILE_SIZE; i++)
    {
        uint8_t value;
        esp_err_t err = cw2215b_read_reg(
            (uint8_t)(CW2215B_REG_BAT_PROFILE + i), &value);
        if (err != ESP_OK)
        {
            return err;
        }
        if (value != CW2215B_PROFILE[i])
        {
            *matches = false;
            return ESP_OK;
        }
    }
    return ESP_OK;
}

static esp_err_t cw2215b_write_profile(void)
{
    for (size_t i = 0; i < CW2215B_PROFILE_SIZE; i++)
    {
        esp_err_t err = cw2215b_write_reg(
            (uint8_t)(CW2215B_REG_BAT_PROFILE + i), CW2215B_PROFILE[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t cw2215b_ensure_ready(void)
{
    uint8_t mode;
    uint8_t alert;
    bool profile_matches = false;
    esp_err_t err = cw2215b_read_reg(CW2215B_REG_MODE_CONFIG, &mode);
    if (err != ESP_OK)
    {
        return err;
    }
    err = cw2215b_read_reg(CW2215B_REG_SOC_ALERT, &alert);
    if (err != ESP_OK)
    {
        return err;
    }

    if ((alert & CW2215B_UPDATE_FLAG) != 0)
    {
        err = cw2215b_profile_matches(&profile_matches);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (mode == CW2215B_MODE_ACTIVE && profile_matches)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "CW2215B startup: mode=0x%02x update=%d profile=%s",
             mode, (alert & CW2215B_UPDATE_FLAG) != 0,
             profile_matches ? "match" : "update required");

    if (!profile_matches)
    {
        err = cw2215b_set_sleep();
        if (err != ESP_OK)
        {
            return err;
        }
        err = cw2215b_write_profile();
        if (err != ESP_OK)
        {
            return err;
        }
        err = cw2215b_write_reg(CW2215B_REG_GPIO_CONFIG, 0x00);
        if (err != ESP_OK)
        {
            return err;
        }
        err = cw2215b_write_reg(CW2215B_REG_SOC_ALERT,
                                CW2215B_UPDATE_FLAG);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = cw2215b_set_active();
    if (err != ESP_OK)
    {
        return err;
    }
    return cw2215b_wait_ready();
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
    /* CW2215B fuel gauge over I2C: detect, load the board-specific stock
     * profile when needed, activate the IC, then read VCELL and SOC. */
    s_gauge_present = cw2215b_detect();
    if (s_gauge_present)
    {
        int vcell_mv = 0;
        int soc = 0;
        bool vcell_ok = false;
        bool soc_ok = false;
        esp_err_t gauge_err;

        ESP_LOGI(TAG, "CW2215B fuel gauge (0x%02x): chip id 0x%02x OK",
                 (unsigned)I2C_ADDR_FUEL_GAUGE, (unsigned)CW2215B_CHIP_ID);

        gauge_err = cw2215b_ensure_ready();
        if (gauge_err == ESP_OK)
        {
            vcell_ok = cw2215b_read_vcell_mv(&vcell_mv);
            soc_ok = cw2215b_read_soc(&soc);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B initialization failed: %s",
                     esp_err_to_name(gauge_err));
        }

        if (vcell_ok)
        {
            ESP_LOGI(TAG, "CW2215B VCELL = %d mV", vcell_mv);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B VCELL unavailable");
        }
        if (soc_ok)
        {
            ESP_LOGI(TAG, "CW2215B SOC = %d %%", soc);
        }
        else
        {
            ESP_LOGW(TAG, "CW2215B SOC unavailable");
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
    if (!s_gauge_present || !s_gauge_data_valid)
    {
        return false;
    }

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
