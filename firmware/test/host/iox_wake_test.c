/* Host regression test for the power-button IO-expander wake mask. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_BOARD_REV_04
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

static uint8_t s_registers[2][256];
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
    s_registers[expander][write_buffer[0]] = write_buffer[1];
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

int main(void)
{
    memset(s_registers, 0, sizeof(s_registers));
    CHECK(iox_init() == ESP_OK,
          "rev04 IOX init touched unsupported interrupt-mask registers");
    CHECK(iox_prepare_power_button_wake() == ESP_ERR_NOT_SUPPORTED,
          "rev04 did not select timer-backed wake fallback");

    if (s_failures != 0)
    {
        fprintf(stderr, "IOX wake test: %d assertion(s) failed\n", s_failures);
        return 1;
    }
    printf("IOX wake test: all assertions passed\n");
    return 0;
}
