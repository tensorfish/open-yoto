/*
 * iox.c — PI4IOE5V6416 IO expander driver (2 chips on one I2C bus).
 */
#include "iox.h"
#include "board_pins.h"

#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "iox";

/* PI4IOE5V6416 register map (per 8-bit port) */
#define IOX_REG_INPUT_0      0x00
#define IOX_REG_INPUT_1      0x01
#define IOX_REG_OUTPUT_0     0x02
#define IOX_REG_OUTPUT_1     0x03
#define IOX_REG_POLARITY_0   0x04
#define IOX_REG_POLARITY_1   0x05
#define IOX_REG_CONFIG_0     0x06   /* 1 = input, 0 = output */
#define IOX_REG_CONFIG_1     0x07
#define IOX_REG_PULLUP_EN_0  0x46
#define IOX_REG_PULLUP_EN_1  0x47
#define IOX_REG_PULLUP_SEL_0 0x48
#define IOX_REG_PULLUP_SEL_1 0x49
#define IOX_REG_INT_MASK_0   0x4A   /* 1 = masked, 0 = interrupt enabled */
#define IOX_REG_INT_MASK_1   0x4B

#define IOX_TIMEOUT_MS       100

static const uint8_t IOX_ADDRS[2] = { I2C_ADDR_IOX_0, I2C_ADDR_IOX_1 };

static esp_err_t iox_write_reg(uint8_t exp, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, IOX_ADDRS[exp], buf, 2,
                                      pdMS_TO_TICKS(IOX_TIMEOUT_MS));
}

static esp_err_t iox_read_reg(uint8_t exp, uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(I2C_PORT, IOX_ADDRS[exp], &reg, 1,
                                        val, 1, pdMS_TO_TICKS(IOX_TIMEOUT_MS));
}

static esp_err_t iox_set_pin_direction(uint8_t pin, bool output)
{
    const uint8_t exp = IOX_EXP(pin);
    const uint8_t port = IOX_PORT(pin);
    const uint8_t bit = IOX_BIT(pin);
    const uint8_t reg = port == 0 ? IOX_REG_CONFIG_0 : IOX_REG_CONFIG_1;
    uint8_t val;

    esp_err_t err = iox_read_reg(exp, reg, &val);
    if (err != ESP_OK) return err;
    if (output) val &= (uint8_t)~(1u << bit);
    else        val |= (uint8_t)(1u << bit);
    return iox_write_reg(exp, reg, val);
}

/* Factory direction (Config) and output-default (Output) values for each
 * 8-bit port, recovered from the stock firmware's embedded config
 * (hwconfig_05: p0Dir=0xFE p1Dir=0xFF p2Dir=0x00 p3Dir=0xD4,
 *                p0Data=0xFF p1Data=0xFF p2Data=0x5F p3Data=0xDF). */
#ifdef CONFIG_BOARD_REV_04
/* Rev #04 (single IOX, ET6416): exact configuration bytes consumed by
 * stock function 0x4010cb28. These are safe latch defaults, not the final
 * running state: stock app_main subsequently drives VINHOLD HIGH, PWREN LOW,
 * and the level convertor HIGH before peripheral initialization. */
static const uint8_t IOX0_DIR[2] = { 0xB0, 0xAF };
static const uint8_t IOX0_OUT[2] = { 0x30, 0xEF };
#else
static const uint8_t IOX0_DIR[2] = { 0xFE, 0xFF };
static const uint8_t IOX0_OUT[2] = { 0xFF, 0xFF };
#endif
static const uint8_t IOX1_DIR[2] = { 0x00, 0xD4 };
static const uint8_t IOX1_OUT[2] = { 0x5F, 0xDF };

#ifdef CONFIG_BOARD_REV_04
#define IOX_COUNT 1
#else
#define IOX_COUNT 2
#endif

esp_err_t iox_init(void)
{
    esp_err_t err;

    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) return err;

    /* Rev #04 has one ET6416; newer boards have two PI4IOE5V6416 parts. */
    for (int exp = 0; exp < IOX_COUNT; exp++) {
        uint8_t probe = 0;
        err = i2c_master_write_read_device(I2C_PORT, IOX_ADDRS[exp], &(uint8_t){0x00},
                                           1, &probe, 1, pdMS_TO_TICKS(IOX_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IO expander %d (0x%02x) not responding: %s",
                     exp, IOX_ADDRS[exp], esp_err_to_name(err));
        }
    }

    /* Factory ET6416 init @ 0x4010cb28 writes both output latches before
     * changing directions, avoiding reset/power glitches on newly-enabled
     * outputs. Every write is required. */
    static const uint8_t *const dirs[2] = { IOX0_DIR, IOX1_DIR };
    static const uint8_t *const outs[2] = { IOX0_OUT, IOX1_OUT };
    for (int exp = 0; exp < IOX_COUNT; exp++) {
        for (int port = 0; port < 2; port++) {
            err = iox_write_reg(exp, IOX_REG_OUTPUT_0 + port, outs[exp][port]);
            if (err != ESP_OK) return err;
        }
        for (int port = 0; port < 2; port++) {
            err = iox_write_reg(exp, IOX_REG_CONFIG_0 + port, dirs[exp][port]);
            if (err != ESP_OK) return err;
        }
    }

    /* Exact stock run-state transitions from app_main @ 0x400daa89 and
     * 0x400daad1: hold VIN, retain active-low PWREN, then enable the board
     * level convertor. Without the final HIGH transition, the display-side
     * IOX still works but downstream SD/NFC/audio devices remain isolated. */
    err = iox_set_pin(IOX_POWER_VINHOLD, true);
    if (err != ESP_OK) return err;
    err = iox_set_pin(IOX_POWER_PWREN, false);
    if (err != ESP_OK) return err;
    err = iox_set_pin(IOX_POWER_LEVELCONVERTOR, true);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "%dx IO expander initialized (SDA=%d SCL=%d)",
             IOX_COUNT, PIN_I2C_SDA, PIN_I2C_SCL);
    return ESP_OK;
}

esp_err_t iox_prepare_power_button_wake(void)
{
#ifdef CONFIG_BOARD_REV_04
    /* The rev #04 ET6416 rejects PI4IOE5V6416 interrupt-mask registers.
     * Its caller clears pending input transitions before arming the shared
     * active-low interrupt output directly. */
    return ESP_ERR_NOT_SUPPORTED;
#else
    const uint8_t exp = IOX_EXP(IOX_BTN_POWER);
    const uint8_t port = IOX_PORT(IOX_BTN_POWER);
    const uint8_t bit = IOX_BIT(IOX_BTN_POWER);
    uint8_t ignored;
    esp_err_t err;

    /* PI4IOE5V6416 interrupt masks power up as 0xFF. Without clearing the
     * power-button bit, GPIO34 never asserts and ESP sleep cannot wake. */
    err = iox_write_reg(exp, IOX_REG_INT_MASK_0, port == 0
        ? (uint8_t)~(1u << bit) : 0xFF);
    if (err != ESP_OK) return err;
    err = iox_write_reg(exp, IOX_REG_INT_MASK_1, port == 1
        ? (uint8_t)~(1u << bit) : 0xFF);
    if (err != ESP_OK) return err;

    /* Reading the input port clears any transition latched before wake was
     * armed. The button-release wait in app_main guarantees the level is high. */
    return iox_read_reg(exp, port == 0 ? IOX_REG_INPUT_0 : IOX_REG_INPUT_1,
                        &ignored);
#endif
}

esp_err_t iox_set_peripherals_powered(bool powered)
{
    esp_err_t err;

    if (!powered)
    {
        err = iox_set_pin(IOX_POWER_LEVELCONVERTOR, false);
        if (err != ESP_OK) return err;
        err = iox_set_pin_direction(IOX_POWER_PWREN, false);
        if (err != ESP_OK) return err;
#ifndef CONFIG_BOARD_REV_04
        err = iox_set_pin(IOX_POWER_VOUTEN, false);
        if (err != ESP_OK) return err;
#endif
        return ESP_OK;
    }

    err = iox_set_pin(IOX_POWER_VINHOLD, true);
    if (err != ESP_OK) return err;
#ifndef CONFIG_BOARD_REV_04
    err = iox_set_pin(IOX_POWER_VOUTEN, true);
    if (err != ESP_OK) return err;
#endif
    /* PWREN is active-low. Preload its latch before enabling the output so
     * the rail cannot see a HIGH pulse during the direction transition. */
    err = iox_set_pin(IOX_POWER_PWREN, false);
    if (err != ESP_OK) return err;
    err = iox_set_pin_direction(IOX_POWER_PWREN, true);
    if (err != ESP_OK) return err;
    return iox_set_pin(IOX_POWER_LEVELCONVERTOR, true);
}

esp_err_t iox_set_pin(uint8_t pin, bool level)
{
    uint8_t exp = IOX_EXP(pin);
    uint8_t port = IOX_PORT(pin);
    uint8_t bit = IOX_BIT(pin);
    uint8_t reg = (port == 0) ? IOX_REG_OUTPUT_0 : IOX_REG_OUTPUT_1;
    uint8_t val = 0;

    esp_err_t err = iox_read_reg(exp, reg, &val);
    if (err != ESP_OK) return err;
    if (level) val |= (1u << bit);
    else       val &= ~(1u << bit);
    return iox_write_reg(exp, reg, val);
}

bool iox_get_pin(uint8_t pin)
{
    uint8_t exp = IOX_EXP(pin);
    uint8_t port = IOX_PORT(pin);
    uint8_t bit = IOX_BIT(pin);
    uint8_t reg = (port == 0) ? IOX_REG_INPUT_0 : IOX_REG_INPUT_1;
    uint8_t val = 0;
    if (iox_read_reg(exp, reg, &val) != ESP_OK) return false;
    return (val >> bit) & 0x01;
}

esp_err_t iox_read_port(uint8_t expander, uint8_t port, uint8_t *value)
{
    uint8_t reg = (port == 0) ? IOX_REG_INPUT_0 : IOX_REG_INPUT_1;
    return iox_read_reg(expander, reg, value);
}

esp_err_t iox_read_output_port(uint8_t expander, uint8_t port, uint8_t *value)
{
    uint8_t reg = (port == 0) ? IOX_REG_OUTPUT_0 : IOX_REG_OUTPUT_1;

    return iox_read_reg(expander, reg, value);
}
