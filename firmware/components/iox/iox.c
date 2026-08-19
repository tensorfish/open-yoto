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

/* Factory direction (Config) and output-default (Output) values for each
 * 8-bit port, recovered from the stock firmware's embedded config
 * (hwconfig_05: p0Dir=0xFE p1Dir=0xFF p2Dir=0x00 p3Dir=0xD4,
 *                p0Data=0xFF p1Data=0xFF p2Data=0x5F p3Data=0xDF). */
static const uint8_t IOX0_DIR[2] = { 0xFE, 0xFF };
static const uint8_t IOX0_OUT[2] = { 0xFF, 0xFF };
static const uint8_t IOX1_DIR[2] = { 0x00, 0xD4 };
static const uint8_t IOX1_OUT[2] = { 0x5F, 0xDF };

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

    /* probe both expanders */
    for (int exp = 0; exp < 2; exp++) {
        uint8_t probe = 0;
        err = i2c_master_write_read_device(I2C_PORT, IOX_ADDRS[exp], &(uint8_t){0x00},
                                           1, &probe, 1, pdMS_TO_TICKS(IOX_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IO expander %d (0x%02x) not responding: %s",
                     exp, IOX_ADDRS[exp], esp_err_to_name(err));
        }
    }

    /* apply factory direction + output defaults */
    const uint8_t (*dir)[2] = IOX0_DIR;
    const uint8_t (*out)[2] = IOX0_OUT;
    for (int exp = 0; exp < 2; exp++) {
        dir = (exp == 0) ? &IOX0_DIR : &IOX1_DIR;
        out = (exp == 0) ? &IOX0_OUT : &IOX1_OUT;
        for (int port = 0; port < 2; port++) {
            iox_write_reg(exp, IOX_REG_CONFIG_0 + port, (*dir)[port]);
            iox_write_reg(exp, IOX_REG_OUTPUT_0 + port, (*out)[port]);
        }
    }

    ESP_LOGI(TAG, "2x PI4IOE5V6416 initialized (SDA=%d SCL=%d)",
             PIN_I2C_SDA, PIN_I2C_SCL);
    return ESP_OK;
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
