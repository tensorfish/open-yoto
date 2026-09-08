/* Board-wide I2C ownership. No command links or per-transfer registration. */
#include "board_i2c.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <board_pins.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";

static const uint8_t s_addresses[] = {
    I2C_ADDR_IOX_0,
#ifndef CONFIG_BOARD_REV_04
    I2C_ADDR_IOX_1,
    I2C_ADDR_PD_SINK,
#endif
    I2C_ADDR_FUEL_GAUGE,
    I2C_ADDR_CHARGER,
    I2C_ADDR_ACCELEROMETER,
    I2C_ADDR_SPKR_AMP_L,
#if I2C_ADDR_SPKR_AMP_R != I2C_ADDR_SPKR_AMP_L
    I2C_ADDR_SPKR_AMP_R,
#endif
    I2C_ADDR_HP_DAC,
    I2C_ADDR_RTC,
};

#define DEVICE_COUNT (sizeof(s_addresses) / sizeof(s_addresses[0]))

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_devices[DEVICE_COUNT];
static atomic_bool s_ready;
static atomic_flag s_initializing = ATOMIC_FLAG_INIT;

/* Only used before publication: never tear down a bus that clients can use.
 * If removal fails, retain ownership and do not delete its underlying bus. */
static void board_i2c_cleanup(void)
{
    bool devices_remain = false;
    for (size_t i = DEVICE_COUNT; i > 0; i--) {
        if (s_devices[i - 1] == NULL) continue;
        esp_err_t err = i2c_master_bus_rm_device(s_devices[i - 1]);
        if (err == ESP_OK) {
            s_devices[i - 1] = NULL;
        } else {
            devices_remain = true;
            ESP_LOGE(TAG, "device cleanup failed: %s", esp_err_to_name(err));
        }
    }
    if (!devices_remain && s_bus != NULL) {
        esp_err_t err = i2c_del_master_bus(s_bus);
        if (err == ESP_OK) {
            s_bus = NULL;
        } else {
            ESP_LOGE(TAG, "bus cleanup failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t board_i2c_init(void)
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return ESP_OK;
    if (atomic_flag_test_and_set_explicit(&s_initializing, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) goto done;
    /* A failed cleanup retains its handles; do not overwrite or free them. */
    if (s_bus != NULL) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) goto failed;

    for (size_t i = 0; i < DEVICE_COUNT; i++) {
        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_addresses[i],
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        err = i2c_master_bus_add_device(s_bus, &device_config, &s_devices[i]);
        if (err != ESP_OK) goto failed;
    }
    /* No callbacks: transmit/probe APIs remain synchronous and use the SDK's
     * bus semaphore. Published handles are immutable for the rest of boot. */
    atomic_store_explicit(&s_ready, true, memory_order_release);
    goto done;

failed:
    board_i2c_cleanup();
done:
    atomic_flag_clear_explicit(&s_initializing, memory_order_release);
    return err;
}

static i2c_master_dev_handle_t board_i2c_device(uint8_t address)
{
    for (size_t i = 0; i < DEVICE_COUNT; i++) {
        if (s_addresses[i] == address) return s_devices[i];
    }
    return NULL;
}

esp_err_t board_i2c_write(uint8_t address, const uint8_t *data, size_t size,
                          int timeout_ms)
{
    if (data == NULL || size == 0 || timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t device = board_i2c_device(address);
    if (device == NULL) return ESP_ERR_NOT_FOUND;
    return i2c_master_transmit(device, data, size, timeout_ms);
}

esp_err_t board_i2c_write_read(uint8_t address, const uint8_t *write_data,
                               size_t write_size, uint8_t *read_data,
                               size_t read_size, int timeout_ms)
{
    if (write_data == NULL || write_size == 0 || read_data == NULL ||
        read_size == 0 || timeout_ms <= 0) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t device = board_i2c_device(address);
    if (device == NULL) return ESP_ERR_NOT_FOUND;
    return i2c_master_transmit_receive(device, write_data, write_size,
                                       read_data, read_size, timeout_ms);
}

esp_err_t board_i2c_probe(uint8_t address, int timeout_ms)
{
    if (address < 0x08 || address > 0x77 || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(s_bus, address, timeout_ms);
}
