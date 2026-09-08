# Open Yoto firmware — ESP-IDF

Early-development, service-independent firmware for the **Yoto Mini's ESP32**.
This is a working player application under active development, not an ESP-IDF
skeleton or a production-ready release. It plays local MP3/AAC/M4A media from
NFC cards, provides an on-demand local web admin interface, and can receive
Classic Bluetooth audio. There are no published firmware releases yet; build
from source with **ESP-IDF v5.5**.

Start with the [installation guide](../docs/firmware/flash-custom-firmware.md).
It covers board identification, opening-device risks, verified device-specific
flash and SD backups, 3.3 V UART wiring, first boot, and recovery limitations.
Do not flash before completing those precautions. USB-C on the player is
power-only, not a flash interface.

## Hardware scope

| Target | Status | `BOARD_REV_04` |
|---|---|---|
| Mini matching #04: GC9306 TFT, single IOX, SDMMC 4-bit | Supported development target; working color display evidence, not complete hardware validation | Enabled |
| Mini matching #05: HT16D35x, dual IOX, SDMMC 1-bit | Partial; on/off display only, greyscale incomplete | Disabled |
| Full-size Players, other revisions, unidentified hardware | Unsupported installation targets | Do not flash |

Numbers refer to recovered hardware configurations, not retail names or a
proven chronology. Identify the actual board using the
[hardware reference](../docs/hardware.md) and
[`board_pins.h`](components/board/board_pins.h). Open Yoto selects the board at
**compile time**. The stock config table includes other hardware; its presence
is not support for those devices. Porting another board requires driver and
power-path work, not just changing a few pins.

## Build and flash

See [Build environment setup](../docs/build-setup.md) for ESP-IDF installation
and complete menu settings. From the repository root:

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32
idf.py menuconfig
```

In **open-yoto Application**:

- Enable **Target hardware revision #04 (TFT, single IOX)** only for #04;
  disable it for the partial #05 target.
- Disable **Build the boot test firmware**, **Build the I2C bus scanner
  firmware**, and **Play a speaker test tone at boot** for normal operation.

Do not trust a saved `sdkconfig` or Kconfig default to match your board or to
select the normal application. In **Serial flasher config**, select **8 MB**,
**Before flashing → No reset**, and **After flashing → Stay in bootloader**
for the manual workflow. Keep the custom `partitions.csv` partition table.
Save configuration, then:

```bash
idf.py build
```

Only after verifying the original flash and SD backups, select your adapter's
actual port and follow the flash guide's manual BOOT/GPIO0 and RESET/EN sequence:

```bash
export PORT=/dev/cu.usbserial-XXXX
idf.py -p "$PORT" flash
idf.py -p "$PORT" monitor
```

After flashing, release BOOT and pulse RESET to boot the application; exit the
monitor with **Ctrl+]**. Keep the source commit, ESP-IDF commit, `sdkconfig`,
build artifacts, and `components/admin/html/index.html` together. The UI must
match the source revision used for the firmware.

`idf.py` uses the generated flash arguments. The current build places the
bootloader at `0x1000`, partition table at `0x8000`, and `yoto.bin` at `0x10000`.
[`partitions.csv`](partitions.csv) reserves a 4 MiB factory app on the 8 MiB
flash, alongside NVS and PHY data. There are **no OTA slots or automatic image
rollback**. `/upload` updates the SD-hosted HTML only. See
[updates and recovery](../docs/firmware/flash-custom-firmware.md#updates-and-recovery)
for UART updates and the caveats of restoring a same-device full flash dump.

The optional Linux `tools/flash_esp32.py` helper expects the documented
RTS→BOOT and DTR→RESET adapter wiring and no-reset configuration. Follow the
[Linux helper instructions](../docs/build-setup.md#optional-linux-uart-automation)
rather than assuming a universal automatic-reset circuit.

## Current application behavior

These are source implementation notes, not evidence that every feature has
been hardware-validated on both boards.

- **Boot and display:** required IOX, battery, display, audio, NFC, and encoder
  initialization has bounded boot-retry handling. The embedded welcome audio
  starts before SD mounting; a welcome sound or `boot complete` log does not
  prove storage works. #04 renders color icons through GC9306; #05 has a
  boolean-pixel HT16D35x path. Boot, wink, and battery icons are compiled headers
  generated from `icons/`.
- **Local content:** FatFS mounts at `/sdcard`. Card entries live in
  `/sdcard/media/library.json` and refer to media under `/sdcard/media`.
  The index loads lazily; legacy Open Yoto `mapping.json` migration is not an
  importer for stock Yoto `/cards` metadata. No stock-content import workflow
  is implemented.
- **Audio:** MP3, standalone ADTS AAC, and M4A/AAC-LC decoding feed a shared
  resampling, PCM gain, I²S, ES8156 headphone, and AW88194 speaker path. Filename
  extensions are not a guarantee that an arbitrary codec/container is supported.
  Headphone insertion/removal changes routing; speaker and headphones still
  share the PCM gain rather than separate volume curves.
- **NFC/admin:** the exact NDEF URI `https://openyoto.com/admin` toggles the
  `openyoto` hotspot and six-character code. Normal card URLs resolve to local
  playlists; during admin mode, scans are captured for card management instead
  of playback. Card writes compare the expected UID and verify URL read-back.
- **Web UI:** the embedded installer at `/upload` writes
  `/sdcard/webui/index.html`. The matching reference page provides file/folder
  management, media upload, browser-side PNG/JPEG artwork conversion, card
  read/write, and player controls. The hotspot is **open** and traffic is
  **unencrypted HTTP**: access-code login and an HttpOnly session cookie do not
  provide transport confidentiality. Keep admin mode temporary and use a
  trusted physical environment. UART logs also include the code.
- **Bluetooth:** off at boot; a three-second right-knob hold toggles a Classic
  Bluetooth A2DP speaker sink named **Open Yoto**. The top-right indicator is a
  blue dot on #04 or on/off LED on #05. Audio is received from a phone/computer,
  not sent to Bluetooth headphones or saved to the SD card. A bounded PCM ring
  and worker feed the shared audio path with explicit playback-source ownership.
- **Power:** a three-second power hold or **30 minutes** of inactivity requests
  standby. User/idle standby retains VIN_HOLD on both board paths and uses
  **EXT1 ALL_LOW** on the active-low IOX interrupt for wake; a two-second held
  power button qualifies a retained-power wake. Physical VIN_HOLD cutoff is
  reserved for battery protection when external power is absent. Battery-only
  startup requires 4% SOC; two valid samples at or below 3% trigger critical
  shutdown. These policies are not a battery-safety certification.

For actual setup and acceptance checks, use
[Post-flash setup](../docs/firmware/post-flash-setup.md). Deeper runtime details
are in [Firmware behavior](../docs/open-yoto-firmware.md).

## Bring-up applications

Use **open-yoto Application** in `idf.py menuconfig` for deliberate hardware
bring-up, never as the normal install preset:

- `APP_TEST_MODE` selects `main/test_main.c`: IOX/battery/audio initialization,
  then red/green/blue/white TFT stripes on #04 or an LED border/cross on #05,
  plus a repeating 1 kHz tone. It does not run the NFC/SD/encoder player.
- `APP_SCAN_MODE` selects `main/scan_main.c`: an I²C address scan, not the player.
  It takes precedence over the boot test in `main/CMakeLists.txt`.
- `APP_SPEAKER_TEST_TONE` adds a repeating tone to the normal application for
  audio bring-up; disable it for normal use.

Choose the correct board in these modes too. To return to the player, disable
all three options in menuconfig and rebuild; do not append conflicting config
lines by hand. A diagnostic pattern or beep proves only the exercised path on
that unit, not full board support.

## Structure

```text
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults        # defaults, not an identified-board preset
├── partitions.csv           # NVS + PHY + single 4 MiB factory app
├── main/
│   ├── app_main.c           # player, gestures, power, NFC/admin lifecycle
│   ├── test_main.c          # deliberate display/audio bring-up
│   ├── scan_main.c          # deliberate I²C scan
│   ├── Kconfig.projbuild    # application and board selection
│   ├── CMakeLists.txt       # selects one application entry point
│   └── *_rgba.h            # generated boot, wink, and battery assets
├── components/
│   ├── board/              # revision-dependent pins and addresses
│   ├── iox/                # expander I/O and power controls
│   ├── battery/            # CW2215B gauge and SGM41513 charger
│   ├── lis2dh12/           # accelerometer startup
│   ├── display/            # common display rendering
│   ├── gc9306/             # #04 TFT transport
│   ├── ht16d35x/           # #05 on/off LED transport
│   ├── cr95hf/             # UART NFC Type 2 read/write
│   ├── encoder/            # knobs, buttons, gesture classification
│   ├── content/            # SD mount and local card library
│   ├── audio/              # decoding and shared output ownership
│   ├── codec_es8156/       # headphone DAC
│   ├── aw88194/            # speaker amplifier and recovered DSP data
│   ├── bluetooth/          # Classic Bluetooth A2DP sink
│   ├── admin/              # SoftAP, HTTP API, installer and reference HTML
│   ├── yoto_vfs/           # embedded system sound access
│   ├── libhelix-mp3/       # vendored decoder
│   └── libhelix-aac/       # vendored decoder
├── icons/                  # source image assets
└── test/host/              # host-side regression coverage, not hardware proof
```

Original Open Yoto code uses MIT; vendored components and recovered stock
assets are not automatically covered by that choice. See
[licensing scope](../docs/project.md#licensing) and
[release readiness](../docs/project.md#release-readiness), including the
outstanding stock-asset rights audit, before redistributing a firmware image.
Contribution and reporting routes are on the [project page](../docs/project.md).
