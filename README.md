# open-yoto

Reverse engineering of the **Yoto Player** firmware (`yoto-firmware.bin`), an
ESP32 (Xtensa LX6) device running ESP-IDF + ESP-ADF.

## What's here

| Path | Contents |
|------|----------|
| `docs/` | Zensical documentation site (human + AI-agent reference) |
| `analysis/` | Python toolchain: extraction, string/config recovery, decompilation |
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

# extract partitions + app image
uv run python analysis/extract_app.py

# recover the embedded hardware pin maps
uv run python analysis/extract_hwconfig.py
uv run python analysis/extract_strings.py

# decompile with Ghidra/PyGhidra (fast Xtensa path)
GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
  uv run python analysis/ghidra_dump.py

# build / serve the docs
uv run zensical build
uv run zensical serve
```

## Decompilation notes

Binary Ninja (Personal) has no built-in Xtensa support; load the app with the
community `ESPFirmware` + Xtensa plugins for correct-type analysis and ROM
symbols. Ghidra 12 + PyGhidra is the primary decompilation engine — it analyzes
the whole app in ~90s (vs. hours for Binary Ninja's Python Xtensa plugin). See
`docs/ai/decompile.md` for the full how-to, including the Xtensa `L32R`
literal-pool caveat.
