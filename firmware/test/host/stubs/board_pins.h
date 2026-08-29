/*
 * Host-test stub for the board constants referenced by display.c and
 * battery.c.
 */
#ifndef HOST_STUB_BOARD_PINS_H
#define HOST_STUB_BOARD_PINS_H

#define PIN_DISPLAY_BACKLIGHT 26
#define PIN_IOX_INT 34
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 25
#define I2C_MASTER_FREQ_HZ 100000

#define I2C_PORT 0
#define I2C_ADDR_IOX_0 0x20
#define I2C_ADDR_IOX_1 0x21
#define I2C_ADDR_FUEL_GAUGE 0x64
#define I2C_ADDR_CHARGER 0x1A
#define I2C_ADDR_PD_SINK 0x08

#ifdef HOST_IOX_WAKE_TEST
#define IOX_PIN(exp, port, bit) (((exp) << 6) | ((port) << 4) | (bit))
#define IOX_EXP(x) (((x) >> 6) & 0x03)
#define IOX_PORT(x) (((x) >> 4) & 0x03)
#define IOX_BIT(x) ((x) & 0x0F)
#define IOX_BTN_POWER IOX_PIN(0, 1, 3)
#define IOX_AUDIO_PACTRL IOX_PIN(0, 0, 6)
#define IOX_POWER_LEVELCONVERTOR IOX_PIN(0, 0, 3)
#define IOX_POWER_PWREN IOX_PIN(0, 1, 4)
#define IOX_POWER_VINHOLD IOX_PIN(0, 1, 6)
#else
#define IOX_BAT_ALERT 1
#define IOX_CHG_STAT 2
#define IOX_USB_POWER_STAT 3
#define IOX_QI_CHARGE_ENABLE_N 4
#define IOX_USB_CHARGE_ENABLE_N 5
#define IOX_BTN_POWER 6
#define IOX_AUDIO_PACTRL 7
#define IOX_POWER_VINHOLD 8
#endif

#endif /* HOST_STUB_BOARD_PINS_H */
