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
- **PSRAM**: the Yoto V3 board def declares 8 MB QIO PSRAM (ESP32-WROVER-E
  module) — so PSRAM *is* available on the V3 (verify per unit). Keep buffers
  small anyway (16×16 framebuffer = 192 bytes) for portability to PSRAM-less
  revisions.

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

### Upload mode (softAP + HTTP) — the in-place edit path

Since the ESP32 has no native USB and Wi-Fi is normally absent, add a hidden
upload mode that briefly enables the radio:

- **Gate Wi-Fi behind a cargo feature** (`upload-mode`) so the normal build has
  zero radio code/flash cost.
- **Trigger**: all three buttons (encoder pushes IOX0.5/IOX0.4 + power IOX1.3)
  held simultaneously ~400 ms. Poll the PI4IOE5V6416 input ports at a 5 ms tick
  (interrupts unnecessary); one [`debouncr`](https://docs.rs/debouncr/) per
  button; a chord state machine (all-released → all-pressed → settle
  300–500 ms → toggle). Configure pull-ups (reg `0x46`/`0x47` enable,
  `0x48`/`0x49` select) on the button bits; seed state from a first read and
  require a released→pressed transition so it never fires at boot. Match the
  exact 3-bit mask to avoid colliding with the power-button long-press.
- **AP**: `esp-radio` (renamed from `esp-wifi` in esp-hal 1.x) using the
  `embassy_access_point` recipe — `Interface::access_point()` + `WifiController`
  `Config::AccessPoint(AccessPointConfig{ssid:"YotoUpload", ..})` +
  `embassy_net::new` static `192.168.4.1/24` + `edge_dhcp` server (phones
  auto-join).
- **HTTP server**: [`edge-http`](https://docs.rs/edge-http/) 0.8 (no_std,
  no_alloc, `Handler` trait) behind `edge-nal-embassy`, using
  `run_with_socket_queue()` (embassy-net has no accept queue). Alternative:
  [`picoserve`](https://docs.rs/picoserve/) (axum-style router, JSON extractors,
  higher MSRV 1.93 vs edge-http 1.88).
- **Endpoints**:
  - `GET /mapping.json` — read SD, stream back
  - `PUT /mapping.json` — raw JSON body → validate length → parse
    (`serde_json_core`/`serde-json-core`) → write SD
  - `PUT /media/<name>` — raw `application/octet-stream` body streamed in fixed
    chunks to `embedded-sdmmc::File::write` (never buffer a whole file — no PSRAM)
  - `GET /` — tiny status page
- **No multipart needed** — raw JSON + raw binary PUT is simpler and supported
  by both edge-http and esp-idf-svc.
- **Teardown**: `controller.stop()` + drop server/stack on the same chord or a
  ~5-min timeout; show an indicator on the 16×16 matrix while up.

### SD layout

```text
/mapping.json               # NFC UID -> media binding (edited over upload mode)
/media/<name>.wav           # 16-bit PCM, 22.05/44.1 kHz
/media/<name>.bmp           # 16x16 (or 240x320 for TFT)
```

```json
{"cards": {"04A1B2C3D4E5F0": {"sound": "media/a.wav", "image": "media/a.bmp"}}}
```

## 5. NFC → content mapping

- Read the card **UID** (7 bytes) → hex string → look up `cards[uid]` in
  `mapping.json` → get `sound` + `image` paths under `/media/`.
- Key purely on UID (no NDEF write needed); the CR95HF can also write a custom
  NDEF URI if you prefer URL-keyed lookups.

## 6. Phased plan

1. `esp-generate` scaffold + LED blink + `defmt`/`log` over UART.
2. SD read: mount FAT32, parse `mapping.json`.
3. NFC read: poll CR95HF, read UID → resolve media paths.
4. Audio + display: WAV via I2S DMA, 16×16 image render.
5. Upload mode: 3-button chord → softAP + `edge-http` edit/upload.
6. Polish: encoders, power handling, fuel gauge (CW2215B over I2C, optional).

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
- `esp-wifi` was renamed **`esp-radio`** in esp-hal 1.x; it requires esp-hal's
  `unstable` feature (not SemVer-stable) — pin with `~` and keep `unstable` only
  in the `upload-mode` build.
- `edge-http` (MSRV 1.88) vs `picoserve` (MSRV 1.93) — pick to match your esp-hal
  MSRV; both are 0.x.
- Use `edge-http`'s `run_with_socket_queue()` (not `run()`) with embassy-net —
  smoltcp has no accept queue.
- Stream uploads in fixed chunks to SD — never buffer a whole file (no PSRAM).
- The old `docs.esp-rs.org` domain is now serving an unrelated site; use
  `docs.espressif.com/projects/rust/`.

## 8. Primary sources

- https://github.com/esp-rs/esp-hal · https://github.com/esp-rs/esp-template
- https://github.com/esp-rs/esp-hal/blob/main/examples/wifi/embassy_access_point/src/main.rs
- https://docs.rs/esp-radio/ · https://docs.rs/embassy-net/
- https://docs.rs/edge-http/ · https://docs.rs/picoserve/ · https://docs.rs/edge-nal-embassy/
- https://docs.rs/debouncr/ · https://docs.rs/serde-json-core/
- https://docs.rs/st25r95/ · https://docs.rs/nfc_forum_tags/ · https://docs.rs/ndef/
- https://docs.rs/embedded-sdmmc/ · https://docs.rs/embedded-hal-bus/
- https://docs.rs/mipidsi/ · https://docs.rs/minipng/ · https://docs.rs/tinybmp/
- https://docs.rs/esp-hal/latest/esp_hal/i2s/master/index.html
- https://github.com/hi-squeaky-things/es8388_driver_rust
- https://github.com/espressif/esp-adf/tree/master/components/audio_hal/driver/es8156
- https://docs.rs/nanomp3/ · https://docs.rs/rmodem/ · https://docs.rs/xmodem/
- https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- https://github.com/esp-rs/esp-idf-hal/issues/231
