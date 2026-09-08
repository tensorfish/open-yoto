# Open Yoto

Independent replacement firmware for the [Yoto Mini](https://yotoplay.com/yoto-mini)
kids' audio player. Play your own audio from the SD card with NFC cards — no
Yoto account or cloud service required.

**Development firmware, built from source.** Targets Mini board revisions #04
and #05; #05 display support is partial. Not for full-size Yoto Players or
unlisted boards. [Check compatibility before installing](docs/firmware/flash-custom-firmware.md).

<img width="653" height="728" alt="open-yoto-running" src="https://github.com/user-attachments/assets/49d305d2-d503-4f22-9f65-b21ed9a04f94" />

## Why

I didn't like that a kids' toy is permanently online. I had a week of spare time
and $100 of DeepSeek credits, so I dumped the stock firmware, worked out the
board, and wrote a new one from scratch to see how far I'd get in seven days.

Pretty far, as it turns out.

## Status

| What | State |
|---|---|
| Boot, power rails, screen off / sleep / wake | Implemented |
| Colour screen — rev #04 (GC9306) | Implemented |
| LED matrix screen — rev #05 (HT16D35x) | Partial — on/off pixels only, no grey levels |
| Speaker, headphones, and automatic switching | Implemented — shared volume curve |
| MP3, ADTS AAC, and M4A/AAC-LC playback | Implemented |
| NFC cards — read and write | Implemented |
| SD card library (tracks, artwork, card mappings) | Implemented |
| Knobs and buttons — play/pause, skip, volume, power | Implemented |
| Battery level and charging screens | Implemented |
| Boot faces and per-track artwork | Implemented |
| On-demand Wi-Fi admin — upload tracks, edit cards, remote control | Implemented — open hotspot, unencrypted HTTP |
| Optional Bluetooth speaker mode | Implemented — receives phone audio, not Bluetooth headphone output |
| Reusing the stock Yoto card data already on your SD card | Not started |
| Talking to Yoto's servers | Never |

Implemented does not mean validated on every unit. See the
[firmware inventory](firmware/README.md) and [release-readiness gaps](docs/project.md#release-readiness).

## Docs

Full write-up at **[docs.openyoto.com](https://docs.openyoto.com)**:

- [Flash custom firmware](docs/firmware/flash-custom-firmware.md)
- [Post-flash setup](docs/firmware/post-flash-setup.md) — admin card, local
  web UI, media, and cards
- [Build environment](docs/build-setup.md) — ESP-IDF v5.5
- [How it works](docs/open-yoto-firmware.md)
- [Hardware & pin maps](docs/hardware.md) — the Yoto shipped several board
  revisions with different pins; check yours first
- [Reverse engineering notes](docs/reverse-engineering.md)
- [Support & contributing](docs/project.md) — bug reports, development checks,
  security boundaries, and release-readiness work

Flashing means opening the player and using an external 3.3 V USB-UART adapter —
the USB-C port is power-only. Modification can damage the device and may affect
warranty coverage. Back up this player's complete flash and SD card first.

## Licence

Original code and documentation: [MIT](LICENSE). Third-party code and stock-derived
assets are **not** relicensed; [redistribution questions remain](docs/project.md#licensing).
Open Yoto is not affiliated with or endorsed by Yoto.
