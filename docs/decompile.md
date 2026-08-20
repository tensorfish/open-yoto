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
| ESP-IDF toolchain `xtensa-esp32-elf-objdump` | **primary** Xtensa disassembly (no GUI, no Ghidra) |
| Binary Ninja + **ESPFirmware** & **Xtensa** plugins | *optional* GUI: correct-type load + ROM symbols |
| Ghidra 12 + **PyGhidra** | *optional* C-level decompilation |

## Steps

1. **Extract the app** — the firmware is an 8 MiB flash dump. The factory app
   is a self-contained ESP32 image at offset `0x40000`.

   ```bash
   uv run python analysis/extract_app.py
   ```

2. **Disassemble with the Espressif objdump** — no Ghidra needed. Extract the
   code segment to a raw file, then disassemble at its real virtual address:

   ```bash
   # IROM segment: vaddr 0x400D0020, file offset 0xB0020, size 0x18F0CC
   python3 -c "d=open('output/factory.bin','rb').read(); \
     open('/tmp/irom.bin','wb').write(d[0xB0020:0xB0020+0x18F0CC])"
   xtensa-esp32-elf-objdump -D -b binary -m xtensa --adjust-vma=0x400D0020 \
     /tmp/irom.bin > /tmp/irom.dis
   ```

   `xtensa-esp32-elf-objdump` ships with ESP-IDF
   (`~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/`, or on `PATH`
   after `source $IDF_PATH/export.sh`). To disassemble one function cleanly,
   extract a window starting at its address and re-anchor:
   `--adjust-vma=<function address>` (linear decode drifts at literal pools).

3. **Recover strings + pin map** — the firmware embeds its own hardware pin
   map as JSON:

   ```bash
   uv run python analysis/extract_strings.py
   uv run python analysis/extract_hwconfig.py
   ```

4. **Optional GUI passes** — Binary Ninja (`ESPFirmware` view) adds ~2,000 ROM
   symbols; Ghidra/PyGhidra give C pseudocode:

   ```bash
   GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
     uv run python analysis/ghidra_dump.py
   ```

## The key insight

The firmware's own embedded **hardware-config JSON** is the authoritative pin
map — more reliable than inferring pins from decompiled `gpio_config()` calls.
See [Methodology](methodology.md).
