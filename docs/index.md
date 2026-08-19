---
icon: lucide/radio
---

# open-yoto — Reverse Engineering

This project has three parts:

1. **[open-yoto firmware](open-yoto-firmware.md)** — a from-scratch, open-source
   firmware for the Yoto Player (C / ESP-IDF). It reads NFC cards, plays the
   matching audio + picture, and has an offline admin mode to load your own
   content. No Wi-Fi / Bluetooth / cloud unless admin mode is turned on.
2. **Reverse engineering** (this section) — a map of the *original* stock
   firmware in `yoto-firmware.bin` (the 8 MiB flash dump), documented below.
3. **[Research](ai/index.md)** — dense, evidence-backed detail (exact strings,
   function addresses, pin maps) for AI agents and tooling.

## Summary

| Attribute | Value |
|-----------|-------|
| **File** | `yoto-firmware.bin` — 8 MiB raw flash dump |
| **SoC** | Espressif **ESP32** (Xtensa LX6, dual-core, 240 MHz) |
| **Framework** | **ESP-IDF** + **ESP-ADF** (audio development framework) |
| **Chip ID** | `0x0000` (image header magic `0xE9`) |
| **App entry** | `0x400813a8` (IRAM) |
| **App image** | factory partition @ flash `0x40000`, 2.45 MB, 7 segments |

## Partition table

| Label | Type | Offset | Size |
|-------|------|--------|------|
| `nvs` | data / nvs | `0x009000` | 192 KB |
| `otadata` | data / ota | `0x039000` | 8 KB |
| `phy_init` | data / phy | `0x03b000` | 4 KB |
| **`factory`** | app | **`0x040000`** | 2.5 MB |
| `ota_0` | app / ota_0 | `0x2c0000` | 2.5 MB *(empty)* |
| `ota_1` | app / ota_1 | `0x540000` | 2.5 MB *(empty)* |

The device boots the `factory` image; both OTA slots are erased (`0xFF`).

## Memory map (app image)

| Segment | Load address | Size | Region |
|---------|--------------|------|--------|
| DROM | `0x3F400020` | 680 KB | flash-mapped read-only data |
| RTC_DRAM | `0x3FF80063` | 8 B | RTC fast data |
| DRAM | `0x3FFBDB60` | 30 KB | internal data RAM |
| IRAM | `0x40080000` | 9 KB | instruction RAM (entry) |
| IROM | `0x400D0020` | 1.63 MB | flash-mapped code |
| IRAM | `0x40082470` | 98 KB | instruction RAM |
| RTC_IRAM | `0x400C0000` | 100 B | RTC fast instructions |

## Peripheral inventory

| Function | Chip / driver | Interface |
|----------|---------------|-----------|
| NFC / RFID card reader | **ST CR95HF** | UART or SPI |
| SD card | **SDMMC** (1/4-bit) + **FatFS** | `d0..d3`, `clk`, `cmd` |
| Display | **HT16D35x** LED matrix / **GC9306** TFT | SPI (via IO expander CS) |
| Audio codec | **ES8388** / **aw881xx + ES8156** | I2S + I2C |
| Battery fuel gauge | **CW2215B** / CW2015 / ADC | I2C + ADC |
| Charger | **SGM41513** / SGM41511 / ETA6003 | I2C |
| USB-C PD sink | **HUSB238** | I2C |
| Qi wireless RX | **CV8013N** / CV8085 | I2C |
| Accelerometer | **LIS2DH12** | I2C |
| RTC | **IT8563** | I2C |
| Night light | **AW2028H** / direct GPIO | I2C / GPIO |
| IO expander | **PI4IOE5V6416** (×1 or ×2) | I2C |
| Light sensor | ADC (GPIO 36) | ADC1 CH0 |
| Temperature sensor | ADC (GPIO 39) | ADC1 CH3 |

## How to read this documentation

- **[Hardware & Ports](hardware.md)** — the complete GPIO/ADC/IO-expander pin
  map across six hardware revisions.
- **Subsystem pages** — one per peripheral, describing the driver, protocol,
  and how the firmware uses it.
- **[Methodology](methodology.md)** — the tooling and extraction pipeline used
  to produce these findings.
