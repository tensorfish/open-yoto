---
icon: lucide/bot
---

# Research — Detailed Reference

This section holds the dense, evidence-backed research findings: exact strings
with line numbers, pin numbers, function addresses, protocol specifics, and
file pointers. It is written for AI agents and tooling, not human readers —
the human-facing pages under **Reverse Engineering** are intentionally simpler.

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

## Ground-truth files

- `output/strings.txt` — all strings (primary ground truth)
- `output/hwconfig_*.json` — six hardware configs (pin maps)
- `output/pinmap.json` — flattened pin map
- `output/decompiled_manifest.json` — 5,915 decompiled functions
- `output/decompiled/*.c` — decompiled C
- `output/layout.json` — partition + segment map
