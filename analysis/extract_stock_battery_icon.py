#!/usr/bin/env python3
"""Extract the stock low-SOC battery icon from a factory ESP32 app image.

Usage:
    python3 analysis/extract_stock_battery_icon.py output/factory.bin

The input is parsed as an ESP image so the source address is independent of
segment file offsets.  The two outputs are written beside the input:
``stock_low_battery.png`` is the exact embedded PNG byte stream and
``stock_low_battery.rgba`` is its decoded 16 x 16 RGBA8888 buffer.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

# Recovered from battery_ui at 0x400e7868.  The six 20-byte entries begin here.
BATTERY_TABLE = 0x3FFBF49C
LOW_SOC_ENTRY = (10, 0, 10, 1, 0)  # id, unknown, maximum SOC, powered, charging

# platform_icon_fetch uses one of these hardware-family tables.  Index 10 is
# identical in all three, so the selected source does not depend on that branch.
ICON_TABLES = (0x3F41D5A4, 0x3F41D814, 0x3F41DA84)
SOURCE_KEY = 10
SOURCE_PNG = 0x3F468D61
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def fail(message: str) -> None:
    raise ValueError(message)


def segments(image: bytes) -> list[tuple[int, int, int]]:
    if len(image) < 24 or image[0] != 0xE9:
        fail("input is not an ESP app image")
    offset = 24
    result = []
    for _ in range(image[1]):
        if offset + 8 > len(image):
            fail("truncated ESP segment header")
        address, size = struct.unpack_from("<II", image, offset)
        offset += 8
        if offset + size > len(image):
            fail("truncated ESP segment")
        result.append((address, size, offset))
        offset += size
    return result


def at_virtual(image: bytes, image_segments: list[tuple[int, int, int]], address: int, size: int) -> bytes:
    for base, length, file_offset in image_segments:
        if base <= address and address + size <= base + length:
            start = file_offset + address - base
            return image[start:start + size]
    fail(f"virtual range 0x{address:08x}+0x{size:x} is not in an image segment")


def u32(image: bytes, image_segments: list[tuple[int, int, int]], address: int) -> int:
    return struct.unpack("<I", at_virtual(image, image_segments, address, 4))[0]


def select_low_soc(entries: list[tuple[int, int, int, int, int]]) -> tuple[int, int, int, int, int]:
    """Mirror the matching order at 0x400e788a for low-SOC, not charging."""
    powered, charging, soc = 1, 0, 5
    for entry in entries:
        icon_id, _unknown, maximum_soc, entry_powered, entry_charging = entry
        if entry_powered != powered:
            continue
        # 0 is a wildcard; nonzero entries are only selected for charging=2.
        if entry_charging and charging != 2:
            continue
        if soc <= maximum_soc:
            return entry
    fail("stock low-SOC battery table has no matching entry")


def png_end_and_rgba(source: bytes) -> tuple[int, bytes]:
    if not source.startswith(PNG_SIGNATURE):
        fail("selected icon source is not a PNG")

    offset = len(PNG_SIGNATURE)
    idat = bytearray()
    width = height = bit_depth = color_type = interlace = None
    while True:
        if offset + 12 > len(source):
            fail("truncated PNG chunk")
        length = struct.unpack_from(">I", source, offset)[0]
        chunk_type = source[offset + 4:offset + 8]
        chunk_data_start = offset + 8
        chunk_end = chunk_data_start + length
        if chunk_end + 4 > len(source):
            fail("truncated PNG data")
        chunk = source[chunk_data_start:chunk_end]
        if chunk_type == b"IHDR":
            if length != 13:
                fail("invalid PNG IHDR length")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
            if (width, height, bit_depth, color_type, compression, filtering, interlace) != (16, 16, 8, 6, 0, 0, 0):
                fail("selected PNG is not a non-interlaced 16x16 RGBA8888 image")
        elif chunk_type == b"IDAT":
            idat.extend(chunk)
        elif chunk_type == b"IEND":
            if length:
                fail("invalid PNG IEND length")
            offset = chunk_end + 4
            break
        offset = chunk_end + 4

    if width is None or not idat:
        fail("PNG lacks IHDR or IDAT")
    scanlines = zlib.decompress(idat)
    stride = width * 4
    if len(scanlines) != height * (stride + 1):
        fail("unexpected decompressed PNG scanline size")

    rgba = bytearray()
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        filter_type = scanlines[position]
        current = bytearray(scanlines[position + 1:position + 1 + stride])
        position += stride + 1
        for x in range(stride):
            left = current[x - 4] if x >= 4 else 0
            above = previous[x]
            upper_left = previous[x - 4] if x >= 4 else 0
            if filter_type == 0:
                pass
            elif filter_type == 1:
                current[x] = (current[x] + left) & 0xFF
            elif filter_type == 2:
                current[x] = (current[x] + above) & 0xFF
            elif filter_type == 3:
                current[x] = (current[x] + ((left + above) >> 1)) & 0xFF
            elif filter_type == 4:
                p = left + above - upper_left
                pa, pb, pc = abs(p - left), abs(p - above), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else above if pb <= pc else upper_left
                current[x] = (current[x] + predictor) & 0xFF
            else:
                fail(f"unsupported PNG filter {filter_type}")
        rgba.extend(current)
        previous = current
    return offset, bytes(rgba)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", type=Path, default=Path("output/factory.bin"))
    args = parser.parse_args()

    image = args.image.read_bytes()
    image_segments = segments(image)
    entries = [
        struct.unpack("<IIIII", at_virtual(image, image_segments, BATTERY_TABLE + index * 20, 20))
        for index in range(6)
    ]
    if entries[0] != LOW_SOC_ENTRY:
        fail(f"unexpected first battery entry: {entries[0]!r}")
    selected = select_low_soc(entries)
    if selected[0] != SOURCE_KEY:
        fail(f"low-SOC selection expected icon {SOURCE_KEY}, got {selected[0]}")

    pointers = [u32(image, image_segments, table + SOURCE_KEY * 4) for table in ICON_TABLES]
    if pointers != [SOURCE_PNG] * len(ICON_TABLES):
        fail(f"icon-table index {SOURCE_KEY} does not resolve to 0x{SOURCE_PNG:08x}: {pointers!r}")

    # A 16x16 stock PNG is only 205 bytes, but read a bounded region large
    # enough to parse its terminal IEND chunk without assuming a file offset.
    source = at_virtual(image, image_segments, SOURCE_PNG, 0x1000)
    png_size, rgba = png_end_and_rgba(source)
    png = source[:png_size]
    if len(rgba) != 0x400:
        fail(f"decoded RGBA buffer is {len(rgba)} bytes, expected 0x400")

    output_dir = args.image.parent
    png_path = output_dir / "stock_low_battery.png"
    rgba_path = output_dir / "stock_low_battery.rgba"
    png_path.write_bytes(png)
    rgba_path.write_bytes(rgba)
    print(f"entry={selected} source=0x{SOURCE_PNG:08x} geometry=16x16 RGBA8888")
    print(f"wrote {png_path} sha256={hashlib.sha256(png).hexdigest()}")
    print(f"wrote {rgba_path} sha256={hashlib.sha256(rgba).hexdigest()}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zlib.error) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
