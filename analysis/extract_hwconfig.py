"""Extract complete hardware-config JSON documents and build a consolidated pin map.

The firmware embeds several full hardware descriptions (one per hardware
revision). Each is a standalone JSON object starting near a '"battery"' key.
This extracts every complete document and flattens GPIO/ADC/IOX assignments.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "output" / "factory.bin"
data = APP.read_bytes()


def extract_complete_docs(data: bytes, anchor: bytes) -> list[tuple[int, int, dict]]:
    """Find complete top-level JSON docs containing `anchor`. Returns (start, end, obj)."""
    docs = []
    for m in re.finditer(re.escape(anchor), data):
        mid = m.start()
        # walk back to outermost '{' before a null terminator / non-json region
        start = mid
        depth = 0
        for i in range(mid, -1, -1):
            c = data[i]
            if c == 0x7B:  # '{'
                depth += 1
                if depth == 1:
                    start = i
                    break
        # walk forward from start to balanced close
        d = 0
        in_str = False
        esc = False
        end = None
        for i in range(start, min(len(data), start + 100_000)):
            c = data[i]
            if in_str:
                if esc:
                    esc = False
                elif c == 0x5C:
                    esc = True
                elif c == 0x22:
                    in_str = False
                continue
            if c == 0x22:
                in_str = True
            elif c == 0x7B:
                d += 1
            elif c == 0x7D:
                d -= 1
                if d == 0:
                    end = i + 1
                    break
        if end is None:
            continue
        try:
            obj = json.loads(data[start:end].decode("utf-8", errors="replace"))
        except Exception:
            continue
        if isinstance(obj, dict):
            docs.append((start, end, obj))
    return docs


def fingerprint(obj: dict) -> str:
    def g(*path, default="?"):
        cur = obj
        for p in path:
            if not isinstance(cur, dict):
                return default
            cur = cur.get(p, default)
        return cur

    audio = g("audio", "audiochip", "type")
    spk = g("audio", "audiochip", "spkrchip")
    hp = g("audio", "audiochip", "hpchip")
    disp = g("display", "type")
    nfc = g("nfc", "type")
    sd = g("sd", "type")
    bat = g("battery", "monitor", "type")
    return f"audio={audio}|spk={spk}|hp={hp}|disp={disp}|nfc={nfc}|sd={sd}|bat={bat}"


def flatten_pins(obj: dict) -> dict[str, list[str]]:
    """Collect every GPIO.x / ADC.x / IOX.x.y assignment with its JSON path."""
    out: dict[str, list[str]] = {}

    def walk(node, path):
        if isinstance(node, dict):
            for k, v in node.items():
                walk(v, f"{path}.{k}" if path else k)
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, f"{path}[{i}]")
        elif isinstance(node, str):
            if re.match(r"^(GPIO|ADC|IOX)\b", node):
                out.setdefault(node, []).append(path)

    walk(obj, "")
    return out


def main() -> None:
    docs = extract_complete_docs(data, b'"battery"')
    # dedup by start offset
    uniq: dict[int, dict] = {}
    for s, e, obj in docs:
        uniq[s] = obj

    print(f"complete config docs: {len(uniq)}")
    pin_map: dict[str, dict[str, list[str]]] = {}
    for i, (off, obj) in enumerate(sorted(uniq.items())):
        fp = fingerprint(obj)
        name = f"hwconfig_{i:02d}_{fp.replace('|', '_').replace('=', '')}"
        (ROOT / "output" / f"{name}.json").write_text(json.dumps(obj, indent=2))
        pins = flatten_pins(obj)
        pin_map[name] = pins
        print(f"\n[{i}] {name}")
        print(f"    offset={off:#x}  {fp}")
        # concise pin list
        for pin in sorted(pins):
            funcs = pins[pin]
            print(f"    {pin:12s} -> {', '.join(funcs[:3])}{' ...' if len(funcs) > 3 else ''}")

    (ROOT / "output" / "pinmap.json").write_text(json.dumps(pin_map, indent=2))
    print(f"\nWrote output/pinmap.json")


if __name__ == "__main__":
    main()
