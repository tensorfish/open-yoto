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

## Structure

```
firmware/
├── CMakeLists.txt            # top-level (idf.py / CMake)
├── sdkconfig.defaults        # default Kconfig
├── main/
│   ├── app_main.c            # boot + init sequence
│   └── CMakeLists.txt
└── components/
    ├── board/board_pins.h    # THE pin map + I2C addresses (authoritative)
    ├── iox/                  # PI4IOE5V6416 IO expander (buttons, display CS, power/amp ctrl)
    ├── battery/              # CW2215B fuel gauge + SGM41513 charger + ADC
    ├── display/              # HT16D35x 16x16 LED matrix (SPI + IOX CS)
    ├── nfc/                  # ST CR95HF over UART
    └── audio/                # I2S + ES8156 / aw881xx codec
```

## What's implemented vs TODO

**Implemented** (real bus + peripheral setup, compiles against ESP-IDF v5):

- I2C master (SDA=21, SCL=25) + both IO expanders with the factory direction/data.
- ADC1 (VBAT=GPIO39, light=GPIO36, IR-temp=GPIO35).
- SPI (MOSI=22, MISO=26, SCLK=23) + HT16D35x device handle.
- UART (CR95HF, TX=33, RX=32, 57600 8-N-1).
- I2S standard TX (mclk=0, bclk=5, lrclk=18, dout=19, 44100 Hz 16-bit).

**TODO** (register-level detail, marked in the `.c` files with datasheet refs):

- CW2215B fuel-gauge register reads (VCELL/SOC).
- HT16D35x frame format + 6-bit-gray fan-out.
- CR95HF command sequence (Idn/ProtocolSelect/SendRecv + ISO14443-3A activation).
- ES8156 + aw881xx codec register init (the aw881xx init was not cracked by
  Adafruit either — speaker path is at-risk; headphone via ES8156 is the known-good path).
- The NFC → `mapping.json` → play/render main loop, and the hidden upload mode.

## Hardware revision caveat

The Yoto shipped several hardware revisions with **different pins**. This
skeleton targets the latest revision (2× IO expander, HT16D35x LED matrix,
UART NFC, SDMMC 1-bit). If your unit differs, edit
`components/board/board_pins.h` — the variant table is in `../docs/hardware.md`.
