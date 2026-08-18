"""Extract ESP32 partitions and app images from a full flash dump.

Usage: uv run python analysis/extract_app.py [flash.bin]

Produces:
  output/<label>.bin         - each partition as a standalone file
  output/factory_app.bin     - factory app image (header at offset 0)
  output/layout.json         - partition table + segment map
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

FLASH = Path(sys.argv[1] if len(sys.argv) > 1 else "yoto-firmware.bin")
OUT = Path("output")
OUT.mkdir(exist_ok=True)

CHIPS = {
    0x0000: "ESP32", 0x0002: "ESP32-S2", 0x0005: "ESP32-C3", 0x0009: "ESP32-S3",
    0x000C: "ESP32-C2", 0x000D: "ESP32-C6", 0x0012: "ESP32-H2", 0x0017: "ESP32-P4",
    0x0021: "ESP32-C5",
}
PART_TYPES = {0x00: "app", 0x01: "data"}
PART_SUBTYPES = {
    0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x20: "test",
    0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x81: "nvs_keys",
}


def parse_partition_table(data: bytes, base: int = 0x8000) -> list[dict]:
    parts = []
    for off in range(base, base + 0x1000, 32):
        if data[off:off + 2] != b"\xaa\x50":
            break
        typ = data[off + 2]
        sub = data[off + 3]
        poff = struct.unpack("<I", data[off + 4:off + 8])[0]
        psize = struct.unpack("<I", data[off + 8:off + 12])[0]
        label = data[off + 12:off + 28].split(b"\x00")[0].decode(errors="replace")
        parts.append({
            "label": label, "type": typ, "subtype": sub,
            "offset": poff, "size": psize,
            "type_name": PART_TYPES.get(typ, "?"),
            "subtype_name": PART_SUBTYPES.get(sub, "?"),
        })
    return parts


def parse_app_image(data: bytes, base: int) -> dict | None:
    """Parse an ESP32 app image at `base`. Returns image metadata or None."""
    if base + 24 > len(data) or data[base] != 0xE9:
        return None
    hdr = data[base:base + 24]
    nseg = hdr[1]
    entry = struct.unpack("<I", hdr[4:8])[0]
    chip_id = struct.unpack("<H", hdr[12:14])[0]
    segments = []
    p = base + 24
    for _ in range(nseg):
        if p + 8 > len(data):
            break
        addr, size = struct.unpack("<II", data[p:p + 8])
        segments.append({"load_address": addr, "size": size, "file_offset": p + 8 - base})
        p += 8 + size
    # checksum byte then optional 32-byte SHA256
    p += 1
    hash_len = 32 if data[p:p + 1] else 0
    return {
        "chip": CHIPS.get(chip_id, f"0x{chip_id:04x}"),
        "chip_id": chip_id,
        "segments": nseg,
        "entry": entry,
        "image_size": p + hash_len - base,
        "segment_map": segments,
    }


def main() -> None:
    data = FLASH.read_bytes()
    layout = {"flash_size": len(data), "partitions": [], "images": {}}

    parts = parse_partition_table(data)
    layout["partitions"] = parts
    print(f"Flash: {len(data)} bytes ({len(data) // 1024 // 1024} MiB)")
    print("\nPartition table:")
    for p in parts:
        print(f"  {p['label']:12s} {p['type_name']:4s}/{p['subtype_name']:9s} "
              f"off={p['offset']:#08x} size={p['size']:#08x} ({p['size'] // 1024} KB)")

    for p in parts:
        label = p["label"]
        chunk = data[p["offset"]:p["offset"] + p["size"]]
        out_path = OUT / f"{label}.bin"
        out_path.write_bytes(chunk)
        nonff = sum(1 for b in chunk if b != 0xFF)
        filled = 100 * nonff / len(chunk) if chunk else 0
        print(f"  dumped {label}: {out_path} (non-FF {filled:.1f}%)")

        if p["type"] == 0x00 and chunk[:1] == b"\xe9":
            img = parse_app_image(chunk, 0)
            if img:
                layout["images"][label] = img
                print(f"    -> app image: chip={img['chip']} entry={img['entry']:#x} "
                      f"segs={img['segments']} size={img['image_size']:#x}")
                for s in img["segment_map"]:
                    print(f"       seg load={s['load_address']:#010x} size={s['size']:#x}")

    (OUT / "layout.json").write_text(json.dumps(layout, indent=2))
    print(f"\nWrote {OUT / 'layout.json'}")


if __name__ == "__main__":
    main()
