---
icon: lucide/cog
---

# Open Yoto

Independent firmware for the **Yoto Mini**: play local SD-card audio with NFC
cards, manage your library in a browser, or use the player as a Bluetooth
speaker. Normal local playback needs no Yoto account, cloud service, or
internet connection. This project is not affiliated with Yoto.

## Current status

**Early development, source builds only.** There are no published firmware
releases yet. Installation requires opening the player and using an external
3.3 V USB-UART adapter; it is not a consumer-ready upgrade.

- **#04** is the supported development target, with working color TFT evidence.
- **#05** is partial: the LED display is on/off only, without greyscale.
- **Full-size Players, other revisions, and unidentified boards are unsupported.**

These are hardware-configuration numbers, not retail model names. Read the
[compatibility matrix](firmware/flash-custom-firmware.md#supported-hardware)
and identify your actual board before building. Support does not mean every
unit, peripheral, power state, or recovery path has been hardware-validated.

The admin page uses an **open Wi-Fi hotspot and unencrypted HTTP**. Its
six-character access code is not encryption; use it briefly in a trusted
environment. See [security and project limits](project.md#security).

## Start here

1. **Considering an install?** Read [Flash custom firmware](firmware/flash-custom-firmware.md)
   for board identification, risks, verified backups, UART wiring, and recovery
   limits before changing anything.
2. **Ready to build?** Follow [Build environment setup](build-setup.md), select
   your board explicitly, and disable the bring-up test modes.
3. **Already flashed?** Use [Post-flash setup](firmware/post-flash-setup.md) to
   make the admin card, install the matching web UI, add your own media, and
   create playable cards.
4. **Developing or investigating?** Start with [How it works](open-yoto-firmware.md)
   and [Hardware & ports](hardware.md), then the architecture references below.

For contributing, responsible reporting, licensing scope, and what remains
before releases, see the [project page](project.md). There is no stock-content
import workflow; use media you own or have permission to use.

## Firmware architecture

`firmware/main/app_main.c` owns the player state machine. It initializes the
I/O expanders, battery monitor, display, audio path, NFC reader, encoders, SD
content store, and embedded system assets. It starts normal card playback only
outside admin mode.

| Area | Source | Responsibility |
|------|--------|----------------|
| Player state | `firmware/main/app_main.c` | Boot recovery, NFC card lifecycle, playback, physical controls, display state |
| Content | `firmware/components/content/` | SD mount, `/sdcard/media/library.json`, media paths, card mappings |
| Audio | `firmware/components/audio/`, `codec_es8156/`, `aw88194/` | MP3/AAC/M4A decode, I²S output, headphone/speaker routing |
| Physical I/O | `board/`, `iox/`, `encoder/`, `battery/`, `display/`, `gc9306/`, `ht16d35x/` | Revision pins, expanders, controls, power, and panel output |
| NFC | `firmware/components/cr95hf/` | Read/write NFC Type 2 card URLs |
| Admin mode | `firmware/components/admin/` | `openyoto` SoftAP, authenticated HTTP API, SD-hosted web UI |
| Bluetooth | `firmware/components/bluetooth/` | On-demand Classic Bluetooth A2DP speaker sink |
| System assets | `firmware/components/yoto_vfs/` | Read-only boot and welcome assets |

## Runtime model

1. Boot initializes the board and mounts the SD card.
2. In normal mode, an NFC URL resolves through `/sdcard/media/library.json`
   and starts the mapped media.
3. The admin magic URL (`https://openyoto.com/admin`) toggles the `openyoto`
   hotspot and shows the six-character access code.
4. While admin mode is active, card scans are captured for the web UI rather
   than starting playback.

## Documentation map

- **[Flash custom firmware](firmware/flash-custom-firmware.md)** — identify,
  connect, back up, and flash a supported development target.
- **[Post-flash setup](firmware/post-flash-setup.md)** — enter admin mode and
  set up local media and cards.
- **[Firmware behavior](open-yoto-firmware.md)** — boot, playback, controls,
  card mappings, and admin API.
- **[Reverse engineering](reverse-engineering.md)** — stock image layout and
  recovered hardware evidence.
- **[Research](ai/index.md)** — source-oriented subsystem reference.
- **[Project](project.md)** — contribution routes, security limitations,
  licensing scope, and release readiness.
