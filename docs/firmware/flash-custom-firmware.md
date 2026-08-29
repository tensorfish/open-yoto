---
icon: lucide/cpu
---

# Flash custom firmware

This replaces the stock firmware on the Yoto Mini's ESP32. Read the whole guide
before opening the player.

!!! danger "You can permanently damage the player"
    Opening the Yoto Mini voids its warranty. A wiring mistake or interrupted
    flash can leave it unbootable. Save the original firmware before writing
    anything: it contains identifiers unique to your player.

## Supported hardware

Current Open Yoto builds support Yoto Mini board revisions **#04 and #05**.
Identify your board before flashing; revisions use different display, audio,
NFC, storage, and power hardware.

- [Board revisions and pin maps](../hardware.md)
- [Physical disassembly and connector overview](https://learn.adafruit.com/hacking-the-yoto-music-players/overview)

## What you need

- A supported Yoto Mini.
- An external USB-UART adapter with **3.3 V logic**.
- Access to UART0, **BOOT/GPIO0**, **RESET/EN**, and ground inside the player.
- A computer with ESP-IDF v5.5 and this repository.

The player's USB-C port supplies power only. It does not expose the ESP32 UART,
so a USB-C cable cannot flash the firmware.

## 1. Back up the player

Before changing the flash or SD card:

1. Dump the complete 8 MiB stock ESP32 flash.
2. Make a second copy of that dump somewhere outside this repository.
3. Back up the SD card.
4. Keep the backups tied to this specific player.

Do not use a stock dump from another Yoto Mini as a substitute. Device-specific
identifiers are part of the original image.

## 2. Connect the UART adapter

Connect the USB-UART adapter as follows:

| USB-UART line | Player signal |
|---|---|
| GND | GND |
| Adapter RX | ESP32 UART0 TX |
| Adapter TX | ESP32 UART0 RX |

Keep the **BOOT/GPIO0** and **RESET/EN** controls accessible. You will operate
them manually while `idf.py` connects.

Do not connect a 5 V UART signal to the ESP32. Confirm the connector location
and pinout for your board revision before applying power.

## 3. Build Open Yoto

Follow [Build environment setup](../build-setup.md) to install ESP-IDF v5.5 and
build the `firmware/` project for the `esp32` target.

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32
idf.py build
```

## 4. Flash the firmware

From the `firmware/` directory, with the ESP-IDF environment loaded, start the
flash:

```bash
idf.py flash
```

While `idf.py` is trying to connect:

1. Press and hold **BOOT**.
2. While continuing to hold **BOOT**, press and release **RESET**.
3. Release **BOOT**.

This starts the ESP32 ROM bootloader. Leave `idf.py` running; it will connect
and flash the firmware. When the flash completes, press and release **RESET**
once more to boot Open Yoto.

## 5. Check the first boot

A successful boot log reports initialization of the SD card, display, audio,
and NFC reader. Let the welcome sound finish, then continue to
[Post-flash setup](post-flash-setup.md) to create the admin card and install the
local web interface.
