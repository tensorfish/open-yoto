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

## Boot test firmware

A minimal bring-up firmware (`main/test_main.c`) that skips NFC / SD / encoder
and, on boot, just:

- **rev #05** (default): displays a border + cross on the 16×16 LED panel;
- **rev #04** (`CONFIG_BOARD_REV_04=y`): PWM the TFT backlight (GPIO26 via
  LEDC, **40 kHz** — the stock frequency from the factory image; the panel's
  LED rail is AC-coupled, so a plain GPIO high passes nothing and a lower
  PWM frequency is attenuated, leaving the display dim enough that colours
  wash out) and draw a green/blue test pattern on the GC9306 TFT;
- emits a repeating **1 kHz** beep on the headphone DAC (`audio_play_tone()`).

This proves power, I²C, SPI/display, and I²S/ES8156 audio are alive.

Enable it in `idf.py menuconfig` (top-level **"Build the boot test firmware"**,
from `main/Kconfig.projbuild`), or directly:

```bash
echo 'CONFIG_APP_TEST_MODE=y' >> sdkconfig && idf.py build
```

Rev #04 bring-up notes (verified against a physical unit):

- The GC9306 driver (`components/gc9306/`) replicates the stock factory
  image's register init byte-for-byte (extracted from `output/factory.bin`,
  see `docs/ai/decompile.md` for the objdump workflow) — 21 command groups
  opening with the GalaxyCore inter-register enable `0xFE 0xEF`, COLMOD
  `0x06` = 18-bit RGB666, 3 bytes/pixel.
- The IOX must use the authoritative `hwconfig_04` defaults
  (`p0Dir=0xB0 p1Dir=0xAF p0Data=0x30 p1Data=0xEF`): the **level convertor
  (IOX.0.3) enable is active-low**. Driving it high — the #05-style default —
  silently blocks the whole display SPI/backlight domain while everything
  else (I²C, audio, logs) keeps working.
- **Panel colour transform (resolved)**: the GC9306 on this board renders
  the 18-bit stream as `displayed = (XNOR(R,G), G, G XOR B)` per 6-bit
  channel (recovered from two 4-stripe tests, 8/8 data points). The driver
  sends the inverse `(XNOR(Rd,Gd), Gd, Gd XOR Bd)` so requested colours
  display correctly (verified: red/green/blue/white stripes).

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
│   ├── Kconfig.projbuild     # CONFIG_APP_TEST_MODE switch
│   └── CMakeLists.txt
    ├── components/
    ├── board/board_pins.h    # THE pin map + I2C addresses (authoritative)
    ├── iox/                  # IO expander (buttons, display CS, power/amp ctrl)
    ├── battery/              # CW2215B fuel gauge + SGM41513 charger + ADC
    ├── display/              # HT16D35x 16x16 LED matrix (SPI + IOX CS)   [rev #05]
    ├── gc9306/               # GC9306 TFT driver (stock init + 18-bit)    [rev #04]
    ├── nfc/                  # ST CR95HF over UART
    └── audio/                # I2S + ES8156 / aw881xx codec
```

## What's implemented vs TODO

**Implemented** (real bus + peripheral setup, compiles against ESP-IDF v5):

- I2C master (SDA=21, SCL=25) + IO expander(s) with the factory direction/data
  (rev #04: authoritative `hwconfig_04` values — note the active-low level
  convertor on IOX.0.3, see the boot-test notes below).
- ADC1 (VBAT=GPIO39, light=GPIO36, IR-temp=GPIO35).
- SPI (MOSI=22, MISO=26, SCLK=23) + HT16D35x device handle [rev #05].
- **GC9306 TFT driver [rev #04]**: register init replicated from the stock
  factory image (21 groups, `0xFE 0xEF` inter-register enable, COLMOD 0x06 =
  18-bit RGB666), CASET/RASET/RAMWR drawing with DC-high data and CS held per
  rect, LEDC backlight on GPIO26 (40 kHz — the rail is AC-coupled), and the
  empirically-derived colour pre-transform (see boot-test notes). Verified:
  red/green/blue/white render correctly.
- UART (CR95HF, TX=33, RX=32, 57600 8-N-1).
- I2S standard TX (mclk=0, bclk=5, lrclk=18, dout=19, 44100 Hz 16-bit).

**TODO** (register-level detail, marked in the `.c` files with datasheet refs):

- CW2215B fuel-gauge register reads (VCELL/SOC).
- HT16D35x frame format + 6-bit-gray fan-out [rev #05].
- CR95HF command sequence (Idn/ProtocolSelect/SendRecv + ISO14443-3A activation).
- ES8156 + aw881xx codec register init (the aw881xx init was not cracked by
  Adafruit either — speaker path is at-risk; headphone via ES8156 is the known-good path).
- The NFC → `mapping.json` → play/render main loop, and the hidden upload mode.

## Hardware revision caveat

The Yoto shipped several hardware revisions with **different pins**. This
skeleton targets the latest revision (2× IO expander, HT16D35x LED matrix,
UART NFC, SDMMC 1-bit). If your unit differs, edit
`components/board/board_pins.h` — the variant table is in `../docs/hardware.md`.
