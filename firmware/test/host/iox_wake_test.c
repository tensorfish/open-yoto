/* Host regression test for IO-expander wake and peripheral power control. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_IOX_WAKE_TEST

static int s_failures;
#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            fprintf(stderr, "FAIL: ");                     \
            fprintf(stderr, __VA_ARGS__);                   \
            fprintf(stderr, "\n");                        \
            s_failures++;                                   \
        }                                                   \
    } while (0)

#include "../../components/iox/iox.c"

typedef struct
{
    uint8_t address;
    uint8_t reg;
    uint8_t value;
} write_record_t;

static uint8_t s_registers[2][256];
static write_record_t s_writes[64];
static size_t s_write_count;
static uint8_t s_last_read_address;
static uint8_t s_last_read_reg;

esp_err_t i2c_param_config(i2c_port_t port, const i2c_config_t *config)
{
    (void)port;
    (void)config;
    return ESP_OK;
}

esp_err_t i2c_driver_install(i2c_port_t port, int mode,
                             size_t rx_buf_len, size_t tx_buf_len,
                             int intr_alloc_flags)
{
    (void)port;
    (void)mode;
    (void)rx_buf_len;
    (void)tx_buf_len;
    (void)intr_alloc_flags;
    return ESP_OK;
}

static int expander_for_address(uint8_t address)
{
    if (address == I2C_ADDR_IOX_0) return 0;
    if (address == I2C_ADDR_IOX_1) return 1;
    return -1;
}

esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t address,
                                       const uint8_t *write_buffer,
                                       size_t write_size,
                                       uint8_t *read_buffer,
                                       size_t read_size,
                                       TickType_t timeout)
{
    int expander = expander_for_address(address);
    (void)port;
    (void)timeout;
    if (expander < 0 || write_size != 1 || read_size != 1)
    {
        return ESP_FAIL;
    }
    s_last_read_address = address;
    s_last_read_reg = write_buffer[0];
    read_buffer[0] = s_registers[expander][write_buffer[0]];
    return ESP_OK;
}

esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t address,
                                     const uint8_t *write_buffer,
                                     size_t write_size,
                                     TickType_t timeout)
{
    int expander = expander_for_address(address);
    (void)port;
    (void)timeout;
    if (expander < 0 || write_size != 2)
    {
        return ESP_FAIL;
    }
    if (s_write_count == sizeof(s_writes) / sizeof(s_writes[0]))
    {
        return ESP_FAIL;
    }
    s_writes[s_write_count++] = (write_record_t){
        .address = address,
        .reg = write_buffer[0],
        .value = write_buffer[1],
    };
    s_registers[expander][write_buffer[0]] = write_buffer[1];
    return ESP_OK;
}

static int find_write(uint8_t address, uint8_t reg, uint8_t value)
{
    for (size_t i = 0; i < s_write_count; i++)
    {
        if (s_writes[i].address == address &&
            s_writes[i].reg == reg &&
            s_writes[i].value == value)
        {
            return (int)i;
        }
    }
    return -1;
}
const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

int main(void)
{
    memset(s_registers, 0, sizeof(s_registers));
    CHECK(iox_init() == ESP_OK, "IOX init failed");

#ifdef CONFIG_BOARD_REV_04
    CHECK(iox_prepare_power_button_wake() == ESP_ERR_NOT_SUPPORTED,
          "rev04 did not report unsupported interrupt-mask control");
#else
    s_write_count = 0;
    s_last_read_address = 0xFF;
    s_last_read_reg = 0xFF;
    CHECK(iox_prepare_power_button_wake() == ESP_OK,
          "rev05 power-button wake preparation failed");
    CHECK(s_write_count == 2,
          "rev05 wake made %zu register writes instead of 2", s_write_count);
    CHECK(s_writes[0].address == I2C_ADDR_IOX_0 &&
          s_writes[0].reg == 0x4A && s_writes[0].value == 0xFF,
          "rev05 wake did not mask expander0 register 0x4A with 0xFF");
    CHECK(s_writes[1].address == I2C_ADDR_IOX_0 &&
          s_writes[1].reg == 0x4B && s_writes[1].value == 0xF7,
          "rev05 wake did not enable only the power button in register 0x4B");
    CHECK(s_last_read_address == I2C_ADDR_IOX_0 && s_last_read_reg == 0x01,
          "rev05 wake did not clear expander0 input1 transitions");
#endif

    const uint8_t power_exp = IOX_EXP(IOX_POWER_PWREN);
    const uint8_t power_port = IOX_PORT(IOX_POWER_PWREN);
    const uint8_t power_mask = (uint8_t)(1u << IOX_BIT(IOX_POWER_PWREN));
    const uint8_t power_address = IOX_ADDRS[power_exp];
    const uint8_t output_reg = (uint8_t)(IOX_REG_OUTPUT_0 + power_port);
    const uint8_t config_reg = (uint8_t)(IOX_REG_CONFIG_0 + power_port);
    const uint8_t shutdown_config = (uint8_t)(0x5A & (uint8_t)~power_mask);

    s_registers[power_exp][config_reg] = shutdown_config;
    s_registers[power_exp][output_reg] = 0xFF;
    s_write_count = 0;
    CHECK(iox_set_peripherals_powered(false) == ESP_OK,
          "peripheral shutdown failed");
    CHECK(s_registers[power_exp][config_reg] ==
          (uint8_t)(shutdown_config | power_mask),
          "shutdown did not preserve config bits while tri-stating PWREN");
    CHECK((s_registers[power_exp][output_reg] & power_mask) != 0,
          "shutdown actively changed the PWREN output latch");

    const uint8_t power_on_config = (uint8_t)(0xA5 | power_mask);
    const uint8_t powered_latch = (uint8_t)(0xFF & (uint8_t)~power_mask);
    const uint8_t powered_config =
        (uint8_t)(power_on_config & (uint8_t)~power_mask);
    s_registers[power_exp][config_reg] = power_on_config;
    s_registers[power_exp][output_reg] = 0xFF;
    s_write_count = 0;
    CHECK(iox_set_peripherals_powered(true) == ESP_OK,
          "peripheral power-on failed");

    const int latch_write =
        find_write(power_address, output_reg, powered_latch);
    const int direction_write =
        find_write(power_address, config_reg, powered_config);
    CHECK(latch_write >= 0,
          "power-on did not preload the PWREN output latch LOW");
    CHECK(direction_write == latch_write + 1,
          "power-on did not preload PWREN immediately before enabling output");
    CHECK(s_registers[power_exp][config_reg] == powered_config,
          "power-on did not preserve unrelated PWREN config bits");

    if (s_failures != 0)
    {
        fprintf(stderr, "IOX wake test: %d assertion(s) failed\n", s_failures);
        return 1;
    }
    printf("IOX wake test: all assertions passed\n");
    return 0;
}
