/*
 * Host-test stub for the board constants referenced by display.c and
 * battery.c.
 */
#ifndef HOST_STUB_BOARD_PINS_H
#define HOST_STUB_BOARD_PINS_H

#define PIN_DISPLAY_BACKLIGHT 26
#define PIN_IOX_INT 34

#define I2C_PORT 0
#define I2C_ADDR_FUEL_GAUGE 0x64
#define I2C_ADDR_CHARGER 0x1A
#define I2C_ADDR_PD_SINK 0x08

#define IOX_BAT_ALERT 1
#define IOX_CHG_STAT 2
#define IOX_USB_POWER_STAT 3
#define IOX_QI_CHARGE_ENABLE_N 4
#define IOX_USB_CHARGE_ENABLE_N 5
#define IOX_BTN_POWER 6
#define IOX_AUDIO_PACTRL 7
#define IOX_POWER_VINHOLD 8

#endif /* HOST_STUB_BOARD_PINS_H */
