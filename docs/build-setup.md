---
icon: lucide/wrench
---

# Build environment setup

Open Yoto is currently an **early-development source build**, not a published
firmware release. This page prepares the ESP-IDF **v5.5** project in
[`firmware/`](https://github.com/tensorfish/open-yoto/tree/main/firmware).
Before connecting hardware, read the [flash guide](firmware/flash-custom-firmware.md)
for board identification, backup, and electrical precautions.

## Prerequisites

The concrete setup below is for **macOS**, with [Homebrew](https://brew.sh/)
for build tools and [uv](https://docs.astral.sh/uv/) for Python. Git is also
required. Linux and Windows users should follow Espressif's
[ESP-IDF v5.5 installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/index.html)
for their host, then use the same project configuration steps below.
Shell examples use POSIX syntax.

## One-time setup (macOS)

```bash
brew install cmake ninja dfu-util
uv python install 3.12

git clone --depth 1 --recursive --shallow-submodules -b release/v5.5 \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf
export PATH="$(dirname "$(uv python find 3.12)"):$PATH"
~/esp/esp-idf/install.sh esp32
```

`install.sh` downloads the Xtensa tools and creates ESP-IDF's Python environment
under `~/.espressif/python_env`. Load it in each new shell with
`. ~/esp/esp-idf/export.sh`; do not hard-code the generated virtualenv directory.
The `release/v5.5` branch can advance, so record the ESP-IDF commit as well as
the Open Yoto commit when keeping a reproducible build or reporting a problem.

If you do not already have the project:

```bash
git clone https://github.com/tensorfish/open-yoto.git
cd open-yoto
```

## Configure for your board

From the Open Yoto repository root:

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32
idf.py menuconfig
```

`set-target` regenerates configuration and clears the build directory. Use it
when initially selecting the target, not as a substitute for reviewing the
configuration on every update. Both supported code paths target **esp32**, not
ESP32-S3 or ESP32-C3.

!!! warning "Saved configuration is not a safe board preset"
    `sdkconfig` is local build state; `sdkconfig.defaults` only supplies defaults.
    The checkout may be configured for hardware bring-up rather than the player
    application. Never infer your board from either file, and do not flash until
    the following settings match your identified hardware.

Under **open-yoto Application**:

| Menu option | Normal player build |
|---|---|
| **Target hardware revision #04 (TFT, single IOX)** (`BOARD_REV_04`) | **Enabled for #04**, disabled only for the partial #05 target |
| **Build the boot test firmware** (`APP_TEST_MODE`) | **Disabled** |
| **Build the I2C bus scanner firmware** (`APP_SCAN_MODE`) | **Disabled** |
| **Play a speaker test tone at boot** (`APP_SPEAKER_TEST_TONE`) | **Disabled** |

The scanner and boot test replace `app_main.c`; they do not provide the normal
NFC player/admin workflow. Board selection is compile-time, not auto-detection.
#04 uses the GC9306 color TFT, one IO expander and SDMMC 4-bit; #05 uses the
HT16D35x on/off display, two expanders and SDMMC 1-bit. See the
[support matrix](firmware/flash-custom-firmware.md#supported-hardware) before
choosing. Full-size Players and other revisions are not installation targets.

Under **Serial flasher config**, for the manual UART workflow:

- **Flash size → 8 MB** (the physical chip must agree).
- **Before flashing → No reset**.
- **After flashing → Stay in bootloader**.

The last two produce `CONFIG_ESPTOOLPY_BEFORE="no_reset"` and
`CONFIG_ESPTOOLPY_AFTER="no_reset"` in `sdkconfig`. Those are ESP-IDF's config
strings; direct **esptool v5 CLI** commands use hyphens (`--before no-reset`).
Do not assume the checked-in config already disables automatic reset.

Under **Partition Table**, retain the **Custom partition table CSV** selection
and `partitions.csv` filename from the project defaults. It reserves a 4 MiB
factory app partition needed by the current firmware, including Classic
Bluetooth; a generic small default app partition is not interchangeable.
Save and exit menuconfig.

## Build and keep a matching set

```bash
idf.py build
```

Keep these together outside disposable build directories:

- Open Yoto and ESP-IDF commit IDs, and any local source changes.
- The reviewed `sdkconfig` and `partitions.csv`.
- Build artifacts, including the ELF for interpreting crash logs.
- `components/admin/html/index.html` from **this same source revision**.

The current layout produces:

| File | Flash offset |
|---|---|
| `build/bootloader/bootloader.bin` | `0x1000` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/yoto.bin` | `0x10000` |

Prefer `idf.py flash`, which uses the generated arguments for this build,
rather than manually copying offsets from stock firmware or another revision.
The table is not an OTA layout: it has NVS, PHY data, and one factory app.

## Flash and monitor

**First-time installation must go through the
[flash guide](firmware/flash-custom-firmware.md)**: connect the adapter, select
its real UART port, and verify the full stock flash and SD backups before
writing. The player's USB-C connector is power-only; it is not a UART bridge.

With that preparation complete, from `firmware/`:

```bash
export PORT=/dev/cu.usbserial-XXXX  # replace with your actual adapter
idf.py -p "$PORT" flash
idf.py -p "$PORT" monitor
```

Enter the ROM bootloader manually for flashing. With GPIO0/BOOT held low,
pulse RESET/EN low then high, and release BOOT. After flashing, start the monitor
and pulse RESET with BOOT released to boot the application. Exit the monitor
with **Ctrl+]**. Detailed checks and recovery caveats are in the
[first-boot checklist](firmware/flash-custom-firmware.md#5-first-boot-acceptance-checklist)
and [update/recovery guide](firmware/flash-custom-firmware.md#updates-and-recovery).

### Optional Linux UART automation

Only use the helper if your **adapter wiring** matches its assumptions:
**RTS → BOOT/GPIO0**, **DTR → RESET/EN**, active-low. This is not a built-in
USB interface or a universal adapter pinout. Review the configuration above
first; the helper builds and flashes, but does not choose your board or disable
test modes for you. It also requires the no-reset settings above.

From the repository root in an ESP-IDF-enabled Linux shell:

```bash
python3 tools/flash_esp32.py --port /dev/ttyUSB0
```

Replace the port. The helper builds, waits for esptool's connection phase,
invokes `tools/bootloader_esp32.py`, flashes, resets, and captures UART output
(default `/tmp/yoto_scan.log`). The bootloader helper uses Linux modem-control
ioctls so it does not reconfigure the baud rate while esptool owns the port.
Do not run a second pyserial session to manipulate the same port during a
transfer. Logs can contain the admin access code; redact before sharing.

!!! note "`make` vs `idf.py`"
    ESP-IDF v4+ removed the GNU-Make workflow. Use `idf.py` (CMake + Ninja), not
    `make menuconfig` or `make flash`.

Next: [Post-flash setup](firmware/post-flash-setup.md) for the admin card and
matching web UI, or [firmware architecture](open-yoto-firmware.md) for development.
Project participation and scope live on the [project page](project.md).
