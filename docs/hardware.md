---
icon: lucide/circuit-board
---

# Hardware & Ports

The firmware embeds **six complete hardware-configuration documents** as JSON
in the DROM (read-only flash data) segment. Each describes one hardware
revision of the device: every peripheral, its driver chip, and the exact
GPIO / ADC / IO-expander pin assignment. The firmware selects one at boot
based on the detected hardware.

The configs were recovered as standalone JSON objects from the app image and
are saved in `output/hwconfig_*.json` (see [Methodology](methodology.md)).

!!! note "Module & PSRAM"
    The Yoto V3 is an **ESP32-WROVER-E** module: 8 MB QIO flash **and 8 MB QIO
    PSRAM**. (Adafruit's CircuitPython board definition declares the same.)
    Adafruit has also already reversed this hardware — their
    `Adafruit_CircuitPython_YotoPlayer` library exists; their one known gap is
    the `aw881xx` speaker-amp init sequence, which remains uncracked (the
    headphone path via ES8156 works).

## Hardware revisions

| Config | Display | NFC | SD | Audio codec | Fuel gauge | Charger | Qi |
|--------|---------|-----|----|-------------|-----------|---------|-----|
| #00 | GC9306 (TFT) | UART | SDMMC 1-bit | aw881xx + ES8156 | CW2015 | ETA6003 | — |
| #01 | HT16D35x (LED) | **SPI** | **SPI** | **ES8388** (combined) | **ADC** | ETA6003 | — |
| #02 | HT16D35x (LED) | UART | SDMMC 1-bit | aw881xx + ES8156 | CW2015 | SGM41511 | CV8085 |
| #03 | *(none)* | — | — | — | — | — | — |
| #04 | GC9306 (TFT) | UART | **SDMMC 4-bit** | aw881xx + ES8156 | CW2215B | SGM41513 | — |
| #05 | HT16D35x (LED) | UART | SDMMC 1-bit | aw881xx + ES8156 | CW2215B | SGM41513 | CV8013N |

The `#00`–`#05` order is the order the configs appear in the image, not a
proven chronology. The lineage spans two display technologies (TFT vs LED
matrix), two NFC transports (SPI vs UART), two SD transports (SPI vs SDMMC
1/4-bit), and three charging stacks (USB-C-only, +Qi, and the earliest ADC
battery read).

### IOX factory direction/data (authoritative, from `output/hwconfig_*.json`)

Each config carries the stock firmware's IOX port defaults — **use these
exactly** when porting a revision; inferred values are wrong. Notably for
**#04** (single IOX, `ET6416`):

```json
"iox": { "type": "ET6416",
         "p0Dir": "0xB0", "p1Dir": "0xAF",
         "p0Data": "0x30", "p1Data": "0xEF" }
```

- **`levelconvertor` (IOX.0.3) is active-low**: `p0Data` bit 3 = 0 enables
  the display level shifter. Driving it high silently blocks the display
  SPI/backlight domain while everything else (I²C, audio, logs) keeps
  working — the classic "perfect logs, dead display" failure.
- `pwren` (IOX.1.4) is an output driven LOW; `vinhold` (IOX.1.6) an output
  driven HIGH; `pactrl` (IOX.0.6) an output driven LOW.
- TFT `cs`/`dc`/`reset` (IOX.0.0–0.2) default LOW.

## Pin-mapping notation

- `GPIO.n` — ESP32 general-purpose I/O pad `n`.
- `ADC.1.n` — ESP32 SAR ADC1 channel `n` (`ADC.1.0`=GPIO36, `ADC.1.3`=GPIO39,
  `ADC.1.7`=GPIO35 on this SoC).
- `IOX.p.n` — IO-expander **port `p`, bit `n`** (one or two expanders, via I2C).

## Latest 2-IOX revision (#05) — full pin map

This is the most complete config (fuel gauge, Qi, USB-C, IR temp sensor), using
two `PI4IOE5V6416` expanders.

### ESP32 GPIO

| Pin | Function(s) |
|-----|-------------|
| GPIO 0  | I2S `mclk` (audio master clock) |
| GPIO 2  | SDMMC `d0` |
| GPIO 4  | Rotary encoder 1 `pin_b` |
| GPIO 5  | I2S `bclk` (audio bit clock) |
| GPIO 13 | Rotary encoder 0 `pin_b` |
| GPIO 14 | SDMMC `clk` |
| GPIO 15 | SDMMC `cmd` |
| GPIO 18 | I2S `lrclk` (audio frame sync) |
| GPIO 19 | I2S `out` (audio data) |
| GPIO 21 | I2C `sda` |
| GPIO 22 | SPI `mosi` |
| GPIO 23 | SPI `sclk` |
| GPIO 25 | I2C `sck` |
| GPIO 26 | Rotary encoder 0 `pin_a` + SPI `miso` (shared) |
| GPIO 27 | Rotary encoder 1 `pin_a` |
| GPIO 32 | NFC `rx` (CR95HF UART) |
| GPIO 33 | NFC `tx` (CR95HF UART) |
| GPIO 34 | IO expander interrupt `ioxInt` |
| GPIO 35 | IR receiver temp sensor (`qirxtempsensor`) + ADC1 CH7 |
| GPIO 36 | Light sensor + ADC1 CH0 |
| GPIO 39 | Temperature sensor (NTC) + ADC1 CH3 |

### IO expander (×2 PI4IOE5V6416, I2C)

| Pin | Function(s) |
|-----|-------------|
| IOX 0.0 | Qi `qii2cint` |
| IOX 0.1 | RTC interrupt `rtcint` |
| IOX 0.4 | Encoder 1 push button |
| IOX 0.5 | Encoder 0 push button |
| IOX 0.6 | Battery alert `battalrt` |
| IOX 0.7 | Qi status `nqistat` |
| IOX 1.0 | USB-C VBUS status `nvbusstat` |
| IOX 1.1 | Headphone detect `hpdetect` |
| IOX 1.2 | Accelerometer tilt interrupt `tiltind` |
| IOX 1.3 | **Power button** |
| IOX 1.4 | Charger status `chgstat` |
| IOX 2.0–2.3 | Display chip selects `csn0..csn3` (HT16D35x) |
| IOX 2.4 | Audio amp control `pactrl` |
| IOX 2.5 | Power enable `pwren` |
| IOX 2.6 | Qi charge enable `nqichgen` |
| IOX 2.7 | USB-C charge enable `nvbuschgen` |
| IOX 3.0 | Level convertor enable |
| IOX 3.1 | Vin hold `vinhold` |
| IOX 3.3 | Vout enable `vouten` |
| IOX 3.5 | Qi enable 5 W `qien5w` |

### I2C addresses (split-audio revisions)

| Device | Address (#00/#04) | Address (#02/#05) |
|--------|-------------------|-------------------|
| Headphone DAC (`es8156`) | `0x08` | `0x09` |
| Speaker amp L (`aw881xx`) | `0x34` | `0x34` |
| Speaker amp R (`aw881xx`) | `0x34` | `0x37` |
| RTC (`it8563`) | `0x51` | `0x51` |

## SD card transport

| Config | Transport | Pins |
|--------|-----------|------|
| #01 | **SPI** (`sd.type` = `"spi"`, ESP-IDF `sdspi` driver) | `cs`=GPIO15, `sclk`=GPIO14, `mosi`=GPIO22, `miso`=GPIO34 |
| #00/#02/#05 | SDMMC **1-bit** (`"sd1"`) | `clk`=GPIO14, `cmd`=GPIO15, `d0`=GPIO2 |
| #04 | SDMMC **4-bit** (`"sd4"`) | + `d1`=GPIO4, `d2`=GPIO12, `d3`=GPIO13 |

## Audio I2S pins (revision-dependent)

| Config | `mclk` | `bclk` | `lrclk` | `out` |
|--------|--------|--------|---------|-------|
| #01 (combined ES8388) | GPIO0 | GPIO25 | GPIO32 | GPIO33 |
| #00/#02/#04/#05 (split) | GPIO0 | GPIO5 | GPIO18 | GPIO19 |

`pactrl` (amplifier power control) is `IOX.0.6` on #00/#01/#04 and `IOX.2.4` on
#02/#05.

## NFC transport

| Config | Transport | Pins |
|--------|-----------|------|
| #01 | **SPI** | `mosi`=GPIO18, `miso`=GPIO21, `sclk`=GPIO2, `cs`=IOX1.4, `irqin`=IOX0.7, `irqout`=IOX1.0 |
| #00/#02/#04/#05 | **UART** | `rx`=GPIO32, `tx`=GPIO33 |

## Battery / charger specifics

- **Fuel gauge**: `CW2215B` (#04/#05) and `CW2015` (#00/#02), both I2C, with a
  `battalrt` alert line (`IOX.0.6` on 2-IOX boards, `IOX.1.0` on single-IOX).
  The earliest config (#01) reads VBAT via ADC (`ADC.1.3`).
- **Chargers**: `ETA6003` (#00/#01), `SGM41511` (#02), `SGM41513` (#03/#04/#05).
  Status lines vary: `chgstat` is `IOX.1.4` (2-IOX) or `IOX.1.7` (single-IOX),
  with `plugstat` and `chgbst` on some variants.
- **USB-C**: `HUSB238` PD sink controller — negotiates 5/9/12/15 V, signals
  VBUS status (`nvbusstat`) and charge-enable (`nvbuschgen`) via IOX.
- **Qi**: `CV8013N` (#05, with 5 W-en `qien5w`=IOX3.5, `qii2cint`=IOX0.0,
  I2C addr null) and `CV8085` (#02, `i2caddr`=`0x13`, no 5 W-en). Status
  `nqistat` and charge-enable `nqichgen` via IOX.
- **Battery profiles** are matched by `btype` string against
  `batt_profiles[]` in `batt_profile.c`. Five profiles appear in the image:
  `JY734352`, `AS-R18650-2600-112`, `LJDX30X-4500`, `UTL-FD70X-2000`, plus one
  more (see `output/hwconfig_*.json`).

## Rotary encoders

Two quadrature rotary encoders (volume / selection knobs), each with a push
button:

| Encoder | pin_a | pin_b | push |
|---------|-------|-------|------|
| 0 | GPIO 26 (or 5, 35) | GPIO 13 (or 36, 39) | IOX 0.5 |
| 1 | GPIO 27 (or 4) | GPIO 4 (or 36) | IOX 0.4 |

The encoder pins changed across revisions; the two-knob layout is consistent.
