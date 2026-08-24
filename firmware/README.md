# Yoto Firmware — ESP-IDF skeleton

An ESP-IDF project skeleton for the Yoto Player (ESP32), with the correct pin
map and per-peripheral init for the onboard battery, display, NFC reader, and
speaker. Pin assignments are recovered from the stock firmware's embedded
hardware config — see [`components/board/board_pins.h`](components/board/board_pins.h)
and the project's `docs/` for the full reverse-engineering write-up.

## Build

> **Note on `make`**: the GNU-Make workflow (`make menuconfig && make flash`)
> was **removed in ESP-IDF v4/v5**. The current build command is `idf.py`, which
> drives CMake + Ninja. This project targets ESP-IDF **v5.x** (`idf.py`).

### One-time setup (macOS)

```bash
# 1. Build prerequisites (cmake/ninja/dfu-util). Python is managed by uv below,
#    NOT Homebrew.
brew install cmake ninja dfu-util

# 2. uv-managed Python 3.12 (ESP-IDF needs Python 3.10+; 3.12 is a safe choice)
uv python install 3.12

# 3. Clone ESP-IDF v5.5 and install its tools, using uv's Python
git clone --depth 1 --recursive --shallow-submodules -b release/v5.5 \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf
export PATH="$(dirname "$(uv python find 3.12)"):$PATH"   # uv Python first
~/esp/esp-idf/install.sh esp32
```

### Build

```bash
. ~/esp/esp-idf/export.sh     # adds IDF_PATH + toolchain to PATH
cd firmware
idf.py set-target esp32
idf.py build
```

### Flash

`idf.py` drives esptool.py at the correct offsets, or you can raw-flash a single
merged image (the esptool workflow):

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# ...or a single merged image via esptool.py:
idf.py merge-bin -o merged.bin
esptool.py --chip esp32 --flash_size 8MB write_flash 0x0 merged.bin
```

The build emits `build/bootloader/bootloader.bin` (flashed at `0x1000`),
`build/partition_table/partition-table.bin` (`0x8000`), and `build/yoto.bin`
(`0x10000`). Flash size is 8 MB (ESP32-WROVER-E).

### Linux UART automation

The board's USB-UART controls are wired as `RTS -> BOOT/GPIO0` and
`DTR -> RESET/EN`, rather than the conventional Espressif mapping. The checked
in `sdkconfig` sets esptool to `--before=no_reset --after=no_reset`; manual
control is required.

From the repository root, source ESP-IDF and run:

```bash
export PATH="$HOME/.espressif/python_env/idf5.5_py3.12_env/bin:$(dirname "$(uv python find 3.12)"):$PATH"
. "$HOME/.esp/esp-idf/export.sh"
python3 tools/flash_esp32.py
```

The tool builds, waits for esptool's connection phase, enters the ROM
bootloader using the RTS/DTR sequence, flashes, resets into the application,
and writes the UART log to `/tmp/yoto_scan.log`. Do not replace
`tools/bootloader_esp32.py` with a pyserial script: opening pyserial changes
the Linux TTY's termios configuration while esptool is connected. The helper
uses modem-control ioctls only, preserving esptool's settings.

## Boot test firmware

A minimal bring-up firmware (`main/test_main.c`) that skips NFC / SD / encoder
and, on boot, just:

- **rev #05** (default): displays a border + cross on the 16×16 LED panel;
- **rev #04** (`CONFIG_BOARD_REV_04=y`): PWM the TFT backlight (GPIO26 via
  LEDC, **40 kHz** — the stock frequency from the factory image; the panel's
  LED rail is AC-coupled, so a plain GPIO high passes nothing and a lower
  PWM frequency is attenuated, leaving the display dim enough that colours
  wash out) and draw a green/blue test pattern on the GC9306 TFT;
- emits a repeating **1 kHz** beep over the shared I²S bus to the AW88194A
  speaker amp and ES8156 headphone DAC.

This proves the rev #04 power/reset, I²C, display, and complete audio path.

Enable it in `idf.py menuconfig` (top-level **"Build the boot test firmware"**,
from `main/Kconfig.projbuild`), or directly:

```bash
echo 'CONFIG_APP_TEST_MODE=y' >> sdkconfig && idf.py build
```

Rev #04 bring-up notes (verified against a physical unit and stock app):

- The GC9306 driver (`components/gc9306/`) follows the stock electrical
  setup: SPI2 host 1, mode 0, 80 MHz, `SPI_DEVICE_NO_DUMMY`, manual CS,
  queue depth 1, stock reset, the full F2 tail, DC-low init groups, and
  1364-pixel queued RGB666 transfers. Pixel RGB bytes are forwarded
  unchanged — there is no channel transform.
- The IOX first receives the exact `hwconfig_04` latch/direction bytes
  (`p0Dir=0xB0 p1Dir=0xAF p0Data=0x30 p1Data=0xEF`). Stock `app_main` then
  drives VINHOLD HIGH, PWREN LOW, and `levelconvertor` (IOX.0.3) HIGH.
- `analysis/extract_stock_battery_icon.py` recovers the exact low-SOC
  battery_ui asset (table ID 10, PNG `0x3F468D61`) as an analysis artifact. Its
  segmented white-on-black appearance is the stock low-SOC visual, not a
  guessed outline. The runtime battery screen no longer compiles that frame in:
  it renders `firmware/icons/battery-*.png` through the same alpha-premultiplied
  12× window `(24,27)..(215,218)`.

Back to the normal firmware:

```bash
sed -i '' 's/^CONFIG_APP_TEST_MODE=.*/CONFIG_APP_TEST_MODE=n/' sdkconfig && idf.py build
```

## Structure

```
firmware/
├── CMakeLists.txt            # top-level (idf.py / CMake)
├── sdkconfig.defaults        # default Kconfig
├── main/
│   ├── app_main.c            # normal firmware: boot + init + main loop
│   ├── test_main.c           # boot test firmware (display + beep)
│   ├── boot_face_rgba.h      # generated: face-01..face-08 animation frames
│   ├── wink_face_rgba.h      # generated: the two wink faces
│   ├── battery_icons_rgba.h  # generated: charging, 10..100, empty icons
│   ├── Kconfig.projbuild     # CONFIG_APP_TEST_MODE switch
│   └── CMakeLists.txt
├── test/host/                # host tests (no hardware): volume-bar geometry
    ├── components/
    ├── board/board_pins.h    # THE pin map + I2C addresses (authoritative)
    ├── iox/                  # IO expander (buttons, display CS, power/amp ctrl)
    ├── battery/              # CW2215B fuel gauge + SGM41513 charger
    ├── lis2dh12/             # stock 0x18 accelerometer startup
    ├── display/              # HT16D35x 16x16 LED matrix (SPI + IOX CS)   [rev #05]
    ├── gc9306/               # GC9306 TFT driver (stock init + 18-bit)    [rev #04]
    ├── nfc/                  # ST CR95HF over UART
    └── audio/                # I2S + ES8156 / aw881xx codec
```

## What's implemented vs TODO

**Implemented** (real bus + peripheral setup, compiles against ESP-IDF v5):

- I2C master (SDA=21, SCL=25) + IO expander(s) with exact stock latch and
  direction bytes, followed by the recovered run-state transitions (rev #04:
  VINHOLD HIGH, active-low PWREN LOW, levelconvertor IOX.0.3 HIGH).
- **GC9306 TFT driver [rev #04]**: stock SPI2 mode 0/80MHz/no-dummy setup,
  stock reset and complete initialization tail, queue-1 1364-pixel RGB666
  transport, CASET/RASET/RAMWR framing, and 40 kHz GPIO26 backlight.
  `gc9306_draw_rgba16()` alpha-composites and 12× scales into the calibrated
  `(24,24)..(215,215)` test window. Browser-generated OYIM images retain
  16×16 RGB565 color and use the same 12× geometry; older 64×64 OYIM files
  remain readable through a 3× compatibility path. Both compensate the
  physical horizontal mirror and correct the observed R/B RGB666 swap at the
  panel boundary.
- **Boot faces, winks, and battery icons**: `icons/face-01.png`…`face-08.png`
  play at 16 fps and rest on `face-08`, the idle face; turning the right knob
  with no card loaded winks between `icons/face-wink-01.png` and
  `face-wink-02.png`. The battery screen uses `icons/battery-charging.png`,
  `icons/battery-10.png`…`battery-100.png` (SOC floored to ten), and
  `icons/battery-empty.png` below 10%. All three sets are compiled in as
  `main/boot_face_rgba.h`, `main/wink_face_rgba.h` and
  `main/battery_icons_rgba.h` by `tools/png_to_rgba_header.py`, which records
  each PNG and RGBA SHA-256 so a header can be audited against its icon.
  `test/host/run.sh` checks the volume-bar geometry and its draw-call budget on
  the host.
- Stock-matched I2S TX (APLL, mclk=0, bclk=5, lrclk=18, dout=19,
  44100 Hz, 16-bit mono-left) with bounded PCM gain, stereo downmix, decoded
  source-metadata propagation, and streaming 8–96 kHz resampling. Content
  sniffing selects MP3, standalone ADTS AAC, or streamed M4A/AAC-LC sample
  tables (`stsd`/`stsz`/`stsc`/`stco`/`co64`).
  TX descriptors auto-clear after transmission; stop/end transitions reset the
  complete cyclic DMA ring to silence before another playback starts.
- WROVER-E PSRAM backs allocations larger than 4 KiB, including the
  playback-scoped decoder heaps; 32 KiB of internal memory remains reserved
  for DMA/internal-only use.
- **AW88194A speaker amp [rev #04]**: factory reset timing, five ID retries,
  exact 35-entry register table, recovered SmartK firmware and mono config,
  VCALB, DSP/I²S/PLL validation, interrupts, start, and hard-unmute.
- **SD startup**: stock SDMMC width flags, 40 MHz clock, 40 mA CLK drive,
  five mount attempts, and FatFS at `/sdcard`. `mapping.json` is optional and
  never gates a successful mount.
- **Stock welcome**: the exact 18,608-byte oracle M4A asset is embedded once,
  exposed read-only as `/system/sounds/welcome` by `YOTO_VFS`, parsed as one
  AAC-LC/44.1 kHz/mono MP4 track, decoded, and sent through the shared I2S path
  after the SD mount.
- CR95HF on UART1, ESP TX=GPIO32, ESP RX=GPIO33, 57600 8-N-2, with the
  stock control-byte and Echo synchronization sequence. Admin-mode scans
  capture UID/URL without playback; authenticated writes compare the expected
  UID and verify URL read-back under one UART transaction.
- Boot-enabled `openyoto` SoftAP and HTTP server with a random six-character
  alphanumeric code. Its responsive UI provides remote player controls,
  `/sdcard/media`-confined file CRUD, captured-card read/write, mapping
  inspection, whole-folder track upload with browser-side PNG/JPEG conversion,
  and a lazy recursive track catalog.
- Admin memory is request-driven: default-directory listing at login,
  per-directory streamed JSON, tab-scoped card/track loading, lazy card-index
  parsing, playback-scoped decoder state, a 16 KiB HTTP task stack, and a
  temporary access-code raster.

**TODO**:

- HT16D35x frame format + 6-bit-gray fan-out [rev #05].
- AW88194A runtime headphone insertion/removal routing and per-path volume
  curves; cold-start SmartK data and speaker activation are implemented.
- Translation of stock `/sdcard/cards` metadata into open-yoto playlists.

## Hardware revision caveat

The Yoto shipped several hardware revisions with **different pins**. This
skeleton targets the latest revision (2× IO expander, HT16D35x LED matrix,
UART NFC, SDMMC 1-bit). If your unit differs, edit
`components/board/board_pins.h` — the variant table is in `../docs/hardware.md`.
