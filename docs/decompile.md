---
icon: lucide/file-code-2
---

# How to Analyze This Firmware

A reproducible workflow based on `esptool` and the repository's extraction
scripts. No GUI disassembler or decompiler is required.

## What you need

| Tool | Purpose |
|------|---------|
| `uv` | Python package/environment manager |
| `esptool` | validate the extracted ESP32 application image |
| repository analysis scripts | extract partitions, strings, and embedded hardware configs |

## Steps

1. **Extract the app from the original flash dump.**

   ```bash
   uv run python analysis/extract_app.py ~/Downloads/yoto_firmware_clean.bin
   ```

   The script parses the ESP32 partition table at flash offset `0x8000` and
   writes the factory application to `output/factory.bin`.

2. **Validate the extracted image with Espressif's tool.**

   ```bash
   uv run esptool image-info output/factory.bin
   ```

   For the analyzed image, `esptool` reports an ESP32 image, seven segments,
   entry point `0x400813a8`, 8 MB flash, ESP-IDF `v5.1.4-dirty`, and valid
   checksum and validation hash.

3. **Recover strings and the pin map.**

   ```bash
   uv run python analysis/extract_strings.py
   uv run python analysis/extract_hwconfig.py
   ```

   `extract_strings.py` exposes the firmware's subsystem names, boot messages,
   error paths, and driver diagnostics. `extract_hwconfig.py` recovers all six
   hardware-config JSON documents embedded in the factory image.

## The key insight

The firmware's embedded **hardware-config JSON** is the authoritative pin map.
Use it together with the original image's strings and `esptool image-info`;
do not infer the hardware from prose documentation.
See [Methodology](methodology.md).
