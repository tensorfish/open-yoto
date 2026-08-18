"""PyGhidra: decompile all real code functions and dump C output.

Loads factory.bin with the correct ESP32 memory map, analyzes, then decompiles
every function in the code segments (IRAM/IROM/RTC_IRAM) above a size threshold
and writes the C to output/decompiled/ with a manifest.

Usage: GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
       uv run python analysis/ghidra_dump.py
"""
import json
import struct
from pathlib import Path

import pyghidra
from pyghidra import open_program

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "output" / "factory.bin"
OUT = ROOT / "output" / "decompiled"
OUT.mkdir(exist_ok=True)

data = APP.read_bytes()
nseg = data[1]
entry = struct.unpack("<I", data[4:8])[0]
segs = []
p = 24
for i in range(nseg):
    addr, size = struct.unpack("<II", data[p:p + 8])
    segs.append((addr, size, p + 8))
    p += 8 + size

# code regions (IRAM, RTC_IRAM, IROM)
CODE_REGIONS = [
    (0x40080000, 0x400A0000),   # IRAM
    (0x400C0000, 0x400C2000),   # RTC_IRAM
    (0x400D0000, 0x40400000),   # IROM (flash-mapped code)
]
MIN_SIZE = 40
MAX_FUNCS = 6000


def is_code(addr: int) -> bool:
    return any(lo <= addr < hi for lo, hi in CODE_REGIONS)


with open_program(
    str(APP),
    language="Xtensa:LE:32:default",
    compiler="default",
    loader="ghidra.app.util.opinion.BinaryLoader",
    analyze=False,
) as flat:
    program = flat.getCurrentProgram()
    mem = program.getMemory()
    space = program.getAddressFactory().getDefaultAddressSpace()

    from ghidra.util.task import ConsoleTaskMonitor
    monitor = ConsoleTaskMonitor()

    for i, (addr, size, off) in enumerate(segs):
        start = space.getAddress(addr & 0xFFFFFFFF)
        if mem.getBlock(start) is None:
            flat.createMemoryBlock(f"seg{i}", start, data[off:off + size], True)

    raw_blk = mem.getBlock(program.getImageBase())
    if raw_blk is not None and not is_code(raw_blk.getStart().getOffset()):
        mem.removeBlock(raw_blk, monitor)

    eaddr = space.getAddress(entry & 0xFFFFFFFF)
    flat.createLabel(eaddr, "entry", True)
    flat.disassemble(eaddr)
    flat.addEntryPoint(eaddr)
    flat.analyzeAll(program)

    fm = program.getFunctionManager()
    from ghidra.app.decompiler import DecompInterface
    decomp = DecompInterface()
    decomp.openProgram(program)

    funcs = list(fm.getFunctions(True))
    print(f"total functions: {len(funcs)}")

    candidates = []
    for f in funcs:
        a = f.getEntryPoint().getOffset()
        sz = f.getBody().getNumAddresses()
        if is_code(a) and sz >= MIN_SIZE:
            candidates.append((a, sz, f))
    # largest first (more likely real functions)
    candidates.sort(key=lambda x: -x[1])
    if len(candidates) > MAX_FUNCS:
        candidates = candidates[:MAX_FUNCS]
    print(f"code functions (>= {MIN_SIZE} bytes): {len(candidates)} (capped {MAX_FUNCS})")

    manifest = []
    for i, (a, sz, f) in enumerate(candidates):
        res = decomp.decompileFunction(f, 60, monitor)
        df = res.getDecompiledFunction()
        code = df.getC() if df is not None else f"/* failed: {res.getErrorMessage()} */"
        name = f"{f.getName()}_{a:#x}".replace("/", "_").replace(" ", "_")
        rel = f"decompiled/{name}.c"
        (OUT / f"{name}.c").write_text(f"// {f.getName()} @ {a:#x} size={sz}\n{code}")
        manifest.append({"addr": a, "name": f.getName(), "size": sz, "file": rel})
        if (i + 1) % 1000 == 0:
            print(f"  decompiled {i + 1}/{len(candidates)}")

    (ROOT / "output" / "decompiled_manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"wrote output/decompiled_manifest.json ({len(manifest)} functions)")
    print("done")
