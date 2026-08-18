---
icon: lucide/bot
---

# Firmware Frameworks for ESP32 — AI Reference

Dense, source-grounded comparison of the ways to write ESP32 firmware, focused
on the Yoto re-firmware project (NFC CR95HF + SD + 16×16 display + I2S audio +
hidden softAP upload mode) and the user's **esptool.py raw-flash** workflow.

## 1. ESP-IDF (C/C++) — OFFICIAL, canonical

- **Language**: C/C++ (CMake + FreeRTOS). **License**: Apache-2.0.
- **Status**: first-party Espressif; current stable v5.5.x (2025), v5.6/v6 dev.
  [github.com/espressif/esp-idf](https://github.com/espressif/esp-idf),
  [docs.espressif.com/projects/esp-idf](https://docs.espressif.com/projects/esp-idf).
- **esptool workflow** — emits `bootloader.bin`@0x1000,
  `partition-table.bin`@0x8000, `<app>.bin`@0x10000 (factory) + `.elf`/`.map`:

```bash
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32 && idf.py menuconfig && idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor   # drives esptool.py

# raw single-image flash (matches the user's workflow):
idf.py merge-bin -o merged.bin
esptool --chip esp32 merge_bin -o merged.bin --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0x10000 build/<project>.bin
esptool --chip esp32 write_flash 0x0 merged.bin
```

- **Partitions**: CSV-defined (`nvs`@0x9000, `phy_init`@0xe000, `factory`@0x10000;
  OTA adds `otadata`@0x2000 + `ota_0/1`). OTA payload = app-only image (not the
  merged factory image).
- **Fit**: ideal. Native SD/FAT, `led_strip`, ESP-ADF (I2S + ES8388), softAP +
  `esp_http_server` for upload mode. Custom work = CR95HF SPI driver + aw881xx
  I2C init only.
- **Gotchas**: Component Manager for deps; `idf.py partition-table` to inspect;
  `parttool.py` to read/write a single partition.

## 2. Arduino-esp32 + PlatformIO (C++)

- **Language**: C++ (Arduino core built on ESP-IDF). Core = `espressif/arduino-esp32`
  (10k+ stars, 3.x line). PlatformIO = most popular front-end.
  [registry.platformio.org/platforms/platformio/espressif32](https://registry.platformio.org/platforms/platformio/espressif32).
- **esptool workflow** — emits `bootloader.bin`@0x1000, `partitions.bin`@0x8000,
  `boot_app0.bin`@0xe000, `firmware.bin`@0x10000 under `.pio/build/<env>/`:

```bash
pio run -e <env> -t upload --upload-port /dev/ttyUSB0   # esptool, offsets via -v
pio run -e <env> -t uploadfs                             # SPIFFS/LittleFS image
esptool.py --chip esp32 merge_bin -o merged.bin --flash_mode dio --flash_freq 40m --flash_size 8MB \
  0x1000 .pio/build/<env>/bootloader.bin 0x8000 .pio/build/<env>/partitions.bin \
  0xe000 .pio/build/<env>/boot_app0.bin 0x10000 .pio/build/<env>/firmware.bin
```

- **Fit**: good-to-strong — SD_MMC, softAP + async web upload, I2S, TFT_eSPI /
  FastLED. **Custom work regardless of framework**: CR95HF (no mature Arduino
  lib) and the **AW88194 speaker amp** (no driver; Adafruit could not crack the
  init sequence — headphone via ES8156 works, speaker at-risk). TCAL6416 IO
  expander needs a small I2C driver.
- **Gotchas**: pin a core version (3.x broke LEDC/timer APIs vs 2.x); the Yoto
  module is **ESP32-WROVER-E, 8 MB flash** (`--flash_size 8MB`).

## 3. MicroPython / CircuitPython — EASIEST

- **Language**: Python 3 (C runtime). MicroPython = upstream; CircuitPython =
  Adafruit fork (USB CIRCUITPY drive + `code.py` auto-reload).
- **Status**: MicroPython ESP32 = mature/first-class. CircuitPython original
  ESP32 = labeled "beta" (S2/S3 stable), but **Adafruit publishes releases for
  the exact Yoto Player V3 + Yoto Mini 2024 boards** (10.2.1).
- **esptool workflow** — single combined `.bin` at `0x1000` (no merge_bin needed):

```bash
# CircuitPython (Yoto V3):
python -m esptool --chip esp32 --port PORT erase-flash
python -m esptool --chip esp32 --port PORT --baud 460800 write-flash -z 0x1000 \
  adafruit-circuitpython-yoto_player_v3-en_US-10.2.1.bin
# download: https://circuitpython.org/board/yoto_player_v3/
```

- **Fit**: the easiest path. The board build already auto-mounts SD and wires the
  I2C IO expanders, buttons, I2S, SPI display, and NFC pins. Native `audiomp3`,
  `framebufferio`, `socketpool` (softAP upload), `busio.UART/SPI/I2C`. Remaining
  = pure-Python drivers (ES8388/aw881xx I2C init, ht16d35x, CR95HF UART, HTTP
  upload endpoint).
- **Gotchas**: original-ESP32 port is "beta"; board def declares **8 MB QIO
  PSRAM** (so the Yoto V3 *does* have PSRAM — corrects the "no PSRAM" note
  elsewhere); no native USB → UART flashing only (exactly the esptool workflow).

## 4. Rust (esp-rs) — "official" but narrow

- **Language**: Rust. `esp-hal` (no_std) = **Espressif-OFFICIAL** (vendor-backed,
  1.0.0 Oct 2025, "first vendor-backed Rust SDK"); `esp-idf-hal` (std) =
  **community** ("Espressif puts little to no paid developer time in these").
- **Maturity**: esp-hal 1.0 stable surface is deliberately narrow (init, GPIO,
  UART, SPI, I2C, async/blocking, time, reset). **I2S, SDMMC, ADC, LEDC, RMT,
  USB, PSRAM, crypto are all `unstable`** and may break on minor bumps.
  esp-idf-hal 0.46.x wraps ESP-IDF drivers (mature coverage) but is pre-1.0,
  community, lags ESP-IDF.
- **esptool workflow**:

```bash
cargo install espup && espup install && source ~/export-esp.sh   # forked Xtensa rustc
cargo install esp-generate && esp-generate
cargo build --release && cargo espflash flash --release --monitor
cargo espflash save-image --chip esp32 --release --merge          # -> merged.bin
esptool.py --chip esp32 --port PORT write-flash 0x0 merged.bin
```

- **Honest fit verdict**: poor fit for THIS peripheral set in esp-hal (I2S codec,
  SDMMC+FAT, NFC, softAP are all custom/unstable); esp-idf-hal is the lower-risk
  Rust path but is community + pre-1.0. **The user's "Rust support is limited"
  observation is accurate.**
- **Version facts (crates.io, 2026-08-19)**: esp-hal 1.1.2, esp-wifi 0.15.1
  (frozen), esp-radio 0.18.0, esp-idf-hal 0.46.2.

## 5. Zephyr RTOS (and NuttX)

- **Language**: C (devicetree + Kconfig). **Status**: vendor-backed Espressif
  support (`hal_espressif`); NuttX ESP32 port also Espressif-maintained.
- **esptool workflow**: `west build -b esp32_devkitc/esp32 samples/hello_world`
  then `west flash` (shells out to esptool); also `esptool` on the emitted
  `.bin`s.
- **Fit**: viable but higher setup cost for a one-off. Native SPI/I2C/SDMMC+FAT/
  I2S/SoftAP. Overkill vs ESP-IDF for a single-device re-firmware.

## Primary sources

- https://github.com/espressif/esp-idf · https://docs.espressif.com/projects/esp-idf
- https://docs.espressif.com/projects/esptool/en/latest/
- https://github.com/espressif/arduino-esp32 · https://docs.platformio.org/en/latest/platforms/espressif32.html
- https://registry.platformio.org/platforms/platformio/espressif32
- https://circuitpython.org/board/yoto_player_v3/ · https://github.com/adafruit/Adafruit_CircuitPython_YotoPlayer
- https://micropython.org/ · https://circuitpython.org/
- https://github.com/esp-rs/esp-hal · https://github.com/esp-rs/esp-idf-hal
- https://docs.espressif.com/projects/rust/
- https://www.zephyrproject.org/ · https://docs.zephyrproject.org/
