---
icon: lucide/bot
---

# AI Agents — Detailed Reference

This section is **for AI agents and tooling**, not human readers. It contains
the dense, evidence-backed detail: exact strings with line numbers, pin
numbers, function addresses, protocol specifics, and file pointers. The
human-facing pages (top level) are intentionally simpler.

## Index

| Page | Covers |
|------|--------|
| [decompile.md](decompile.md) | Full decompilation how-to: toolchain, exact commands, memory map, L32R caveat |
| [nfc.md](nfc.md) | CR95HF driver, Type 2 tag format, function addresses |
| [storage.md](storage.md) | SD transports, FatFS, content model, .preload.json schema |
| [display.md](display.md) | HT16D35x / GC9306, rendering, nightlight |
| [audio.md](audio.md) | ESP-ADF, codecs, decoders, function addresses |
| [power.md](power.md) | Fuel gauge, chargers, USB-C/Qi, function addresses |
| [connectivity.md](connectivity.md) | Wi-Fi/BT, MQTT, HTTP endpoints |
| [boot.md](boot.md) | Boot sequence, OTA gating, reset paths |
| [firmware-options.md](firmware-options.md) | Comparison of ESP-IDF / Arduino / CircuitPython / Rust / Zephyr |

## Ground-truth files

- `output/strings.txt` — all strings (primary ground truth)
- `output/hwconfig_*.json` — six hardware configs (pin maps)
- `output/pinmap.json` — flattened pin map
- `output/decompiled_manifest.json` — 5,915 decompiled functions
- `output/decompiled/*.c` — decompiled C
- `output/layout.json` — partition + segment map
