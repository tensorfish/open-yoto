/*
 * battery.c — CW2215B fuel gauge + SGM41513 charger control.
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
#define BATTERY_CHARGER_RETRY_MS 2000
#define BATTERY_CHARGER_AUDIT_MS 30000

/* ---- SGM41513 charger (register map shared by #04/#05) ---- */
#define SGM41513_REG_INPUT_SOURCE    0x00
#define SGM41513_REG_POWER_ON        0x01
#define SGM41513_REG_CHARGE_CURRENT  0x02
#define SGM41513_REG_CHARGE_VOLTAGE  0x04
#define SGM41513_REG_TIMER_CONTROL   0x05
#define SGM41513_REG_OVP_VINDPM      0x06
#define SGM41513_REG_MISC_CONTROL    0x07
#define SGM41513_REG_STATUS          0x08
#define SGM41513_REG_FAULT           0x09
#define SGM41513_REG_STOCK_CONTROL   0x0C

#define SGM41513_EN_HIZ              0x80
#define SGM41513_STAT_CTRL_MASK      0x60
#define SGM41513_OTG_CONFIG          0x20
#define SGM41513_CHG_CONFIG          0x10
#define SGM41513_Q1_FULLON           0x40
#define SGM41513_ICHG_MASK           0x3F
#define SGM41513_IINDPM_MASK         0x1F
#define SGM41513_IINDPM_BASE_MA      100
#define SGM41513_IINDPM_STEP_MA      100
#define SGM41513_IINDPM_MAX_MA       2400
#define SGM41513_VRECHG              0x01
#define SGM41513_WATCHDOG_MASK       0x30
#define SGM41513_BATFET_DIS          0x20
#define SGM41513_CHRG_STAT_MASK      0x18
#define SGM41513_CHRG_STAT_PRECHARGE 0x08
#define SGM41513_CHRG_STAT_FAST      0x10
#define SGM41513_PG_STAT             0x04

/* Exact stock init requests recovered from FUN_seg4__400e6830 and its
 * SGM41513 (type 3, address 0x1A) helpers. Rev #04 requests 2220 mA
 * (ICHG code 0x36) and OVP=3; rev #05 requests 1020 mA (ICHG code 0x28)
 * and OVP=1. Both request VINDPM code 5 (4.4 V). */
#ifdef CONFIG_BOARD_REV_04
#define SGM41513_ICHG_CODE           0x36
#define SGM41513_OVP_VINDPM_VALUE    0xC5
#else
#define SGM41513_ICHG_CODE           0x28
#define SGM41513_OVP_VINDPM_VALUE    0x45
#endif

/* The rev #05 HUSB238 reports the Type-C/PD contract current. This is the
 * only reliable authority for raising the charger's input-current limit:
 * the SGM41513's source detector can otherwise leave it at USB-default
 * current even when a 1.5 A, 2.4 A, or 3 A Type-C source is attached. */
#ifndef CONFIG_BOARD_REV_04
#define HUSB238_REG_PD_STATUS0       0x00
#define HUSB238_REG_PD_STATUS1       0x01
#define HUSB238_ATTACHED              0x40
#define HUSB238_PD_RESPONSE_MASK      0x38
#define HUSB238_PD_RESPONSE_SUCCESS   0x08
#define HUSB238_5V_CONTRACT           0x04
#define HUSB238_5V_CURRENT_MASK       0x03
#endif

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

/* Stock BATT_THRS_WRNG default recovered from the settings schema. Supported
 * rev #04/#05 boards use the CW2215B SOC reading for this warning. */
#define BATTERY_LOW_SOC_PCT 7


static bool s_gauge_present = false;
static bool s_gauge_data_valid = false;
static bool s_charger_present = false;
static bool s_charger_configured = false;
static bool s_external_power_present = false;
static bool s_charger_retry_pending = false;
static bool s_charger_probe_warned = false;
static TickType_t s_charger_retry_ticks = 0;
static uint8_t s_charger_status = 0;
static TickType_t s_charger_audit_ticks = 0;
static bool s_batfet_fault_warned = false;

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


static esp_err_t sgm41513_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(I2C_PORT, I2C_ADDR_CHARGER,
                                        &reg, 1, val, 1,
                                        pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
}

static esp_err_t sgm41513_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, I2C_ADDR_CHARGER,
                                      data, sizeof(data),
                                      pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS));
}

static esp_err_t sgm41513_update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    esp_err_t err = sgm41513_read_reg(reg, &current);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t updated = (current & (uint8_t)~mask) | (value & mask);
    if (updated == current)
    {
        return ESP_OK;
    }
    return sgm41513_write_reg(reg, updated);
}

static bool sgm41513_status_has_input(uint8_t status)
{
    return (status & SGM41513_PG_STAT) != 0;
}

#ifndef CONFIG_BOARD_REV_04
/* Return the negotiated contract current in mA, capped to the 2.4 A input
 * limit used by the stock rev #05 power-source arbitration. Zero means the
 * charger must retain its own source-detection result. */
static unsigned husb238_input_current_limit_ma(void)
{
    uint8_t status0;
    uint8_t status1;
    unsigned current_ma;

    if (i2c_master_write_read_device(I2C_PORT, I2C_ADDR_PD_SINK,
                                     (uint8_t[]){ HUSB238_REG_PD_STATUS1 }, 1,
                                     &status1, 1,
                                     pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS))
            != ESP_OK
        || (status1 & HUSB238_ATTACHED) == 0)
    {
        return 0;
    }

    if ((status1 & HUSB238_PD_RESPONSE_MASK) == HUSB238_PD_RESPONSE_SUCCESS)
    {
        static const unsigned pd_current_ma[] = {
            500, 700, 1000, 1250, 1500, 1750, 2000, 2250,
            2500, 2750, 3000, 3250, 3500, 4000, 4500, 5000,
        };

        if (i2c_master_write_read_device(
                I2C_PORT, I2C_ADDR_PD_SINK,
                (uint8_t[]){ HUSB238_REG_PD_STATUS0 }, 1, &status0, 1,
                pdMS_TO_TICKS(BATTERY_I2C_TIMEOUT_MS)) != ESP_OK)
        {
            return 0;
        }
        current_ma = pd_current_ma[status0 & 0x0F];
    }
    else if ((status1 & HUSB238_5V_CONTRACT) != 0)
    {
        static const unsigned type_c_current_ma[] = { 500, 1500, 2400, 3000 };
        current_ma = type_c_current_ma[status1 & HUSB238_5V_CURRENT_MASK];
    }
    else
    {
        return 0;
    }

    return current_ma < SGM41513_IINDPM_MAX_MA
        ? current_ma : SGM41513_IINDPM_MAX_MA;
}
#endif

static esp_err_t battery_enable_usb_charge_path(void)
{
#ifdef CONFIG_BOARD_REV_04
    return ESP_OK;
#else
    /* The HUSB238 source-control pin is active-low. Keep Qi disabled so two
     * sources are never connected to the charger at once. */
    esp_err_t err = iox_set_pin(IOX_QI_CHARGE_ENABLE_N, true);
    if (err != ESP_OK)
    {
        return err;
    }
    return iox_set_pin(IOX_USB_CHARGE_ENABLE_N, false);
#endif
}

static esp_err_t sgm41513_configure(bool update_input_current,
                                    bool allow_batfet_enable)
{
    esp_err_t err;

    /* Stock init clears HIZ/STAT mode but preserves REG00 IINDPM on rev #04,
     * leaving the charger's D+/D- source detector authoritative. Forcing
     * 2.4 A from a weak adapter causes input-voltage DPM cycling and can make
     * charging slower, not faster. Rev #05 may override IINDPM only from a
     * valid HUSB238 contract. */
    uint8_t input_mask = SGM41513_EN_HIZ | SGM41513_STAT_CTRL_MASK;
    uint8_t input_value = 0;
#ifdef CONFIG_BOARD_REV_04
    (void)update_input_current;
    unsigned input_current_ma = 0;
#else
    unsigned input_current_ma =
        update_input_current ? husb238_input_current_limit_ma() : 0;
#endif
    if (input_current_ma != 0)
    {
        input_mask |= SGM41513_IINDPM_MASK;
        input_value = (uint8_t)((input_current_ma - SGM41513_IINDPM_BASE_MA)
                                / SGM41513_IINDPM_STEP_MA);
    }

    err = sgm41513_update_reg(SGM41513_REG_INPUT_SOURCE, input_mask,
                              input_value);
    if (err != ESP_OK) return err;

    err = sgm41513_update_reg(SGM41513_REG_POWER_ON,
                              SGM41513_OTG_CONFIG, 0);
    if (err != ESP_OK) return err;

    /* Reproduce the stock board-specific charge-current and input-voltage
     * requests. Q1_FULLON is also enabled by the stock initialization. */
    err = sgm41513_update_reg(
        SGM41513_REG_CHARGE_CURRENT,
        SGM41513_Q1_FULLON | SGM41513_ICHG_MASK,
        SGM41513_Q1_FULLON | SGM41513_ICHG_CODE);
    if (err != ESP_OK) return err;

    err = sgm41513_update_reg(SGM41513_REG_CHARGE_VOLTAGE,
                              SGM41513_VRECHG, SGM41513_VRECHG);
    if (err != ESP_OK) return err;

    /* The replacement does not feed the charger watchdog, so disable it as
     * stock initialization does rather than allowing periodic register reset. */
    err = sgm41513_update_reg(SGM41513_REG_TIMER_CONTROL,
                              SGM41513_WATCHDOG_MASK, 0);
    if (err != ESP_OK) return err;

    err = sgm41513_update_reg(SGM41513_REG_OVP_VINDPM, 0xCF,
                              SGM41513_OVP_VINDPM_VALUE);
    if (err != ESP_OK) return err;
    if (input_current_ma != 0)
    {
        ESP_LOGI(TAG, "SGM41513 USB input current limit set to %u mA",
                 input_current_ma);
    }

    /* BATFET_DIS can also be latched by discharge over-current protection.
     * Clear it only on an explicit fresh-input initialization, never during
     * periodic configuration repair. */
    if (allow_batfet_enable)
    {
        err = sgm41513_update_reg(SGM41513_REG_MISC_CONTROL,
                                  SGM41513_BATFET_DIS, 0);
        if (err != ESP_OK) return err;
    }

#ifdef CONFIG_BOARD_REV_04
    /* The no-PD #04 path applies these two additional stock writes. */
    err = sgm41513_update_reg(SGM41513_REG_TIMER_CONTROL, 0x01, 0);
    if (err != ESP_OK) return err;
    err = sgm41513_write_reg(SGM41513_REG_STOCK_CONTROL, 0x45);
    if (err != ESP_OK) return err;
#endif

    /* Enable charging only after every safety/current register is in place. */
    return sgm41513_update_reg(SGM41513_REG_POWER_ON,
                               SGM41513_CHG_CONFIG,
                               SGM41513_CHG_CONFIG);
}

static esp_err_t sgm41513_audit_reg(uint8_t reg, uint8_t mask,
                                    uint8_t expected, bool *drift)
{
    uint8_t value;
    esp_err_t err = sgm41513_read_reg(reg, &value);

    if (err != ESP_OK)
    {
        return err;
    }
    if ((value & mask) != (expected & mask))
    {
        *drift = true;
    }
    return ESP_OK;
}

/*
 * Detect a charger watchdog/reset without touching autonomous status or
 * source-detected IINDPM. A mismatch triggers the complete owned-register
 * configuration, but BATFET_DIS remains untouched: that bit can be a latched
 * over-current protection response rather than configuration drift.
 */
static esp_err_t sgm41513_audit_configuration(void)
{
    bool drift = false;
    uint8_t misc;
    esp_err_t err;

    err = sgm41513_audit_reg(
        SGM41513_REG_INPUT_SOURCE,
        SGM41513_EN_HIZ | SGM41513_STAT_CTRL_MASK, 0, &drift);
    if (err != ESP_OK) return err;
    err = sgm41513_audit_reg(
        SGM41513_REG_POWER_ON,
        SGM41513_OTG_CONFIG | SGM41513_CHG_CONFIG,
        SGM41513_CHG_CONFIG, &drift);
    if (err != ESP_OK) return err;
    err = sgm41513_audit_reg(
        SGM41513_REG_CHARGE_CURRENT,
        SGM41513_Q1_FULLON | SGM41513_ICHG_MASK,
        SGM41513_Q1_FULLON | SGM41513_ICHG_CODE, &drift);
    if (err != ESP_OK) return err;
    err = sgm41513_audit_reg(
        SGM41513_REG_CHARGE_VOLTAGE,
        SGM41513_VRECHG, SGM41513_VRECHG, &drift);
    if (err != ESP_OK) return err;
#ifdef CONFIG_BOARD_REV_04
    err = sgm41513_audit_reg(
        SGM41513_REG_TIMER_CONTROL,
        SGM41513_WATCHDOG_MASK | 0x01, 0, &drift);
#else
    err = sgm41513_audit_reg(
        SGM41513_REG_TIMER_CONTROL,
        SGM41513_WATCHDOG_MASK, 0, &drift);
#endif
    if (err != ESP_OK) return err;
    err = sgm41513_audit_reg(
        SGM41513_REG_OVP_VINDPM, 0xCF,
        SGM41513_OVP_VINDPM_VALUE, &drift);
    if (err != ESP_OK) return err;
#ifdef CONFIG_BOARD_REV_04
    err = sgm41513_audit_reg(
        SGM41513_REG_STOCK_CONTROL, 0xFF, 0x45, &drift);
    if (err != ESP_OK) return err;
#endif

    err = sgm41513_read_reg(SGM41513_REG_MISC_CONTROL, &misc);
    if (err != ESP_OK) return err;
    if ((misc & SGM41513_BATFET_DIS) != 0)
    {
        if (!s_batfet_fault_warned)
        {
            ESP_LOGE(TAG,
                     "SGM41513 BATFET is latched off; leaving protection asserted");
            s_batfet_fault_warned = true;
        }
    }
    else
    {
        s_batfet_fault_warned = false;
    }

    if (!drift)
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SGM41513 configuration drift detected; restoring settings");
    return sgm41513_configure(false, false);
}

esp_err_t battery_service(void)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t status = 0;
    esp_err_t err = sgm41513_read_reg(SGM41513_REG_STATUS, &status);

    if (err != ESP_OK)
    {
        if (!s_charger_probe_warned)
        {
            ESP_LOGW(TAG, "SGM41513 status read failed: %s",
                     esp_err_to_name(err));
            s_charger_probe_warned = true;
        }
        /* A bus error is unknown, not proof of unplug. Keep the last
         * successfully observed power/configuration state and retry. */
        s_charger_present = false;
        return err;
    }

    s_charger_present = true;
    s_charger_probe_warned = false;
    s_charger_status = status;
    bool external_power = sgm41513_status_has_input(status);

    if (!external_power)
    {
        if (s_external_power_present)
        {
            ESP_LOGI(TAG, "external power removed");
        }
        s_external_power_present = false;
        s_charger_configured = false;
        s_charger_retry_pending = false;
        s_charger_audit_ticks = 0;
        s_batfet_fault_warned = false;
        return ESP_OK;
    }

    if (!s_external_power_present)
    {
        ESP_LOGI(TAG, "external power connected");
        s_external_power_present = true;
        s_charger_retry_pending = false;
    }
    if (s_charger_configured)
    {
        if ((TickType_t)(now - s_charger_audit_ticks)
                < pdMS_TO_TICKS(BATTERY_CHARGER_AUDIT_MS))
        {
            return ESP_OK;
        }
        s_charger_audit_ticks = now;
        err = sgm41513_audit_configuration();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "SGM41513 configuration audit failed: %s",
                     esp_err_to_name(err));
        }
        return err;
    }
    if (s_charger_retry_pending
        && (TickType_t)(now - s_charger_retry_ticks)
               < pdMS_TO_TICKS(BATTERY_CHARGER_RETRY_MS))
    {
        return ESP_OK;
    }
    s_charger_retry_pending = true;
    s_charger_retry_ticks = now;

    err = battery_enable_usb_charge_path();
    if (err != ESP_OK)
    {
        return err;
    }

    err = sgm41513_configure(true, true);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "SGM41513 configuration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_charger_configured = true;
    s_charger_audit_ticks = now;
    uint8_t fault = 0;
    if (sgm41513_read_reg(SGM41513_REG_STATUS, &status) == ESP_OK)
    {
        s_charger_status = status;
    }
    (void)sgm41513_read_reg(SGM41513_REG_FAULT, &fault);
    ESP_LOGI(TAG, "SGM41513 charging enabled (status=0x%02x fault=0x%02x)",
             s_charger_status, fault);
    return ESP_OK;
}

esp_err_t battery_init(void)
{
    esp_err_t init_err = ESP_OK;

    /* The fuel gauge is required for player boot. Reset cached state before
     * probing so a retry after a partial initialization cannot use stale data. */
    s_gauge_present = false;
    s_gauge_data_valid = false;
    s_charger_present = false;
    s_charger_configured = false;
    s_external_power_present = false;
    s_charger_retry_pending = false;
    s_charger_probe_warned = false;
    s_charger_retry_ticks = 0;
    s_charger_status = 0;
    s_charger_audit_ticks = 0;
    s_batfet_fault_warned = false;

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
            init_err = gauge_err == ESP_OK ? ESP_ERR_INVALID_STATE : gauge_err;
        }
    }
    else
    {
        ESP_LOGW(TAG, "CW2215B fuel gauge (0x%02x) not found / chip id mismatch",
                 (unsigned)I2C_ADDR_FUEL_GAUGE);
        init_err = ESP_ERR_NOT_FOUND;
    }

    /* The charger is powered only while external input is present. Configure
     * it now when booting on USB, and let battery_service() handle later
     * unplug/replug events. Gauge availability remains independent. */
    (void)battery_service();

    /* ---- external power, charger, and battery-alert status via IOX ---- */
    bool power_stat = iox_get_pin(IOX_USB_POWER_STAT);
    bool chg_stat = iox_get_pin(IOX_CHG_STAT);
    bool bat_alert = iox_get_pin(IOX_BAT_ALERT);
    ESP_LOGI(TAG, "USB power status (raw) = %s",
             power_stat ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "charger status (raw) = %s",
             chg_stat ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "battery alert (raw) = %s",
             bat_alert ? "HIGH" : "LOW");

    return init_err;
}

esp_err_t battery_get_snapshot(battery_snapshot_t *snapshot)
{
    uint8_t status;
    int soc;
    int voltage_mv;

    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = (battery_snapshot_t){ 0 };
    if (!s_gauge_present || !s_gauge_data_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (sgm41513_read_reg(SGM41513_REG_STATUS, &status) != ESP_OK
        || !cw2215b_read_soc(&soc)
        || !cw2215b_read_vcell_mv(&voltage_mv))
    {
        return ESP_ERR_INVALID_STATE;
    }

    snapshot->external_power_present = sgm41513_status_has_input(status);
    snapshot->soc_percent = soc;
    snapshot->voltage_mv = voltage_mv;
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
    return soc >= 0 && soc <= BATTERY_LOW_SOC_PCT;
}

bool battery_is_charging(void)
{
    uint8_t charge_state = s_charger_status & SGM41513_CHRG_STAT_MASK;

    return s_external_power_present
        && (charge_state == SGM41513_CHRG_STAT_PRECHARGE
            || charge_state == SGM41513_CHRG_STAT_FAST);
}
