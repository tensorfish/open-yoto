# Repository map

`README.md` is the human entrypoint: short pitch, status table, doc links. Keep
it glanceable — detail belongs in `docs/`, not there.

- `docs/index.md` — documentation hub (published to https://docs.openyoto.com).
- `docs/firmware/setup.md` — install, flash, and first-boot guide for users.
- `docs/build-setup.md` — ESP-IDF installation and verified Linux UART flash workflow.
- `docs/open-yoto-firmware.md` — runtime architecture and behavior of this firmware.
- `docs/hardware.md` — board revisions, pin maps, and physical interfaces.
- `docs/reverse-engineering.md`, `docs/methodology.md`, `docs/decompile.md` — how the stock image was recovered.
- `docs/subsystems/` — per-subsystem summaries; `docs/reference/` — nav index only.
- `docs/ai/` — condensed reverse-engineering reference by subsystem (long form).
- `zensical.toml` — docs site config; `site/` is generated output, never edited by hand.
- `firmware/README.md` — ESP-IDF project layout, builds, hardware flashing, and the authoritative implemented/TODO list.
- `firmware/main/` — `app_main.c` player state machine, `test_main.c` bring-up firmware, generated RGBA icon headers.
- `firmware/components/` — one directory per peripheral or subsystem; `board/board_pins.h` is the authoritative pin map.
- `firmware/icons/` — source PNGs compiled into headers by `tools/png_to_rgba_header.py`.
- `firmware/test/host/` — host tests that need no hardware.
- `tools/` — hardware-control helpers: flash, bootloader, reset, serial capture, icon conversion.
- `analysis/` — firmware-dump extraction and reverse-engineering scripts.
- `assets/` — icons and manifests extracted from the stock image.
- `output/` — analysis artifacts; regenerable, not source.
