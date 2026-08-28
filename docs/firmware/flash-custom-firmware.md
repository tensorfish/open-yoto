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

The verified automatic-control wiring is:

| USB-UART line | Player signal |
|---|---|
| GND | GND |
| Adapter RX | ESP32 UART0 TX |
| Adapter TX | ESP32 UART0 RX |
| RTS | BOOT/GPIO0 |
| DTR | RESET/EN |

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

From the repository root, with the ESP-IDF environment loaded:

```bash
python3 tools/flash_esp32.py
```

The helper builds the firmware, enters the ROM bootloader using RTS/DTR, flashes
the image, resets the player, and captures its UART output. Its default Linux
serial port is `/dev/ttyUSB0`; use `--port` when yours differs:

```bash
python3 tools/flash_esp32.py --port /dev/ttyUSB1
```

For a conventional adapter or a manually controlled boot sequence, the detailed
`idf.py` and `esptool.py` alternatives are in
[Build environment setup](../build-setup.md#flash).

## 5. Check the first boot

A successful boot log reports initialization of the SD card, display, audio,
and NFC reader. Let the welcome sound finish, then continue to
[Post-flash setup](post-flash-setup.md) to create the admin card and install the
local web interface.
