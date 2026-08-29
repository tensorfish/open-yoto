/*
 * Host-side charger regression test. Compiled once for each supported board,
 * this includes the real battery.c and models the charger and I/O expander.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            fprintf(stderr, "FAIL: ");                     \
            fprintf(stderr, __VA_ARGS__);                   \
            fprintf(stderr, "\n");                         \
            s_failures++;                                   \
        }                                                   \
    } while (0)

#include "../../components/battery/battery.c"

static TickType_t s_tick_ms;
static bool s_iox_levels[16];
static uint8_t s_charger_registers[256];
static uint8_t s_pd_registers[256];
static bool s_charger_responds;
static bool s_pd_responds;
static unsigned s_charger_reads;
static unsigned s_charger_writes;
static uint8_t s_set_pins[16];
static bool s_set_levels[16];
static unsigned s_set_count;

TickType_t xTaskGetTickCount(void)
{
    return s_tick_ms;
}

void vTaskDelay(TickType_t ticks)
{
    s_tick_ms += ticks;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

esp_err_t iox_init(void)
{
    return ESP_OK;
}

esp_err_t iox_set_peripherals_powered(bool powered)
{
    (void)powered;
    return ESP_OK;
}

esp_err_t iox_set_pin(uint8_t pin, bool level)
{
    s_iox_levels[pin] = level;
    s_set_pins[s_set_count] = pin;
    s_set_levels[s_set_count] = level;
    s_set_count++;
    return ESP_OK;
}

bool iox_get_pin(uint8_t pin)
{
    return s_iox_levels[pin];
}

esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t address,
                                       const uint8_t *write_buffer,
                                       size_t write_size,
                                       uint8_t *read_buffer,
                                       size_t read_size,
                                       TickType_t timeout)
{
    (void)port;
    (void)timeout;
    if (write_size != 1 || read_size != 1)
    {
        return ESP_FAIL;
    }
    if (address == I2C_ADDR_CHARGER && s_charger_responds)
    {
        s_charger_reads++;
        read_buffer[0] = s_charger_registers[write_buffer[0]];
        return ESP_OK;
    }
    if (address == I2C_ADDR_PD_SINK && s_pd_responds)
    {
        read_buffer[0] = s_pd_registers[write_buffer[0]];
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t address,
                                     const uint8_t *write_buffer,
                                     size_t write_size,
                                     TickType_t timeout)
{
    (void)port;
    (void)timeout;
    if (address != 0x1A || !s_charger_responds
        || write_size != 2)
    {
        return ESP_FAIL;
    }

    s_charger_writes++;
    s_charger_registers[write_buffer[0]] = write_buffer[1];
    return ESP_OK;
}

static void reset_model(void)
{
    s_tick_ms = 0;
    memset(s_iox_levels, 0, sizeof(s_iox_levels));
    memset(s_charger_registers, 0, sizeof(s_charger_registers));
    memset(s_pd_registers, 0, sizeof(s_pd_registers));
    memset(s_set_pins, 0, sizeof(s_set_pins));
    memset(s_set_levels, 0, sizeof(s_set_levels));
    s_charger_responds = true;
    s_pd_responds = true;
    s_charger_reads = 0;
    s_charger_writes = 0;
    s_set_count = 0;

    /* Active-low VBUS and STAT both start inactive. */
    s_iox_levels[IOX_USB_POWER_STAT] = true;
    s_iox_levels[IOX_CHG_STAT] = true;

#ifndef CONFIG_BOARD_REV_04
    /* A 5 V Type-C source advertises 3 A; the firmware caps this to the
     * SGM41513's stock 2.4 A input limit. */
    s_pd_registers[HUSB238_REG_PD_STATUS1] =
        HUSB238_ATTACHED | HUSB238_5V_CONTRACT | 0x03;
#endif

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
}

static void test_configures_only_when_power_appears(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_INPUT_SOURCE] = 0xE5;
    s_charger_registers[SGM41513_REG_POWER_ON] = SGM41513_OTG_CONFIG;
    s_charger_registers[SGM41513_REG_CHARGE_CURRENT] = 0x80;
    s_charger_registers[SGM41513_REG_TIMER_CONTROL] = 0x31;
    s_charger_registers[SGM41513_REG_MISC_CONTROL] = SGM41513_BATFET_DIS;

    CHECK(battery_service() == ESP_OK, "service without VBUS failed");
    CHECK(s_charger_reads == 1 && s_charger_writes == 0,
          "charger status was not sampled exactly once without VBUS");

    /* REG08 PG_STAT is authoritative; the board plug-status pin is not. */
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "plug-in configuration failed");
    CHECK(s_charger_present && s_charger_configured,
          "charger was not marked configured");
#ifdef CONFIG_BOARD_REV_04
    CHECK((s_charger_registers[SGM41513_REG_INPUT_SOURCE]
           & SGM41513_IINDPM_MASK) == 0x05,
          "rev04 overwrote the charger's source-detected input limit");
#else
    CHECK((s_charger_registers[SGM41513_REG_INPUT_SOURCE]
           & SGM41513_IINDPM_MASK) == 0x17,
          "rev05 did not apply the 2.4 A USB-C contract limit");
#endif
    CHECK((s_charger_registers[SGM41513_REG_POWER_ON]
           & (SGM41513_OTG_CONFIG | SGM41513_CHG_CONFIG))
              == SGM41513_CHG_CONFIG,
          "normal charging path was not enabled");
#ifdef CONFIG_BOARD_REV_04
    CHECK((s_charger_registers[SGM41513_REG_CHARGE_CURRENT]
           & (SGM41513_Q1_FULLON | SGM41513_ICHG_MASK)) == 0x76,
          "rev04 did not request the stock 2220 mA code");
#else
    CHECK((s_charger_registers[SGM41513_REG_CHARGE_CURRENT]
           & (SGM41513_Q1_FULLON | SGM41513_ICHG_MASK)) == 0x68,
          "rev05 did not request the stock 1020 mA code");
#endif
    CHECK((s_charger_registers[SGM41513_REG_TIMER_CONTROL]
           & SGM41513_WATCHDOG_MASK) == 0,
          "charger watchdog remained enabled");
    CHECK((s_charger_registers[SGM41513_REG_MISC_CONTROL]
           & SGM41513_BATFET_DIS) == 0,
          "battery FET remained disconnected");
#ifdef CONFIG_BOARD_REV_04
    CHECK(s_charger_registers[SGM41513_REG_STOCK_CONTROL] == 0x45,
          "rev04 stock control write is missing");
    CHECK(s_set_count == 0, "rev04 unexpectedly selected a rev05 input path");
#else
    CHECK(s_set_count == 2, "rev05 did not select exactly one input path");
    CHECK(s_set_pins[0] == IOX_QI_CHARGE_ENABLE_N && s_set_levels[0],
          "rev05 Qi path was not disabled first");
    CHECK(s_set_pins[1] == IOX_USB_CHARGE_ENABLE_N && !s_set_levels[1],
          "rev05 USB charge path was not enabled");
#endif

    unsigned writes = s_charger_writes;
    CHECK(battery_service() == ESP_OK, "idempotent service failed");
    CHECK(s_charger_writes == writes,
          "configured charger was rewritten on every poll");
}

static void test_unplug_replug_reconfigures(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "initial plug-in failed");

    s_charger_registers[SGM41513_REG_STATUS] = 0;
    CHECK(battery_service() == ESP_OK, "unplug service failed");
    CHECK(s_charger_present && !s_charger_configured,
          "unplug did not invalidate charger configuration");

    /* The real charger loses power here, including CHG_CONFIG. */
    memset(s_charger_registers, 0, sizeof(s_charger_registers));
    unsigned writes = s_charger_writes;
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "replug configuration failed");
    CHECK(s_charger_writes > writes,
          "replug did not restore charger registers");
    CHECK((s_charger_registers[SGM41513_REG_POWER_ON]
           & SGM41513_CHG_CONFIG) != 0,
          "charging remained disabled after replug");
}

static void test_probe_failure_recovers(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    s_charger_responds = false;
    CHECK(battery_service() != ESP_OK, "missing charger unexpectedly succeeded");
    CHECK(!s_charger_configured, "missing charger was marked configured");

    s_charger_responds = true;
    CHECK(battery_service() == ESP_OK, "charger did not recover on next poll");
    CHECK(s_charger_configured, "recovered charger was not configured");
}

static void test_rev05_uses_advertised_input_limit(void)
{
#ifdef CONFIG_BOARD_REV_04
    return;
#else
    reset_model();
    s_charger_registers[SGM41513_REG_INPUT_SOURCE] = 0x05;
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    s_pd_registers[HUSB238_REG_PD_STATUS1] =
        HUSB238_ATTACHED | HUSB238_5V_CONTRACT | 0x01;
    CHECK(battery_service() == ESP_OK, "1.5 A USB-C configuration failed");
    CHECK((s_charger_registers[SGM41513_REG_INPUT_SOURCE]
           & SGM41513_IINDPM_MASK) == 0x0E,
          "rev05 did not respect the advertised 1.5 A limit");

    reset_model();
    s_charger_registers[SGM41513_REG_INPUT_SOURCE] = 0x05;
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    s_pd_responds = false;
    CHECK(battery_service() == ESP_OK, "legacy-source configuration failed");
    CHECK((s_charger_registers[SGM41513_REG_INPUT_SOURCE]
           & SGM41513_IINDPM_MASK) == 0x05,
          "legacy source did not retain the safe detector limit");
#endif
}

static void test_charging_status_uses_charger_state(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_CHRG_STAT_FAST;
    CHECK(battery_service() == ESP_OK, "no-input status service failed");
    CHECK(!battery_is_charging(), "charge state without PG_STAT reported charging");

    s_charger_registers[SGM41513_REG_STATUS] =
        SGM41513_PG_STAT | SGM41513_CHRG_STAT_FAST;
    CHECK(battery_service() == ESP_OK, "fast-charge status service failed");
    CHECK(battery_is_charging(), "fast-charge status was not reported");

    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "disabled-charge status service failed");
    CHECK(!battery_is_charging(), "disabled charger reported charging");
}

static void test_periodic_audit_repairs_owned_configuration(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_INPUT_SOURCE] = 0x05;
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "audit setup configuration failed");

    uint8_t detected_iindpm =
        s_charger_registers[SGM41513_REG_INPUT_SOURCE]
        & SGM41513_IINDPM_MASK;
    unsigned writes = s_charger_writes;
    s_tick_ms += BATTERY_CHARGER_AUDIT_MS;
    CHECK(battery_service() == ESP_OK, "stable charger audit failed");
    CHECK(s_charger_writes == writes,
          "stable charger audit performed unnecessary writes");

    s_charger_registers[SGM41513_REG_CHARGE_CURRENT] = 0;
    s_charger_registers[SGM41513_REG_TIMER_CONTROL] |=
        SGM41513_WATCHDOG_MASK;
#ifndef CONFIG_BOARD_REV_04
    /* An unrelated repair must not rewrite IINDPM if the reported contract
     * changes after initial source selection. */
    s_pd_registers[HUSB238_REG_PD_STATUS1] =
        HUSB238_ATTACHED | HUSB238_5V_CONTRACT | 0x01;
#endif
    s_charger_registers[SGM41513_REG_MISC_CONTROL] |= SGM41513_BATFET_DIS;
#ifdef CONFIG_BOARD_REV_04
    s_charger_registers[SGM41513_REG_STOCK_CONTROL] = 0x75;
#endif
    s_tick_ms += BATTERY_CHARGER_AUDIT_MS;
    CHECK(battery_service() == ESP_OK, "drift repair failed");
    CHECK((s_charger_registers[SGM41513_REG_CHARGE_CURRENT]
           & (SGM41513_Q1_FULLON | SGM41513_ICHG_MASK))
              == (SGM41513_Q1_FULLON | SGM41513_ICHG_CODE),
          "audit did not restore charge current");
    CHECK((s_charger_registers[SGM41513_REG_TIMER_CONTROL]
           & SGM41513_WATCHDOG_MASK) == 0,
          "audit did not restore watchdog-disable state");
    CHECK((s_charger_registers[SGM41513_REG_MISC_CONTROL]
           & SGM41513_BATFET_DIS) != 0,
          "audit unsafely cleared latched BATFET protection");
    CHECK((s_charger_registers[SGM41513_REG_INPUT_SOURCE]
           & SGM41513_IINDPM_MASK) == detected_iindpm,
          "audit overwrote the source-authoritative input limit");
#ifdef CONFIG_BOARD_REV_04
    CHECK(s_charger_registers[SGM41513_REG_STOCK_CONTROL] == 0x45,
          "audit did not restore rev04 stock temperature control");
#endif
}

static void test_status_read_failure_preserves_known_state(void)
{
    reset_model();
    s_charger_registers[SGM41513_REG_STATUS] = SGM41513_PG_STAT;
    CHECK(battery_service() == ESP_OK, "failure-state setup failed");

    s_charger_responds = false;
    CHECK(battery_service() != ESP_OK, "status failure unexpectedly succeeded");
    CHECK(s_charger_configured && s_external_power_present,
          "status failure was misclassified as unplug/configuration loss");

    s_charger_responds = true;
    CHECK(battery_service() == ESP_OK, "status failure did not recover");
}

int main(void)
{
    CHECK(I2C_ADDR_CHARGER == 0x1A,
          "SGM41513 must use its stock 7-bit address 0x1A");
    test_configures_only_when_power_appears();
    test_unplug_replug_reconfigures();
    test_probe_failure_recovers();
    test_rev05_uses_advertised_input_limit();
    test_charging_status_uses_charger_state();
    test_periodic_audit_repairs_owned_configuration();
    test_status_read_failure_preserves_known_state();

    if (s_failures != 0)
    {
        fprintf(stderr, "%d charger test(s) failed\n", s_failures);
        return 1;
    }
    return 0;
}
