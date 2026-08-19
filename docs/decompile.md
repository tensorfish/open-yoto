---
icon: lucide/file-code-2
---

# How to Decompile This Firmware

A short guide to reproducing the reverse engineering. Full detail (exact
commands, addresses, caveats) is in the
[Research section](ai/decompile.md).

## What you need

| Tool | Purpose |
|------|---------|
| `uv` | Python package/env manager (pins Python 3.13) |
| `esptool` / custom Python | parse the ESP32 partition table + app image |
| Binary Ninja + **ESPFirmware** & **Xtensa** plugins | correct-type load + ROM symbols |
| Ghidra 12 + **PyGhidra** | fast Xtensa decompilation |

## Steps

1. **Extract the app** — the firmware is an 8 MiB flash dump. The factory app
   is a self-contained ESP32 image at offset `0x40000`.

   ```bash
   uv run python analysis/extract_app.py
   ```

2. **Open in Binary Ninja** (correct type) — load `output/factory.bin` with the
   `ESPFirmware` view type, which auto-detects the chip and maps segments to
   their real addresses.

3. **Decompile with Ghidra** — Ghidra's Xtensa is much faster than Binary
   Ninja's Python Xtensa plugin:

   ```bash
   GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
     uv run python analysis/ghidra_dump.py
   ```

4. **Recover strings + pin map** — the firmware embeds its own hardware pin
   map as JSON:

   ```bash
   uv run python analysis/extract_strings.py
   uv run python analysis/extract_hwconfig.py
   ```

## The key insight

The firmware's own embedded **hardware-config JSON** is the authoritative pin
map — more reliable than inferring pins from decompiled `gpio_config()` calls.
See [Methodology](methodology.md).
