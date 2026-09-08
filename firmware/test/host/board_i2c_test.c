/* Resource-failure regression for the real board bus owner.
 * Build with stubs before components/board in the include path and link
 * components/board/board_i2c.c. Run with no argument, "remove", and "delete";
 * each process begins with a fresh bus-owner state. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_i2c.h"
#include "board_pins.h"
#include "driver/i2c_master.h"

struct host_i2c_bus { bool live; };
struct host_i2c_device {
    bool live;
    uint8_t registers[256];
};

static struct host_i2c_bus s_model_bus;
static struct host_i2c_device s_model_devices[128];
static unsigned s_live_devices;
static int s_fail_add_after = -1;
static bool s_fail_create;
static bool s_fail_remove;
static bool s_fail_delete;
static bool s_check_publication;

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "injected error";
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *bus)
{
    (void)config;
    assert(!s_model_bus.live);
    if (s_fail_create) return ESP_ERR_NO_MEM;
    s_model_bus.live = true;
    *bus = &s_model_bus;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    assert(bus == &s_model_bus && bus->live);
    if (s_check_publication && s_live_devices != 0) {
        uint8_t data[] = { 0, 0x55 };
        assert(board_i2c_init() == ESP_ERR_INVALID_STATE);
        assert(board_i2c_write(I2C_ADDR_IOX_0, data, sizeof(data), 100)
               == ESP_ERR_INVALID_STATE);
    }
    if (s_fail_add_after == (int)s_live_devices) return ESP_ERR_NO_MEM;
    assert(config->device_address < 128);
    struct host_i2c_device *model = &s_model_devices[config->device_address];
    /* A mono amp's left/right aliases must not register the same device twice. */
    assert(!model->live);
    model->live = true;
    s_live_devices++;
    *device = model;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(s_model_bus.live && device->live);
    if (s_fail_remove) return ESP_ERR_INVALID_STATE;
    device->live = false;
    s_live_devices--;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus == &s_model_bus && bus->live);
    /* Deleting an owner with surviving devices would make those handles stale. */
    assert(s_live_devices == 0);
    if (s_fail_delete) return ESP_FAIL;
    bus->live = false;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t *data, size_t size, int timeout_ms)
{
    assert(s_model_bus.live && device->live && timeout_ms > 0);
    assert(size == 2);
    device->registers[data[0]] = data[1];
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *write_data, size_t write_size,
                                      uint8_t *read_data, size_t read_size,
                                      int timeout_ms)
{
    assert(s_model_bus.live && device->live && timeout_ms > 0);
    assert(write_size == 1 && read_size == 1);
    *read_data = device->registers[*write_data];
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int timeout_ms)
{
    assert(bus == &s_model_bus && bus->live && timeout_ms > 0);
    return address == 0x70 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int main(int argc, char **argv)
{
    uint8_t data[] = { 0x02, 0xA5 };
    uint8_t value = 0;
    assert(board_i2c_write(I2C_ADDR_IOX_0, data, sizeof(data), 100)
           == ESP_ERR_INVALID_STATE);
    assert(board_i2c_probe(0x70, 50) == ESP_ERR_INVALID_STATE);

    s_fail_create = true;
    assert(board_i2c_init() == ESP_ERR_NO_MEM);
    assert(!s_model_bus.live && s_live_devices == 0);
    s_fail_create = false;

    s_fail_add_after = 2;
    s_check_publication = true;
    s_fail_remove = argc > 1 && strcmp(argv[1], "remove") == 0;
    s_fail_delete = argc > 1 && strcmp(argv[1], "delete") == 0;
    assert(board_i2c_init() == ESP_ERR_NO_MEM);
    assert(board_i2c_write(I2C_ADDR_IOX_0, data, sizeof(data), 100)
           == ESP_ERR_INVALID_STATE);
    if (s_fail_remove || s_fail_delete) {
        assert(s_model_bus.live);
        assert(s_live_devices == (s_fail_remove ? 2u : 0u));
        assert(board_i2c_init() == ESP_ERR_INVALID_STATE);
        puts("Board I2C: failed cleanup retains ownership without publication");
        return 0;
    }
    assert(!s_model_bus.live && s_live_devices == 0);

    s_fail_add_after = -1;
    assert(board_i2c_init() == ESP_OK);
    assert(board_i2c_write(I2C_ADDR_IOX_0, data, sizeof(data), 100) == ESP_OK);
    /* Reinitializing a published bus cannot reset state or delete live handles. */
    assert(board_i2c_init() == ESP_OK);
    assert(board_i2c_write_read(I2C_ADDR_IOX_0, data, 1, &value, 1, 100) == ESP_OK);
    assert(value == 0xA5);

    puts("Board I2C: rollback, publication and live-bus lifecycle regression passed");
    return 0;
}
