---
icon: lucide/wrench
---

# Build Environment Setup

How to set up the ESP-IDF toolchain to compile the replacement firmware in
[`firmware/`](https://github.com/tensorfish/open-yoto/tree/main/firmware), using
**uv** for Python version management (not Homebrew Python).

## Prerequisites

- [Homebrew](https://brew.sh/) — for `cmake`, `ninja`, `dfu-util`.
- [uv](https://docs.astral.sh/uv/) — for the Python interpreter.

## One-time setup (macOS)

```bash
# 1. Build tools (Python comes from uv, NOT brew)
brew install cmake ninja dfu-util

# 2. uv-managed Python 3.12 (ESP-IDF needs 3.10+)
uv python install 3.12

# 3. Clone ESP-IDF v5.5 and install its toolchain, using uv's Python
git clone --depth 1 --recursive --shallow-submodules -b release/v5.5 \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf
export PATH="$(dirname "$(uv python find 3.12)"):$PATH"   # uv Python first
~/esp/esp-idf/install.sh esp32
```

`install.sh` downloads the Xtensa cross-compiler and creates a Python virtualenv
(at `~/.espressif/python_env`) from the uv-managed interpreter.

## Build

```bash
. ~/esp/esp-idf/export.sh     # sets IDF_PATH + toolchain on PATH
cd firmware
idf.py set-target esp32
idf.py build
```

## Flash

`idf.py` drives `esptool.py` at the correct offsets, or you can raw-flash a
single merged image (the esptool workflow used throughout this project):

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# ...or a single merged image:
idf.py merge-bin -o merged.bin
esptool.py --chip esp32 --flash_size 8MB write_flash 0x0 merged.bin
```

Output binaries (flash offsets):

| File | Offset |
|------|--------|
| `build/bootloader/bootloader.bin` | `0x1000` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/yoto.bin` | `0x10000` |

Flash size is 8 MB (`ESP32-WROVER-E` module).

!!! note "`make` vs `idf.py`"
    ESP-IDF **v4+ removed the GNU-Make workflow**. The current build command is
    `idf.py` (CMake + Ninja) — there is no `make menuconfig` / `make flash` in
    modern ESP-IDF.

## Notes

- The original ESP32 (Xtensa LX6) has **no native USB**, so flashing is over the
  UART bridge only (`esptool.py ... write_flash`).
- The `firmware/README.md` has the same instructions plus the project structure
  and the pin-map caveat.
- Reference: [ESP-IDF macOS setup](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/macos-setup.html).
