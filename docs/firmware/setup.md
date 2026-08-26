---
icon: lucide/wrench
---

# Installation & setup

This guide is the installation outline for the replacement firmware. It will
be expanded with board-revision photos and connector locations as they are
verified.

## Before you begin

- Identify the player's board revision.
- Back up anything important from the SD card.
- Use the external USB-UART adapter; the player's USB-C port is power-only and
  cannot flash the original ESP32 by itself.
- Ensure the adapter can control **BOOT/GPIO0** and **RESET/EN**. See
  [Build & flash](../build-setup.md) for the verified RTS/DTR wiring.

## Toolchain

1. Install ESP-IDF v5.5 and its ESP32 tools.
2. Clone this repository with its managed dependencies.
3. Source the ESP-IDF environment.
4. Build the `firmware/` project for `esp32`.

The exact commands and supported Linux UART workflow are in
[Build & flash](../build-setup.md).

## Flash the player

1. Connect the external USB-UART adapter to the player programming connection.
2. Run `python3 tools/flash_esp32.py` from the repository root.
3. Confirm the captured boot log reports SD mount, display, audio, and NFC
   initialization.

## First boot

1. Allow the stock welcome asset to complete.
2. Put a card with `https://openyoto.com/admin` on the reader to open admin
   mode.
3. Join the `openyoto` hotspot and enter the six-character access code shown
   on the player.
4. Use the web UI to upload its current `index.html`, manage media, and create
   card mappings.

## Power control

Hold the dedicated power button for three seconds to switch off. This
disconnects downstream peripheral rails and leaves the player in its
low-activity off state. Hold it for three seconds again to restore the rails
and restart the player.

## Prepare media

- Store audio and `.img` artwork beneath `/sdcard/media`.
- Use the Cards UI to select a directory, choose tracks, associate artwork, and
  write the generated card URL to a scanned card.
- The firmware stores the mapping catalog as `/sdcard/media/library.json`.

## Setup details to add

- Board-revision identification photographs.
- Programming connector location and adapter pinout photographs.
- SD-card migration and recovery procedure.
- End-to-end first-card walkthrough.
