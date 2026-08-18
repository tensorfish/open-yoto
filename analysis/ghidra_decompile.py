"""PyGhidra: decompile ESP32 app and export key functions + string xrefs.

Loads factory.bin with the correct ESP32 memory map, analyzes, then:
  - decompiles the entry point + its call tree (init flow)
  - finds functions that reference subsystem keywords and decompiles them
  - exports a string -> referencing-functions map

Usage: GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
       uv run python analysis/ghidra_decompile.py
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
magic, nseg = data[0], data[1]
entry = struct.unpack("<I", data[4:8])[0]
segs = []
p = 24
for i in range(nseg):
    addr, size = struct.unpack("<II", data[p:p + 8])
    segs.append((addr, size, p + 8))
    p += 8 + size

# subsystem keyword -> tag (functions referencing these strings get decompiled)
KEYWORDS = {
    "cr95hf": "nfc", "_cr95hf": "nfc", "NDEF": "nfc", "ndef": "nfc",
    "sdmmc": "sd", "sdcard": "sd", "preload": "sd", "fatfs": "sd",
    "ht16d35x": "display", "gc9306": "display", "DISPLAY": "display",
    "i2s": "audio", "es8388": "audio", "aw881xx": "audio", "es8156": "audio",
    "cw2215": "power", "sgm415": "power", "husb238": "power", "cv8013": "power",
    "gpio_config": "gpio", "gpio_set": "gpio",
    "app_main": "init",
}


def decompile(decomp, func, monitor) -> str:
    res = decomp.decompileFunction(func, 60, monitor)
    df = res.getDecompiledFunction()
    return df.getC() if df is not None else f"/* failed: {res.getErrorMessage()} */"


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

    # drop the raw binary block so analysis only sees the correct segments
    raw_blk = mem.getBlock(program.getImageBase())
    if raw_blk is not None and raw_blk.getStart().getOffset() != (0x40080000 & 0xFFFFFFFF):
        mem.removeBlock(raw_blk, monitor)

    eaddr = space.getAddress(entry & 0xFFFFFFFF)
    flat.createLabel(eaddr, "entry", True)
    flat.disassemble(eaddr)
    flat.addEntryPoint(eaddr)
    flat.analyzeAll(program)

    fm = program.getFunctionManager()
    from ghidra.app.decompiler import DecompInterface
    # (monitor already defined above)
    decomp = DecompInterface()
    decomp.openProgram(program)

    funcs = list(fm.getFunctions(True))
    by_addr = {f.getEntryPoint().getOffset(): f for f in funcs}
    print(f"functions: {len(funcs)}")

    # --- 1. decompile entry + call tree (init flow) ---
    init = {}
    frontier = [entry]
    seen = set()
    depth = 0
    while frontier and depth < 3:
        nxt = []
        for a in frontier:
            if a in seen or a not in by_addr:
                continue
            seen.add(a)
            f = by_addr[a]
            c = decompile(decomp, f, monitor)
            name = f.getName()
            init[name] = {"addr": a, "code": c}
            # collect call targets
            for ref in f.getCalledFunctions(monitor):
                ca = ref.getEntryPoint().getOffset()
                if ca not in seen:
                    nxt.append(ca)
        frontier = nxt
        depth += 1
    (ROOT / "output" / "decompiled" / "init_flow.json").write_text(
        json.dumps({k: v["addr"] for k, v in init.items()}, indent=2))
    # write C files
    for name, v in init.items():
        (OUT / f"init_{name}.c").write_text(f"// {name} @ {v['addr']:#x}\n{v['code']}")
    print(f"init flow: decompiled {len(init)} functions (depth<=3)")

    # --- 2. string xrefs -> functions, decompile keyword functions ---
    # build a lowercased string -> address map from Ghidra's defined strings
    sm = program.getListing()
    from ghidra.program.model.data import StringDataType
    str_iter = sm.getDefinedData(True)
    xref_map = {}
    nstr = 0
    for d in str_iter:
        if d is None:
            continue
        try:
            val = d.getValue()
        except Exception:
            continue
        if not isinstance(val, str):
            continue
        nstr += 1
        low = val.lower()
        if any(k in low for k in KEYWORDS):
            addr = d.getAddress().getOffset()
            refs = []
            for r in program.getReferenceManager().getReferencesTo(d.getAddress()):
                refs.append(r.getFromAddress().getOffset())
            if refs:
                xref_map.setdefault(val, {"addr": addr, "refs": refs})

    (ROOT / "output" / "string_xrefs.json").write_text(json.dumps(xref_map, indent=2))
    print(f"strings scanned: {nstr}, keyword strings with xrefs: {len(xref_map)}")

    # decompile unique referencing functions
    decompiled = {}
    for val, info in xref_map.items():
        for from_addr in info["refs"]:
            if from_addr in by_addr and from_addr not in decompiled:
                f = by_addr[from_addr]
                decompiled[from_addr] = {
                    "addr": from_addr,
                    "name": f.getName(),
                    "code": decompile(decomp, f, monitor),
                    "strings": [val],
                }
            elif from_addr in decompiled:
                decompiled[from_addr]["strings"].append(val)

    (ROOT / "output" / "keyword_functions.json").write_text(json.dumps(decompiled))
    for addr, info in decompiled.items():
        fn = f"{info['name']}_{addr:#x}".replace("/", "_")
        (OUT / f"kw_{fn}.c").write_text(
            f"// {info['name']} @ {addr:#x} refs: {info['strings']}\n{info['code']}")
    print(f"decompiled {len(decompiled)} keyword-referencing functions")
    print("done")
