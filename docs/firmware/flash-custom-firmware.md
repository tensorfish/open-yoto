---
icon: lucide/cpu
---

# Flash custom firmware

Open Yoto is an **early-development, source-built** replacement for the Yoto
Mini's ESP32 firmware. There are no published firmware releases yet. This is
not a one-click installer or a production-ready upgrade: expect to open the
player, identify its hardware, build for that board, and troubleshoot over UART.
Read the whole guide before opening the player.

!!! danger "You can permanently damage the player"
    Opening the player may affect its warranty. A wiring mistake can damage
    hardware; an interrupted or incorrect flash can leave it unbootable.
    Save and verify the original flash and SD card before writing anything.
    Do not proceed if you cannot identify the board or preserve its backups.

## Supported hardware

The revision numbers below refer to recovered hardware configurations, not
retail model names or a proven manufacturing chronology. The
[hardware reference](../hardware.md#hardware-revisions) includes stock configs
that Open Yoto does **not** support; it is not an installation compatibility list.

| Hardware target | Current status | Build selection |
|---|---|---|
| Yoto Mini matching config **#04**: GC9306 TFT, one IO expander, SDMMC 4-bit | Supported development target; color display has working hardware bring-up evidence. Not a claim that every subsystem or unit is fully validated. | `BOARD_REV_04` enabled |
| Yoto Mini matching config **#05**: HT16D35x LED panel, two IO expanders, SDMMC 1-bit | Partial implementation; display output is on/off only, with greyscale still incomplete. Do not treat it as equivalent to #04 maturity. | `BOARD_REV_04` disabled |
| Full-size Yoto Players, other revisions, or unidentified boards | Unsupported by this installation guide | Do not flash |

Match the board's components and wiring against the
[pin maps](../hardware.md) before choosing a build. Do not infer a match from
the case, purchase date, or an ESP32 chip identification alone. See the
[physical disassembly and connector overview](https://learn.adafruit.com/hacking-the-yoto-music-players/overview)
for access, not as a guarantee of Open Yoto compatibility.

## What you need

- An identified board covered above, with the limitations understood.
- An external USB-UART adapter with **3.3 V logic**.
- Access to UART0, **BOOT/GPIO0**, **RESET/EN**, and ground inside the player.
- Stable player power and a safe workspace for exposed electronics.
- A computer with [ESP-IDF v5.5 and this repository](../build-setup.md).
- Space for two copies of the complete flash dump and a separate SD backup.

The player's USB-C port supplies power only; it does not expose the ESP32
UART. A USB-C cable alone cannot flash it. Do not power the player from the
adapter's VCC pin or connect 5 V UART signals.

## 1. Connect and select the UART port

With player power disconnected, confirm the connector pinout for your board
and connect:

| USB-UART line | Player signal |
|---|---|
| GND | GND |
| Adapter RX | ESP32 UART0 TX |
| Adapter TX | ESP32 UART0 RX |

Keep BOOT/GPIO0 and RESET/EN accessible, then restore player power safely.
BOOT and RESET below mean those electrical signals or your attached controls,
not the player's everyday power or knob buttons.

Load the ESP-IDF environment and list serial adapters:

```bash
. ~/esp/esp-idf/export.sh
python -m serial.tools.list_ports -v
export PORT=/dev/cu.usbserial-XXXX
```

Replace the example with your adapter's actual port: typically
`/dev/cu.usbserial-*` on macOS or `/dev/ttyUSB*` on Linux. Compare the port list
with the adapter unplugged and plugged in if uncertain. These examples use a
POSIX shell; Windows users must adapt the shell syntax and select the matching
`COM` port. Close any serial monitor before backup or flashing commands.

### Enter the ROM bootloader manually

Before **each** esptool operation below:

1. Hold **BOOT/GPIO0 low**.
2. While holding BOOT, assert **RESET/EN low**, then release RESET/EN high.
3. Release BOOT after reset so GPIO0 is no longer held low.
4. Run the command. If it only reports `Connecting...`, repeat the sequence
   while it is connecting.

The `--before no-reset --after no-reset` options leave reset control to you.
They do not enter the bootloader on their own. To boot the application later,
release BOOT and pulse RESET once. GPIO0 is also used by audio, so do not leave
it grounded during normal operation.

## 2. Back up and verify before writing

These commands use **esptool v5** syntax (`read-flash`, not `read_flash`). Check
`esptool version` in the loaded environment. If it is not v5, use a separate
v5 environment rather than changing ESP-IDF's managed dependencies in place.
The following assumes an **8 MiB** flash chip; stop if detection disagrees:

```bash
esptool --chip esp32 --port "$PORT" --before no-reset --after no-reset flash-id
```

Create a private backup directory outside this repository. Set `BACKUP` to a
new filename labelled for this specific player, then enter the ROM bootloader
again and read the full chip:

```bash
export BACKUP="$HOME/yoto-backups/my-player-stock-8MiB.bin"
esptool --chip esp32 --port "$PORT" --baud 115200 --before no-reset --after no-reset read-flash 0x0 0x800000 "$BACKUP"
```

Re-enter the bootloader and compare the saved file against flash:

```bash
esptool --chip esp32 --port "$PORT" --baud 115200 --before no-reset --after no-reset verify-flash 0x0 "$BACKUP"
wc -c "$BACKUP"
shasum -a 256 "$BACKUP"
```

Require a successful verification and a file size of **8,388,608 bytes** before
continuing. Save the SHA-256 result with the board identification and backup
date, and keep a second copy on separate storage. A checksum protects against
later file corruption; it does not prove the player can be restored.

Power down before removing the SD card. Make a full SD image or copy **all**
files, including hidden files, using your computer's backup tools. Confirm the
backup is readable before returning the card to the player. A flash dump does
not include SD contents, and an SD backup does not include ESP32 flash.

!!! warning "Backups are private and device-specific"
    Stock flash can contain identifiers, credentials, and other private data.
    Never post dumps in issues, commit them, or substitute another player's
    image. Keep the original flash and SD backups unchanged, even after making
    newer Open Yoto backups. Redact UART logs too: admin access codes are logged.

If esptool reports secure-boot, flash-encryption, or read-protection restrictions,
stop and investigate that device. Do not bypass them with `--force`, erase the
chip, or change eFuses to make this guide work.

Command references: Espressif's esptool v5
[basic commands](https://github.com/espressif/esptool/blob/v5.0.0/docs/en/esptool/basic-commands.rst),
[verification](https://github.com/espressif/esptool/blob/v5.0.0/docs/en/esptool/advanced-commands.rst),
and [reset options](https://github.com/espressif/esptool/blob/v5.0.0/docs/en/esptool/advanced-options.rst).

## 3. Configure and build Open Yoto

Follow [Build environment setup](../build-setup.md#configure-for-your-board)
for the full configuration procedure. From the repository root:

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32
idf.py menuconfig
```

**Do not build and flash using unreviewed defaults.** In **open-yoto
Application**, enable **Target hardware revision #04 (TFT, single IOX)** only
for #04; disable it for the partial #05 target. Disable **Build the boot test
firmware**, **Build the I2C bus scanner firmware**, and **Play a speaker test
tone at boot** for normal use.

In **Serial flasher config**, confirm **Flash size → 8 MB**, set **Before
flashing → No reset**, and **After flashing → Stay in bootloader** for this
manual workflow. Save and exit, then:

```bash
idf.py build
```

Keep the source commit, saved `sdkconfig`, build artifacts, and
`components/admin/html/index.html` together. The web UI must come from the same
source revision as the firmware, not whichever version happens to be on `main`
when you install it.

## 4. Flash and monitor

From `firmware/`, with the verified backup safe and `PORT` still set, enter the
ROM bootloader and run:

```bash
idf.py -p "$PORT" flash
```

Keep power and wiring stable until flashing and verification finish. Do not
use `erase-flash` as a routine installation step. `idf.py` uses the build's
partition table and correct offsets; the Open Yoto layout differs from stock.

Start the UART monitor, then release BOOT and pulse RESET to boot Open Yoto:

```bash
idf.py -p "$PORT" monitor
```

The console is 115200 baud. Exit the ESP-IDF monitor with **Ctrl+]** before
using the port for another command.

## 5. First-boot acceptance checklist

Check the actual player, not just a successful flash command:

- [ ] UART reaches `boot complete` without a repeated reset or failed required
      peripheral initialization.
- [ ] No `SD/content initialization failed` warning appears. A welcome sound
      or `boot complete` alone does **not** prove SD access works.
- [ ] The display shows the startup/idle UI appropriate to the selected board;
      #05's on/off display is not expected to reproduce #04 color artwork.
- [ ] Audio is audible at a comfortable volume and the knobs respond.
- [ ] Using [Post-flash setup](post-flash-setup.md), the admin card shows a code,
      the hotspot opens, and the matching web UI installs.
- [ ] A short local audio file uploads, plays, and works from a newly written
      card after leaving admin mode. Confirm removing the card stops playback.
- [ ] Scanning the admin card again stops the hotspot when finished.

These are checks for your unit, not a project-wide hardware certification.
Keep UART access available until the basic journey works.

## Updates and recovery

There is **no firmware OTA update or automatic image rollback** in the current
installation path. `firmware/partitions.csv` has one 4 MiB `factory` application
partition, plus NVS and PHY data; no OTA slots or OTA metadata. The embedded
`/upload` page replaces the **web UI**, not the firmware. Runtime boot-retry
logic is not a second firmware image.

### Updating Open Yoto

1. Preserve the untouched stock backups. Also back up the current SD contents
   (especially `media/library.json`, media/artwork, and `webui/index.html`) and,
   if you want a snapshot of the current install, make a new full flash dump.
2. Choose and record a source revision. Keep the previous source, `sdkconfig`,
   build files, and matching UI; do not assume data formats stay compatible.
3. Repeat configuration and build for the **same physical board**, then flash
   over UART as above. Recheck settings after `set-target` or changing source.
4. Upload `index.html` from that revision via `/upload` and repeat the first-boot
   checklist. An ESP32 firmware flash does not back up or update the SD-hosted UI.

### Returning to a saved full flash image

A full-chip restore through the ESP32 ROM loader is a recovery route, **not a
hardware-tested rollback guarantee**. It requires functioning UART access and
the unchanged, verified **8 MiB dump from this exact player**. It cannot repair
wiring damage, changed eFuses, a missing backup, or incompatible SD data.

After checking the saved dump's size and recorded checksum, enter the ROM
bootloader. This command overwrites the entire flash with that saved image:

```bash
esptool --chip esp32 --port "$PORT" --baud 115200 --before no-reset --after no-reset write-flash 0x0 "$BACKUP"
```

Re-enter the bootloader and run `verify-flash 0x0 "$BACKUP"` with the same global
options used for backup. Do not override flash-mode/size/frequency header values
or use `--force`. Power down and restore the matching SD backup if its contents
were changed, then release BOOT and reset. A successful byte comparison proves
flash matches the file, not that stock services or every function will work.

## Troubleshooting

| Symptom | Check before writing again |
|---|---|
| Port absent or busy | Adapter driver, actual port selection, permissions on Linux, and whether another monitor owns the port. Player USB-C is not the UART. |
| Stuck at `Connecting...` | Common ground, crossed TX/RX, 3.3 V logic, correct pads, stable power, and the manual BOOT/RESET sequence. |
| Transfer errors | Stop, inspect wiring and power, and retry at 115200 baud (`idf.py -b 115200 -p "$PORT" flash`). Do not bypass verification failures. |
| Repeating beep/test pattern or I2C address scan | Revisit `menuconfig`; disable test, scanner, and speaker-tone modes, then rebuild. |
| Blank display, peripheral init failure, or repeated resets | Capture the first UART error; recheck the physical revision and `BOARD_REV_04`. Do not try unrelated board settings at random. |
| Immediate low-battery shutdown | Charge using the player's normal power input; the firmware has a battery-only startup gate. Do not disable battery checks. |
| SD failure despite welcome sound | Power down, check SD seating/filesystem and board selection; preserve the card backup before repair or formatting. |
| Admin page or card setup fails | Use [post-flash troubleshooting](post-flash-setup.md#troubleshooting); check the exact NFC record and matching UI revision. |

For a reproducible problem, keep the source revision, board identification,
configuration, and a redacted UART log. See [Contributing](../project.md#contributing)
and the [release-readiness limits](../project.md#release-readiness).
