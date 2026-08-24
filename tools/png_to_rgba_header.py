#!/usr/bin/env python3
"""Generate a C header of 16x16 RGBA8888 frames from PNG icons.

The firmware links its faces as `static const uint8_t` arrays, so every icon
under ``firmware/icons`` that the player draws is converted here. Output is
deterministic: the same PNGs always produce the same header, and the emitted
provenance comment carries the source SHA-256 digests so a header can be
audited against its PNG.

Only stdlib is used (zlib for the IDAT stream); PNG colour types 0, 2, 4 and 6
at bit depth 8 without interlacing are supported, which covers the authored
icon set.

    python3 tools/png_to_rgba_header.py --symbol BOOT_FACE \
        --out firmware/main/boot_face_rgba.h \
        --alias IDLE_FACE_RGBA=last firmware/icons/face-0[1-8].png
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS_BY_COLOUR_TYPE = {0: 1, 2: 3, 4: 2, 6: 4}
FRAME_WIDTH = 16
FRAME_HEIGHT = 16
BYTES_PER_LINE = 16


def fail(message: str) -> None:
    raise SystemExit(f"error: {message}")


def unfilter(raw: bytes, width: int, height: int, channels: int) -> bytes:
    """Reverse the per-scanline PNG filters (RFC 2083 section 6)."""
    stride = width * channels
    out = bytearray()
    previous = bytearray(stride)
    position = 0

    for row in range(height):
        if position >= len(raw):
            fail(f"truncated scanline data at row {row}")
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        if len(line) != stride:
            fail(f"truncated scanline {row}")
        position += stride

        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 0:
                continue
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                estimate = left + up - up_left
                d_left = abs(estimate - left)
                d_up = abs(estimate - up)
                d_up_left = abs(estimate - up_left)
                if d_left <= d_up and d_left <= d_up_left:
                    predictor = left
                elif d_up <= d_up_left:
                    predictor = up
                else:
                    predictor = up_left
                line[i] = (line[i] + predictor) & 0xFF
            else:
                fail(f"unsupported PNG filter {filter_type} on row {row}")

        out.extend(line)
        previous = line

    return bytes(out)


def decode_rgba(path: Path) -> bytes:
    """Decode one PNG into a 16x16 RGBA8888 buffer."""
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        fail(f"{path} is not a PNG")

    header = None
    idat = bytearray()
    offset = len(PNG_SIGNATURE)
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.extend(body)
        elif kind == b"IEND":
            break
        offset += 12 + length

    if header is None:
        fail(f"{path} has no IHDR")
    width, height, depth, colour_type, compression, filter_method, interlace = header
    if (width, height) != (FRAME_WIDTH, FRAME_HEIGHT):
        fail(f"{path} is {width}x{height}; firmware frames are "
             f"{FRAME_WIDTH}x{FRAME_HEIGHT}")
    if depth != 8 or compression != 0 or filter_method != 0 or interlace != 0:
        fail(f"{path}: only 8-bit, non-interlaced PNGs are supported")
    if colour_type not in CHANNELS_BY_COLOUR_TYPE:
        fail(f"{path}: unsupported colour type {colour_type}")

    channels = CHANNELS_BY_COLOUR_TYPE[colour_type]
    pixels = unfilter(zlib.decompress(bytes(idat)), width, height, channels)

    rgba = bytearray()
    for i in range(0, len(pixels), channels):
        chunk = pixels[i:i + channels]
        if colour_type == 6:
            rgba.extend(chunk)
        elif colour_type == 4:
            rgba.extend((chunk[0], chunk[0], chunk[0], chunk[1]))
        elif colour_type == 2:
            rgba.extend((chunk[0], chunk[1], chunk[2], 0xFF))
        else:
            rgba.extend((chunk[0], chunk[0], chunk[0], 0xFF))

    expected = FRAME_WIDTH * FRAME_HEIGHT * 4
    if len(rgba) != expected:
        fail(f"{path} decoded to {len(rgba)} bytes, expected {expected}")
    return bytes(rgba)


def format_frame(rgba: bytes, indent: str) -> str:
    lines = []
    for start in range(0, len(rgba), BYTES_PER_LINE):
        row = rgba[start:start + BYTES_PER_LINE]
        lines.append(indent + " ".join(f"0x{value:02X}," for value in row))
    return "\n".join(lines)


def render_header(symbol: str, sources: list[Path], frames: list[bytes],
                  aliases: list[tuple[str, int]]) -> str:
    bytes_macro = f"{symbol}_RGBA_BYTES"
    count_macro = f"{symbol}_FRAME_COUNT"
    array = f"{symbol}_FRAMES"

    provenance = [
        f"/* Generated by tools/png_to_rgba_header.py from "
        f"{len(sources)} PNG frame{'s' if len(sources) != 1 else ''}.",
        " * Do not edit by hand; re-run the generator after changing the icons.",
        " *",
    ]
    for index, (path, frame) in enumerate(zip(sources, frames)):
        provenance.append(
            f" * [{index}] {path.as_posix()}\n"
            f" *     PNG SHA-256:  {hashlib.sha256(path.read_bytes()).hexdigest()}\n"
            f" *     RGBA SHA-256: {hashlib.sha256(frame).hexdigest()}")
    provenance.append(" */")

    body = ",\n".join("    {\n" + format_frame(frame, "        ") + "\n    }"
                      for frame in frames)

    parts = [
        "\n".join(provenance),
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define {bytes_macro} ({FRAME_WIDTH} * {FRAME_HEIGHT} * 4)",
        f"#define {count_macro} {len(frames)}",
        "",
        f"static const uint8_t {array}[{count_macro}][{bytes_macro}] = {{",
        body,
        "};",
    ]
    for name, index in aliases:
        target = (f"{count_macro} - 1" if index == len(frames) - 1
                  else str(index))
        parts += [
            "",
            f"#define {name} {array}[{target}]",
        ]
    return "\n".join(parts) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pngs", nargs="+", type=Path,
                        help="16x16 PNG frames, in animation order")
    parser.add_argument("--symbol", required=True,
                        help="C macro/array prefix, e.g. BOOT_FACE")
    parser.add_argument("--out", required=True, type=Path,
                        help="header file to write")
    parser.add_argument("--alias", action="append", default=[],
                        metavar="NAME=INDEX",
                        help="macro aliasing one frame; INDEX is 0-based or "
                             "'last' (repeatable)")
    args = parser.parse_args()

    sources = list(args.pngs)
    frames = [decode_rgba(path) for path in sources]
    aliases = []
    for spec in args.alias:
        name, _, index = spec.partition("=")
        if not name or not index:
            fail(f"--alias expects NAME=INDEX, got {spec!r}")
        position = len(frames) - 1 if index == "last" else int(index)
        if not 0 <= position < len(frames):
            fail(f"--alias {name}: frame {index} is outside 0..{len(frames) - 1}")
        aliases.append((name, position))
    args.out.write_text(render_header(args.symbol, sources, frames, aliases))
    print(f"wrote {args.out} ({len(frames)} frames, "
          f"{len(frames) * FRAME_WIDTH * FRAME_HEIGHT * 4} bytes of RGBA)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
