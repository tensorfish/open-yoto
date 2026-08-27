# Open Yoto

Open source replacement firmware for the [Yoto Mini](https://yotoplay.com/yoto-mini)
kids' audio player. Cards play straight off the SD card — no account, no cloud,
nothing phoning home.

<img width="653" height="728" alt="open-yoto-running" src="https://github.com/user-attachments/assets/49d305d2-d503-4f22-9f65-b21ed9a04f94" />

## Why

I didn't like that a kids' toy is permanently online. I had a week of spare time
and $100 of DeepSeek credits, so I dumped the stock firmware, worked out the
board, and wrote a new one from scratch to see how far I'd get in seven days.

Pretty far, as it turns out.

## Status

| What | State |
|---|---|
| Boot, power rails, screen off / sleep / wake | Done |
| Colour screen — rev #04 (GC9306) | Done |
| LED matrix screen — rev #05 (HT16D35x) | Partial — on/off pixels only, no grey levels |
| Speaker, headphones, and auto-switching between them | Done |
| MP3, AAC, and M4A playback | Done |
| NFC cards — read and write | Done |
| SD card library (tracks, artwork, card mappings) | Done |
| Knobs and buttons — play/pause, skip, volume, power | Done |
| Battery level and charging screens | Done |
| Boot faces and per-track artwork | Done |
| Wi-Fi admin page — upload tracks, edit cards, remote control | Done |
| Reusing the stock Yoto card data already on your SD card | Not started |
| Talking to Yoto's servers | Never |

## Docs

Full write-up at **[docs.openyoto.com](https://docs.openyoto.com)**:

- [Install & first boot](docs/firmware/setup.md)
- [Build & flash](docs/build-setup.md) — ESP-IDF v5.5
- [How it works](docs/open-yoto-firmware.md)
- [Hardware & pin maps](docs/hardware.md) — the Yoto shipped several board
  revisions with different pins; check yours first
- [Reverse engineering notes](docs/reverse-engineering.md)

Flashing means opening the player and using an external USB-UART adapter — the
USB-C port is power-only. It voids your warranty and can brick the device. Dump
the stock firmware before you overwrite it.
