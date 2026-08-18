"""Load factory app as the correct ESP32/Xtensa BinaryView and save a .bndb.

Verifies arch, entry point, segments (correct load addresses), and ROM symbols.
Saves the database WITHOUT a full analysis pass (analysis runs separately).

Run inside Binary Ninja via:  bn py exec --script analysis/bn_load.py
"""
import binaryninja as bninja
from pathlib import Path

ROOT = Path("/Users/k/Development/tensorfish/open-yoto")
APP = ROOT / "output" / "factory.bin"
BNDB = ROOT / "output" / "yoto_factory.bndb"

view = bninja.load(str(APP), options={"analysis.mode": "off"})
if view is None:
    raise SystemExit("load returned None")

print(f"view_type={view.view_type}")
print(f"arch={view.arch}")
print(f"platform={view.platform}")
print(f"entry={hex(view.entry_point)}  start={hex(view.start)}  end={hex(view.end)}")
print("segments:")
for s in view.segments:
    print(f"  {hex(s.start)}-{hex(s.end)}  r={int(s.readable)} w={int(s.writable)} x={int(s.executable)}")

rom_syms = [s for s in view.symbols if s.address < 0x40070000]
print(f"ROM symbols={len(rom_syms)}")
print(f"total symbols={len(list(view.symbols))}")
print(f"strings={len(view.strings)}")

view.create_database(str(BNDB))
print(f"saved {BNDB}")
