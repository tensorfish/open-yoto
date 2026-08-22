# open-yoto

Reverse engineering of the **Yoto Player** firmware (`yoto-firmware.bin`), an
ESP32 (Xtensa LX6) device running ESP-IDF + ESP-ADF.

## What's here

| Path | Contents |
|------|----------|
| `docs/` | Zensical documentation site (human + AI-agent reference) |
| `analysis/` | Python toolchain: partition extraction and string/config recovery |
| `zensical.toml` | Zensical site config (content in `docs/`, output in `site/`) |
| `pyproject.toml`, `uv.lock` | `uv`-managed Python project |

`output/` (firmware artifacts) and `yoto-firmware.bin` (the input dump) are
gitignored — regenerate them with the scripts below.

## Key findings

- **SoC**: ESP32 (Xtensa LX6), ESP-IDF + ESP-ADF. Factory app @ flash `0x40000`,
  entry `0x400813a8`.
- **Ports**: the firmware embeds six complete hardware-config JSON documents
  (its own per-revision pin map) recovered verbatim.
- **NFC / SD**: ST **CR95HF** reads NFC Forum Type 2 tags → UID + URL →
  `card → chapter → track` hierarchy under `/sdcard/cards/` (FatFS over SDMMC
  1/4-bit or SPI).
- **Display**: 16×16 HT16D35x LED matrix or GC9306 TFT; AW2028H night light.
- Full detail in `docs/` (human) and `docs/ai/` (agent reference).

## Reproduce

```bash
uv sync

# extract partitions + app image from the authoritative flash dump
uv run python analysis/extract_app.py ~/Downloads/yoto_firmware_clean.bin

# verify the extracted ESP32 app image
uv run esptool image-info output/factory.bin

# recover embedded strings and hardware pin maps
uv run python analysis/extract_hwconfig.py
uv run python analysis/extract_strings.py

# build / serve the docs
uv run zensical build
uv run zensical serve
```
