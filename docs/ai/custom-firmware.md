---
icon: lucide/bot
---

# Custom Rust Firmware — AI Reference

Dense, source-grounded reference for writing a replacement Rust firmware for
the Yoto Player (ESP32, Xtensa LX6). Goals: **no Wi-Fi/BT** (guaranteed) and
**load custom audio+images onto the SD, bound to custom NFC cards**.

## 1. Hard constraints (drive every decision)

- **Original ESP32 has NO native USB** — no USB OTG peripheral/PHY, no D+/D-
  pins. Dev-board "USB" is always an external USB-UART bridge. USB Mass
  Storage *device* mode is **impossible on this silicon** without an external
  USB device-controller IC. ([datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf))
- The Yoto's USB-C port is **power-only**: HUSB238 is a PD *sink*, CV8013N is a
  Qi *RX*. No USB data reaches the ESP32.
- **No hardware FPU** on Xtensa LX6 → software MP3 decode stutters. Pre-convert
  audio to 16-bit PCM WAV on the host.
- **No PSRAM** assumed → keep buffers small (16×16 framebuffer = 192 bytes;
  audio double-buffer of a few KB).

## 2. Stack decision

Two mutually-exclusive Rust stacks; pick one.

| | `esp-hal` (no_std) | `esp-idf-hal` (std) |
|---|---|---|
| Maintenance | Espressif-official, semver-stable | community, "lags ESP-IDF" |
| Radio | **opt-in** (never link `esp-wifi`/`esp-radio`) → no RF in binary | IDF-linked; strip via `sdkconfig` |
| SD | `embedded-sdmmc` over SPI (mature) | `esp-idf-svc` FatFs + `std::fs` |
| I2S | DMA-backed but `unstable`/partial on ESP32 | mature IDF "standard-mode" driver |
| Rotary encoder | GPIO intr / RMT (partial) | PCNT example |
| Binary size | smallest | larger |

**Recommendation**: start `no_std` `esp-hal` (target `xtensa-esp32-none-elf`,
MSRV 1.88, scaffold via `esp-generate`) because "no Wi-Fi" is then guaranteed
architecturally and it's the officially-supported stack. Fall back to
`esp-idf-hal` **only** if I2S/SD prove too immature — accept the larger binary
and radio-strip via sdkconfig.

## 3. Toolchain

```bash
# one-time: install the esp-rs toolchain + Xtensa target
cargo install espup
espup install            # sets up Rust nightly + xtensa-esp32-none-elf target
cargo install espflash cargo-generate

# scaffold
cargo generate esp-rs/esp-template --name yoto-rs
# then edit Cargo.toml: target = "xtensa-esp32-none-elf", features = ["esp32"]

# build/flash/monitor
cargo run --release          # via espflash runner
espflash flash --monitor
```

Sources: [esp-hal](https://github.com/esp-rs/esp-hal),
[esp-template](https://github.com/esp-rs/esp-template),
[esp-generate](https://github.com/esp-rs/esp-generate),
[espup](https://github.com/esp-rs/espup).

## 4. Component drivers

### NFC — CR95HF = ST25R95

- **Crates**: [`st25r95`](https://docs.rs/st25r95/) (transceiver, `send_receive`),
  [`nfc-forum-tags`](https://docs.rs/nfc_forum_tags/) (Type 2 tag `T2TReader` /
  NDEF read+write), [`ndef`](https://docs.rs/ndef/) (URI encode/decode).
- **Prefer SPI** so `st25r95` works directly. The stock Yoto config #01 already
  exposes the CR95HF on SPI (`cs`=IOX1.4, `mosi`=GPIO18, `miso`=GPIO21,
  `sclk`=GPIO2); newer configs use UART (GPIO32/33).
- **UART path** (no crate): implement CR95HF framing (SOF `0x00` + LEN + CMD +
  DATA + EOF `0x00`, 57600 8-N-1) over `esp_hal::uart::Uart`; minimum commands
  `Idn`/`ProtocolSelect`/`SendRecv`/`Idle`.
- **Activation** (ISO 14443-3A): REQA `0x26` → ATQA; anti-collision
  `0x93 0x20` → UID+BCC (handle cascade for 7/10-byte UIDs); select `0x93 0x70`
  → SAK; then `T2TReader` for NDEF read/write.
- **License**: the Foundation Devices stack is GPL-3.0-or-later — decide up
  front.

### SD card + FAT

- **Primary**: [`embedded-sdmmc`](https://docs.rs/embedded-sdmmc/) (FAT16/32,
  no_alloc) over **SPI** via
  [`embedded-hal-bus::spi::ExclusiveDevice`](https://docs.rs/embedded-hal-bus/),
  plus an esp-hal `Delay` and a small `TimeSource` impl.
  `VolumeManager::new(sd, timesource)` → `open_volume(VolumeIdx(0))` →
  `open_root_dir()` → `File::read`/`write` (embedded-io).
- **SDMMC 4-bit**: esp-hal `sdmmc` is unstable/partial; escalate only if SPI
  isn't wired to the slot. Needs a hand-written `BlockDevice` + card init.
- **`fatfs`** only if you need LFN write/rename/mkdir/format (needs `alloc`).
- Format FAT32 on a PC first.

### Display

- **HT16D35x (16×16 LED matrix)**: no crate — write a no_std driver.
  Implement `embedded-graphics-core` `DrawTarget` + `OriginDimensions` over a
  custom 6-bit-gray color type. Maintain a 256×6-bit framebuffer; threshold
  decoded RGBA to gray; fan out each 8×8 quadrant to its `csn0..3` chip.
  Reference [`ht16k33`](https://docs.rs/ht16k33/) for structure.
- **GC9306 (TFT)**: add a `Model` impl to
  [`mipidsi`](https://docs.rs/mipidsi/) (DCS-compatible) using the vendor init
  sequence; drive over `display-interface-spi` (SPI + DC + CS + reset). Blit
  RGB565 via batched window writes.
- **Images**: `tinybmp`/`tinytga` (zero-dep) or
  [`minipng`](https://docs.rs/minipng/)/`nopng` for PNG; downscale to 16×16.

### Audio

- **I2S**: esp-hal `I2s::new(periph, dma, Config)` +
  `Config::new_tdm_philips().with_sample_rate(44_100.Hz()).with_channels(2)
  .with_data_format(DataFormat::Data16Channel16)`; `I2s::with_mclk(pin)` for
  MCLK; `I2sTx::write_dma_circular` for gapless double-buffered playback.
  (esp-idf-hal equivalent: `I2sDriver::new_std_tx()` + `StdConfig`.)
- **Codec ES8388**: [`es8388_driver_rust`](https://github.com/hi-squeaky-things/es8388_driver_rust)
  (no_std, but **embedded-hal 0.2** — needs a shim against esp-hal 1.x eh1.0, or
  esp-hal's `embedded-hal-02` feature). Slave-I2S only; ESP32 drives BCLK/LRCK.
  Porting ref: [`es8311-rs`](https://github.com/zRedShift/es8311-rs).
- **aw881xx + ES8156**: **no Rust driver**. ES8156 (I2C addr 0x18) port from
  [ESP-ADF es8156](https://github.com/espressif/esp-adf/tree/master/components/audio_hal/driver/es8156);
  aw881xx (AW88298) from the [Awinic datasheet](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/K128%20CoreS3/AW88298.PDF).
- **Decode**: prefer 16-bit PCM WAV (parse RIFF/fmt/data by hand, or
  [`hound`](https://docs.rs/hound/) on std). MP3 only via
  [`nanomp3`](https://docs.rs/nanomp3/) (pure-Rust no_std minimp3 port) — but
  no FPU, so CPU-heavy. `symphonia`/`awedio`/`awedio_esp32` are the std/ESP-IDF
  path (`mp3 may not work well on ESPs without native FPU`).

### Content loading (the "push to SD" problem)

Given no native USB + no Wi-Fi:

1. **Removable SD (primary)** — eject, copy on PC, reinsert. Zero transfer
   firmware.
2. **Serial XMODEM/YMODEM** — expose a UART (the CR95HF UART GPIO32/33, or the
   programming UART); firmware runs [`rmodem`](https://docs.rs/rmodem/)
   (XMODEM/1k/CRC; AGPL-3.0) or [`xmodem`](https://docs.rs/xmodem/)
   /[`xymodem.rs`](https://github.com/TGMM/xymodem.rs); PC side `sz --ymodem`.
   Baud-limited, not drag-and-drop.
3. **USB MSC device mode — NOT possible** on this silicon. `usbd-mass-storage`
   is immature + pins `usb-device 0.2.4` (esp-hal ≤0.23 uses 0.3.x, 1.x uses
   embassy-usb which has **no MSC device class**; `embassy-usb-msd` is an empty
   name reservation). Only S2/S3 have OTG, and esp-idf-hal doesn't wrap tinyusb
   ([issue #231](https://github.com/esp-rs/esp-idf-hal/issues/231)).

### SD layout

```text
/cards/<uid>/sound.wav     # 16-bit PCM, 22.05/44.1 kHz
/cards/<uid>/image.bmp     # 16x16 (or 240x320 for TFT)
```

## 5. NFC → content mapping

- Read the card **UID** (7 bytes) → hex string → directory `/cards/<uid>/`.
- Simplest: key purely on UID; no NDEF write needed. Alternatively write a
  custom NDEF URI and parse it. The CR95HF can write blank Type 2 tags.

## 6. Phased plan

1. `esp-generate` scaffold + LED blink + `defmt`/`log` over UART.
2. SD read: mount FAT32, list `/cards/`.
3. NFC read: poll CR95HF, read UID.
4. Audio: WAV from SD → I2S DMA → codec.
5. Display: render 16×16 image from SD.
6. NFC write: program blank cards.
7. Polish: encoders (GPIO intr), power button, fuel gauge (CW2215B over I2C,
   optional).

## 7. Gotcha summary

- Pick ONE stack; don't mix `esp-hal` and `esp-idf-hal`.
- `usbd-mass-storage` won't compile against esp-hal OTG (usb-device 0.2 vs 0.3
  vs embassy-usb); embassy has no device MSC. Don't build on it.
- No Rust driver for aw881xx/ES8156; ES8388 driver is eh0.2 + immature.
- I2S data format must match codec (Philips/I2S 16-bit) or you get noise.
- ESP32 no FPU → avoid MP3; use WAV/PCM.
- `es8388_driver_rust` is slave-I2S only; ESP32 must emit BCLK/LRCK (MCLK
  optional via `with_mclk`).
- Yoto USB-C is charge-only; serial upload needs the UART pins.
- GPL-3.0 (NFC stack) and AGPL-3.0 (`rmodem`) license obligations.

## 8. Primary sources

- https://github.com/esp-rs/esp-hal · https://github.com/esp-rs/esp-template
- https://docs.rs/st25r95/ · https://docs.rs/nfc_forum_tags/ · https://docs.rs/ndef/
- https://docs.rs/embedded-sdmmc/ · https://docs.rs/embedded-hal-bus/
- https://docs.rs/mipidsi/ · https://docs.rs/minipng/ · https://docs.rs/tinybmp/
- https://docs.rs/esp-hal/latest/esp_hal/i2s/master/index.html
- https://github.com/hi-squeaky-things/es8388_driver_rust
- https://github.com/espressif/esp-adf/tree/master/components/audio_hal/driver/es8156
- https://docs.rs/nanomp3/ · https://docs.rs/awedio_esp32/
- https://docs.rs/rmodem/ · https://docs.rs/xmodem/
- https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- https://github.com/esp-rs/esp-idf-hal/issues/231
