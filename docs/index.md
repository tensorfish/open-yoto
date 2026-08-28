---
icon: lucide/cog
---

# open-yoto Firmware

A from-scratch, service-independent firmware for the Yoto Player's ESP32. It
plays local SD-card media from NFC cards and exposes an authenticated
administration UI on demand. No cloud account or Yoto service is required.

## Start here

- [Flash custom firmware](firmware/flash-custom-firmware.md) — supported
  hardware, backups, wiring, and flashing.
- [Post-flash setup](firmware/post-flash-setup.md) — make the admin card,
  install the local web UI, add media, and create cards.
- [Build environment](build-setup.md) — verified ESP-IDF and UART workflow.
- [How it works](open-yoto-firmware.md) — runtime architecture and behavior.
- [Hardware & ports](hardware.md) — board revisions and recovered pin maps.

## Firmware architecture

`firmware/main/app_main.c` owns the player state machine. It initializes the
I/O expanders, battery monitor, display, audio path, NFC reader, encoders, SD
content store, and embedded system assets. It starts normal card playback only
outside admin mode.

| Area | Source | Responsibility |
|------|--------|----------------|
| Player state | `firmware/main/app_main.c` | Boot recovery, NFC card lifecycle, playback, physical controls, display state |
| Content | `firmware/components/content/` | SD mount, `library.json`, media paths, card mappings |
| Audio | `firmware/components/audio/`, `codec_es8156/`, `aw88194/` | MP3/AAC/M4A decode, I²S output, headphone/speaker routing |
| Physical I/O | `board/`, `iox/`, `encoder/`, `battery/`, `display/` | Revision pins, expanders, controls, power, and panel output |
| NFC | `firmware/components/cr95hf/` | Read/write NFC Type 2 card URLs |
| Admin mode | `firmware/components/admin/` | `openyoto` SoftAP, authenticated HTTP API, SD-hosted web UI |
| System assets | `firmware/components/yoto_vfs/` | Read-only boot and welcome assets |

## Runtime model

1. Boot initializes the board and mounts the SD card.
2. In normal mode, an NFC URL resolves through `library.json` and starts the
   mapped media.
3. The admin magic URL (`https://openyoto.com/admin`) toggles the `openyoto`
   hotspot and shows the six-character access code.
4. While admin mode is active, card scans are captured for the web UI rather
   than starting playback.

## Documentation map

- **[Flash custom firmware](firmware/flash-custom-firmware.md)** — prepare,
  back up, connect, and flash a supported player.
- **[Post-flash setup](firmware/post-flash-setup.md)** — enter admin mode and
  set up local media and cards.
- **[Firmware behavior](open-yoto-firmware.md)** — boot, playback, controls,
  card mappings, and admin API.
- **[Reverse engineering](reverse-engineering.md)** — stock image layout and
  recovered hardware evidence.
- **[Research](ai/index.md)** — source-oriented subsystem reference.
