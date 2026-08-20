---
icon: lucide/binary
---

# Decompilation Guide & Tooling

Reference for AI agents doing further reverse-engineering of
`yoto-firmware.bin`. This is the **how-to** companion to
[Methodology](../methodology.md) (the narrative) and the per-subsystem pages
under [subsystems/](../subsystems/). Every command, address, and string below is
the exact value from the repo, not a guess.

## Target at a glance

| Attribute | Value | Source |
|-----------|-------|--------|
| Raw flash dump | `yoto-firmware.bin` (8,388,608 B = 8 MiB) | repo root |
| SoC | **ESP32** (Xtensa LX6), chip_id `0x0000` | `output/layout.json` `images.factory.chip_id` |
| Image header magic | `0xE9` (byte 0 of app image) | `analysis/ghidra_esp32.py:38` |
| Framework | **ESP-IDF v5.1.4-dirty** + **ESP-ADF** | `output/strings.txt:1` |
| App entry point | **`0x400813a8`** (`call_start_cpu0`, IRAM) | `output/layout.json` `images.factory.entry` |
| App image | factory partition @ flash `0x40000`, 2,454,473 B, 7 segments | `output/layout.json` |

## Environment & dependencies

Python is managed with [`uv`](https://docs.astral.sh/uv/). `.python-version` =
`3.13`; dependencies in `pyproject.toml`:

```toml
requires-python = ">=3.13"
dependencies = [
    "esptool>=5.3.1",
    "pyghidra>=3.1.0",
    "zensical>=0.0.55",
]
```

```bash
# one-time setup (creates .venv/ and uv.lock; both already exist)
uv sync

# Ghidra (optional, for C pseudocode only) is installed via Homebrew and
# located by pyghidra via this env var:
export GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec
```

Disassembly tooling (no extra install — ships with the ESP-IDF toolchain):

| Tool | Role |
|------|------|
| `xtensa-esp32-elf-objdump` | **primary disassembler**; raw segments at real vaddrs via `--adjust-vma`; literal values printed inline make `l32r` string xrefs work (see 4c) |

Third-party GUI tools (optional, not in `pyproject.toml`):

| Tool | Plugin | Role |
|------|--------|------|
| Binary Ninja | `bnesp32` (**ESPFirmware** BinaryView) | auto-detects chip, creates segments at real addresses, loads ~2000 ESP32 ROM symbols |
| Binary Ninja | `binja-xtensa` (Xtensa arch) | decompilation; BN Personal has no built-in Xtensa |
| Ghidra 12.0.4 | built-in `Xtensa:LE:32:default` SLEIGH | optional C-level decompilation (much faster than BN's Python Xtensa) |

## Pipeline

```mermaid
graph TD
  A[yoto-firmware.bin<br/>8 MiB] --> B[extract_app.py]
  B --> C[output/factory.bin<br/>2.45 MB app image]
  B --> D[output/layout.json<br/>partitions + 7 segments]
  C --> E[extract_strings.py]
  C --> F[extract_hwconfig.py]
  E --> G[output/strings.txt<br/>25,732 unique]
  F --> H[output/hwconfig_00..05_*.json]
  F --> I[output/pinmap.json]
  C --> J[xtensa-esp32-elf-objdump<br/>segments at real vaddrs]
  J --> K[/tmp/irom.dis<br/>linear disassembly + l32r xrefs]
  C --> L[bn_load.py / ESPFirmware (optional)]
  L --> M[output/yoto_factory.bndb]
  C --> N[ghidra_dump.py + ghidra_decompile.py / PyGhidra (optional)]
  N --> O[output/decompiled/*.c<br/>output/decompiled_manifest.json]
```

## Step 1 — Partition & app-image extraction

`analysis/extract_app.py` parses the **partition table** and dumps each
partition, then parses the app-image header. (`esptool` is installed but the
extraction uses this custom parser, which records exact segment metadata that
esptool's CLI does not emit.)

```bash
uv run python analysis/extract_app.py yoto-firmware.bin
# usage: uv run python analysis/extract_app.py [flash.bin]  (defaults to yoto-firmware.bin)
```

Partition table location: flash `0x8000`, 32-byte entries, magic `AA 50`
(`analysis/extract_app.py:33-49`). Full table:

| Label | Type/subtype | Flash offset | Size | Status |
|-------|--------------|--------------|------|--------|
| `nvs` | data / nvs | `0x009000` | 192 KB | used |
| `otadata` | data / ota | `0x039000` | 8 KB | used |
| `phy_init` | data / phy | `0x03b000` | 4 KB | used |
| **`factory`** | app / factory | **`0x040000`** | 2.5 MB | **booted** |
| `ota_0` | app / ota_0 | `0x2c0000` | 2.5 MB | empty (0xFF) |
| `ota_1` | app / ota_1 | `0x540000` | 2.5 MB | empty (0xFF) |

Outputs: `output/<label>.bin` (one per partition) and `output/layout.json`.
The app image (`output/factory.bin`) has its header at offset 0 and 7 segments
(`output/layout.json` `images.factory.segment_map`).

## Step 2 — ESP32 memory map

The app image header (24 bytes) describes the segment map. Decode: byte 0
magic `0xE9`, byte 1 = `nseg`, `entry` @ +4 (LE u32), `chip_id` @ +12 (LE u16),
then 7 × (`load_addr`, `size`) pairs at +24. This exact table is in
`output/layout.json` `images.factory.segment_map` (decimal) — hex here:

| idx | Load address | Size | Region | File offset* |
|-----|--------------|------|--------|--------------|
| 0 | `0x3F400020` | 680,880 (0xA63B0) | DROM (flash rodata) | 32 |
| 1 | `0x3FF80063` | 8 | RTC_DRAM (fast) | 680,920 |
| 2 | `0x3FFBDB60` | 30,648 (0x77B8) | DRAM | 680,936 |
| 3 | `0x40080000` | 9,328 (0x2470) | IRAM (entry) | 711,592 |
| 4 | `0x400D0020` | 1,634,508 (0x18F0CC) | IROM (flash code) | 720,928 |
| 5 | `0x40082470` | 98,888 (0x18248) | IRAM | 2,355,444 |
| 6 | `0x400C0000` | 100 (0x64) | RTC_IRAM (fast) | 2,454,340 |

*`file_offset` is relative to the factory partition; absolute flash offset =
`0x40000 + file_offset`. Entry point `0x400813a8` is inside segment 3 (IRAM).

Standard ESP32 address space (for interpreting further addresses — standard
SoC map, not per-image data):

| Range | Region |
|-------|--------|
| `0x40000000–0x40070000` | ROM (mask ROM — NOT in app image; ~2000 symbols loaded by BN) |
| `0x40080000–0x4009FFFF` | IRAM (internal SRAM 1, 128 KB) |
| `0x400C0000–0x400C1FFF` | RTC fast IRAM |
| `0x400D0000–0x40400000` | IROM (flash-mapped code, via cache) |
| `0x3F400000–0x3F800000` | DROM (flash-mapped read-only data, via cache) |
| `0x3FF80000–0x3FF81FFF` | RTC fast DRAM |
| `0x3FFAE000–0x3FFDFFFF` | DRAM (internal SRAM 2) |

The three code regions used by the dump scripts
(`analysis/ghidra_dump.py:33-37`) are: **IRAM** `0x40080000–0x400A0000`,
**RTC_IRAM** `0x400C0000–0x400C2000`, **IROM** `0x400D0000–0x40400000`.

## Step 3 — Binary Ninja load (ESPFirmware + Xtensa)

```bash
# inside Binary Ninja's Python console / command palette:
bn py exec --script analysis/bn_load.py
```

`analysis/bn_load.py` loads `output/factory.bin`, prints arch/platform/segments,
counts ROM symbols (`address < 0x40070000`), and saves
`output/yoto_factory.bndb` **without** running analysis (line 15 uses
`options={"analysis.mode": "off"}`). For a correct load use the **`ESPFirmware`**
BinaryView type (community `bnesp32`) with the **Xtensa** arch plugin
(`binja-xtensa`); a plain `bninja.load()` (no view type) will NOT auto-detect an
ESP32 image — it lands flat at image base 0.

Binary Ninja's value: it loads ~2,000 ROM symbols (`< 0x40070000`) that Ghidra's
raw-binary load does not, giving real names to ROM helper calls. Ghidra remains
the primary decompiler (speed + SLEIGH Xtensa quality).

## Step 4 — Xtensa disassembly with the Espressif objdump (primary)

The ESP-IDF toolchain ships an Xtensa-capable objdump; it is the default
disassembler for this repo — **no Ghidra or Binary Ninja needed**. Location:

```bash
# on PATH after sourcing ESP-IDF:
source $IDF_PATH/export.sh
which xtensa-esp32-elf-objdump
# or directly:
ls ~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump
```

### 4a. Disassemble a whole segment

Slice the code segment out of `output/factory.bin` at its file offset and
disassemble with `--adjust-vma` set to the segment's load address (otherwise
objdump starts at address 0):

```bash
# IROM (flash code): vaddr 0x400D0020, file offset 0xB0020, size 0x18F0CC
python3 -c "d=open('output/factory.bin','rb').read(); \
  open('/tmp/irom.bin','wb').write(d[0xB0020:0xB0020+0x18F0CC])"
xtensa-esp32-elf-objdump -D -b binary -m xtensa --adjust-vma=0x400D0020 \
  /tmp/irom.bin > /tmp/irom.dis
# 625k lines; IRAM/RTC_IRAM segments likewise (see the segment table above)
```

### 4b. Disassemble one function cleanly

Linear decode **drifts at embedded literal pools** (literals are decoded as
instructions — expect garbage like `mul16u`/`orb`/`ill` between functions).
Always re-anchor a window at the function's own address:

```bash
# function at vaddr 0x4010854c -> file offset 0xB0020 + (0x4010854c - 0x400D0020)
python3 -c "d=open('output/factory.bin','rb').read(); \
  open('/tmp/fn.bin','wb').write(d[0xE854C:0xE854C+0x300])"
xtensa-esp32-elf-objdump -D -b binary -m xtensa --adjust-vma=0x4010854c \
  /tmp/fn.bin
```

### 4c. Xtensa reading notes (verified against this image)

- **Functions open with `entry a1, <frame>`**; a bare `retw.n` ends one. A
  function boundary that does not start with `entry` in the linear decode is
  a misaligned literal pool — re-anchor.
- **Windowed ABI**: `call8` shifts the register window, so the caller's
  `a10/a11/...` become the callee's `a2/a3/...`. A call's arguments are the
  `mov`/`movi` instructions *immediately before* the `call8`.
- **`l32r aN, <lit_addr> (<value>)`**: objdump prints the loaded 32-bit value
  in parentheses. String/data references therefore show up as
  `l32r aN, ... (0x3f4xxxxx)` — `grep '(0x3f4' irom.dis` is a working
  **string xref** (this is exactly what Ghidra's Xtensa cannot do — see the
  L32R caveat below).
- **`call8` targets are 4-byte aligned**; a target that looks misaligned in
  the linear decode is a decode-drift artifact, not a real call.
- Calls with literal constants (e.g. `movi a11, 72` before `call8`) expose
  command/data bytes directly — the GC9306 init table in
  `firmware/components/gc9306/gc9306.c` was recovered this way.

## Step 5 — Ghidra / PyGhidra: memory-map + dump (optional)

Ghidra is only needed for C-level pseudocode. Both entry points must
**create the segments at their real addresses** (a flat binary load defaults
to image base 0, which is wrong).

### 4a. Headless (Java scripts)

```bash
GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec

# pre-script form (creates segments + entry label before auto-analysis):
analyzeHeadless <project_dir> <project_name> \
  -import output/factory.bin \
  -loader ghidra.app.util.opinion.BinaryLoader \
  -processor Xtensa:LE:32:default \
  -preScript analysis/MapESP32.java

# post-script form (Tools.ESP32.MapImage — same mapping, run after load):
analyzeHeadless <project_dir> <project_name> \
  -import output/factory.bin \
  -loader ghidra.app.util.opinion.BinaryLoader \
  -processor Xtensa:LE:32:default \
  -postScript analysis/ghidra_esp32.py
```

Both scripts parse the 0xE9 header, read the 7 segments, `createBlock("segN",
addr, size, ...)` at real addresses, and label the entry point
(`analysis/MapESP32.java:25-54`, `analysis/ghidra_esp32.py:37-79`).

### 4b. PyGhidra (bulk decompile — the canonical path)

```bash
# dump all code functions >= 40 bytes (largest-first, cap 6000) to C:
GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
  uv run python analysis/ghidra_dump.py

# decompile entry call-tree (depth 3) + keyword string-xref functions:
GHIDRA_INSTALL_DIR=/opt/homebrew/Cellar/ghidra/12.0.4/libexec \
  uv run python analysis/ghidra_decompile.py
```

What each produces:

| Script | Outputs |
|--------|---------|
| `ghidra_dump.py` | `output/decompiled/*.c` (5,915 files, `FUN_segN__ADDR.c`), `output/decompiled_manifest.json` |
| `ghidra_decompile.py` | `output/decompiled/init_flow.json`, `output/decompiled/init_*.c`, `output/string_xrefs.json`, `output/keyword_functions.json` |

The load recipe (both PyGhidra scripts, identical):

```python
open_program(
    str(APP), language="Xtensa:LE:32:default", compiler="default",
    loader="ghidra.app.util.opinion.BinaryLoader", analyze=False,
)
# then for each (addr, size, off): flat.createMemoryBlock(f"seg{i}", start, data[off:off+size], True)
# then remove the flat raw block, label entry, flat.analyzeAll(program)
```

(`analysis/ghidra_dump.py:46-73`, `analysis/ghidra_decompile.py:51-79`.)

## Step 6 — Strings & hardware config

```bash
uv run python analysis/extract_strings.py     # -> output/strings.txt, strings_categorized.json
uv run python analysis/extract_hwconfig.py    # -> output/hwconfig_00..05_*.json, pinmap.json
```

`extract_strings.py` scans for printable ASCII runs (≥4 chars) and categorizes
by subsystem keyword (first-match). `extract_hwconfig.py` finds complete JSON
docs anchored on `"battery"` and flattens every `GPIO.x` / `ADC.x` /
`IOX.p.n` assignment. The **six hardware configs are the device's own per-revision
pin map** — more authoritative than decompiled `gpio_config()` calls. The six
revisions (recovered order, not proven chronology) are:

| Config file (`output/`) | Display | NFC | SD | Fuel gauge |
|-------------------------|---------|-----|----|------------|
| `hwconfig_00_...` | GC9306 TFT | UART | SDMMC 1-bit | CW2015 |
| `hwconfig_01_...` | HT16D35x LED | SPI | SPI | ADC |
| `hwconfig_02_...` | HT16D35x LED | UART | SDMMC 1-bit | CW2015 |
| `hwconfig_03_...` | *(incomplete)* | — | — | — |
| `hwconfig_04_...` | GC9306 TFT | UART | SDMMC 4-bit | CW2215B |
| `hwconfig_05_...` | HT16D35x LED | UART | SDMMC 1-bit | CW2215B |

Full pin tables (GPIO/ADC/IOX, per revision) are in [Hardware & Ports](../hardware.md); this page only covers the tooling.

## The L32R caveat (critical — and how to work around it)

On ESP32, immediate 32-bit constants (including string/data pointers) are
loaded via **`L32R`** from a literal pool embedded in the code segment.

**Workaround (preferred): use the Espressif objdump, not Ghidra.** objdump
prints the literal's *value* inline — `l32r a13, 0x400d4f04 (0x3f41ce65)` —
so `grep '(0x3f4'` on the disassembly finds every code→string reference.
This is how driver functions are located: find the string in DROM
(vaddr = `0x3F400000 + file_offset` within segment 0), grep the value, and
the matching `l32r` sits in the referencing function.

Ghidra's generic Xtensa SLEIGH does **not** resolve L32R literal-pool
references, so its string→function xrefs are unreliable. Consequence,
empirically: the string-xref pass in `ghidra_decompile.py` found exactly
**one** keyword string with a reference, and that reference is a data→data
pointer, not code→data:

```json
// output/string_xrefs.json (entire contents)
{
  "PRELOADED_REP": { "addr": 1061609160, "refs": [1061612432] }
}
```

(`1061609160` = `0x3F46DEC8`; `1061612432` = `0x3F46EB90` — both in DROM.)

And `output/keyword_functions.json` is empty `{}`. So:

- **Function names are auto-generated** (`FUN_segN__ADDR`) — no symbol names.
- **String→function xrefs are unreliable** — do NOT rely on xrefs to locate a
  driver; use `output/strings.txt` + `output/hwconfig_*.json` as primary ground
  truth, then match by address ranges / called ROM symbols.
- The entry-point call tree also stalls: `init_flow.json` contains only 4 IRAM
  functions (below) and never reaches `app_main` (which lives in IROM and is
  dispatched through a function pointer in the literal pool).

## Function address list

### Entry / boot stubs (IRAM) — verified

From `output/decompiled/init_flow.json` (decimal → hex) and the corresponding
`init_*.c` files:

| Address | Ghidra name | Likely role |
|---------|-------------|-------------|
| `0x400813a8` | `FUN_seg3.7__400813a8` | **App entry = `call_start_cpu0`** (confirmed by `layout.json` `entry`). Decompiled: sets vector base (`wsr VECBASE`), calls the ROM function-pointer table, zeroes `.bss` (memcpy via `DAT_...400804c4`), dispatches through ROM. |
| `0x400812f4` | `FUN_seg3.7__400812f4` | `start_cpu0` (per `boot.md`): cache/clock config; bit-twiddles IRAM-mapped registers at `0x40080458+` with `memw()`. |
| `0x400820b0` | `FUN_seg3.7__400820b0` | Cache/ROM init helper: busy-waits on a cache status register (`+0x1c`) until idle. |
| `0x40082104` | `FUN_seg3.7__40082104` | SPI-flash clock/divider setup: computes `(param_2 << 4) / param_3` and programs the flash clock control register. |

`boot.md` additionally names ROM/cache helpers `0x400988fc`, `0x400881bc`,
`0x40090b88` (all IRAM) and notes auto-analysis identified **81,667 functions**
across IRAM/IROM/DROM. *[INFERENCE]* — the 4 rows above are the only functions
directly verified in this repo's `init_flow.json`; the three `boot.md` addresses
are cross-referenced, not re-verified here.

### Largest decompiled functions (IROM) — unidentified

Top 5 by size from `output/decompiled_manifest.json` (5,915 total, real
addresses `0x400811F8..0x4025EE24`):

| Address | Size (B) | Note |
|---------|----------|------|
| `0x4020DFF4` | 8,067 | largest code function *[INFERENCE: likely a decoder/UI state machine]* |
| `0x40101638` | 7,408 | — |
| `0x4010AB44` | 5,265 | — |
| `0x401F645C` | 4,904 | — |
| `0x400E193C` | 4,624 | — |

Names are auto-generated; purpose unknown (see L32R caveat — no string xrefs to
identify them).

### Two function lists — DIFFERENT address bases (trap)

| File | Entries | Address base |
|------|---------|--------------|
| `output/ghidra_functions.json` | 9,185 | **file offsets** (966..2,454,304) — flat load at image base 0 |
| `output/decompiled_manifest.json` | 5,915 | **real segment addresses** (`0x400811F8..0x4025EE24`) |

`ghidra_functions.json` is NOT produced by any `analysis/*.py` script (grep
confirms); it is a pre-existing flat-load export (`FUN_000003c6` etc.). Do not
treat its addresses as real code addresses — add the segment base to remap.
`decompiled_manifest.json` is the authoritative, correctly-mapped list.

## Exact strings (toolchain-relevant)

Line numbers from `output/strings.txt` (25,732 unique lines):

| Line | String | Meaning |
|------|--------|---------|
| 1 | `v5.1.4-dirty` | ESP-IDF version (printed by line 18) |
| 2 | `cpu_start` | ROM/boot symbol |
| 18 | `I (%lu) %s: ESP-IDF:          %s` | boot log — IDF version banner |
| 17 | `I (%lu) %s: ELF file SHA256:  %s...` | app SHA banner |
| 169 | `I (%lu) %s: Calling app_main()` | entry into app code |
| 170 | `I (%lu) %s: Returned from app_main()` | app_main return |
| 210 | `phy_init` | PHY init partition label |
| 4173 | `nvs.net80211` | Wi-Fi NVS namespace |
| 5493 | `esp_image` | bootloader image loader tag |
| 5492 | `E (%lu) %s: spi_flash_mmap failed: 0x%x` | flash mmap error (segment map) |
| 5501 | `I (%lu) %s: segment %d: paddr=%08lx vaddr=%08x size=%05lxh (%6lu) %s` | bootloader segment dump |
| 8513 | `d7b0a45ddbddbac53afb4fc28168f9f9259dbb79` | **IDF SHA** (printed by line 8514) |
| 8514 | `IDF SHA: %s` | IDF commit |
| 8515 | `49f80aafefc31642ea98db78bf024e18688b8de9` | **ADF SHA** (printed by line 8516) |
| 8516 | `ADF SHA: %s` | ADF commit |
| 8517 | `ERROR: failed to load bootloader image header` | second-stage bootloader error |
| 8509 | `This is the Yoto factory console.` | factory test console banner |
| 8523 | `esp32> ` | console prompt |
| 481 | `//opt/atlassian/pipelines/agent/build/components/my_board/yoto_v2/board_pins_config.c` | board pin config source path |
| 482 | `YOTO_V2_0` | board identity string |
| 837 | `//opt/atlassian/pipelines/agent/build/components/my_board/yoto_v2/board.c` | board init source path |
| 838 | `AUDIO_BOARD` | ADF audio-board tag |
| 1397 | `//opt/atlassian/pipelines/agent/build/components/yoto_app/walkman.c` | app UI source path |
| 5992 | `espressif` | vendor string |
| 10342 | `Please enter IDF-PATH with "cd $IDF_PATH" and apply the IDF patch with "git apply $ADF_PATH/idf_patches/idf_%.4s_freertos.patch" first` | ADF↔IDF patch instruction |
| 10344 | `/opt/esp/adf/components/audio_sal/audio_thread.c` | ADF build path |
| 10614 | `/builds/idf/crosstool-NG/.build/xtensa-esp32-elf/src/newlib/newlib/libc/time/lcltime.c` | **toolchain = xtensa-esp32-elf (crosstool-NG)** |
| 110 | `Backtrace:` | panic backtrace marker |
| 113 | `Guru Meditation Error: Core` | panic handler |
| 11222 | `abort() was called at PC 0x` | abort trace |

Build provenance: compiled on an Atlassian Bitbucket Pipelines agent
(`/opt/atlassian/pipelines/agent/build/...`), IDF toolchain
`xtensa-esp32-elf` via crosstool-NG at `/builds/idf/crosstool-NG`, ADF at
`/opt/esp/adf/...`.

## Artifact locations

| Path | Content | Producer |
|------|---------|----------|
| `yoto-firmware.bin` | 8 MiB raw flash dump | input |
| `output/factory.bin` | 2.45 MB factory app image (header @ 0) | `extract_app.py` |
| `output/{nvs,otadata,phy_init,ota_0,ota_1}.bin` | per-partition dumps | `extract_app.py` |
| `output/layout.json` | partition table + 7-segment map + entry | `extract_app.py` |
| `output/strings.txt` | 25,732 unique printable strings | `extract_strings.py` |
| `output/strings_categorized.json` | strings grouped by subsystem | `extract_strings.py` |
| `output/hwconfig_00..05_*.json` | six per-revision hardware configs | `extract_hwconfig.py` |
| `output/pinmap.json` | flattened `GPIO.x`/`ADC.x`/`IOX.p.n` → function | `extract_hwconfig.py` |
| `output/decompiled_manifest.json` | 5,915 code funcs (addr/name/size/file) — real addrs | `ghidra_dump.py` |
| `output/decompiled/*.c` | 5,915 decompiled C files (`FUN_segN__ADDR.c`) | `ghidra_dump.py` |
| `output/decompiled/init_flow.json` | entry call-tree (4 IRAM funcs) | `ghidra_decompile.py` |
| `output/decompiled/init_*.c` | decompiled entry stubs | `ghidra_decompile.py` |
| `output/string_xrefs.json` | 1 resolved string xref (L32R caveat) | `ghidra_decompile.py` |
| `output/keyword_functions.json` | empty `{}` (xrefs unresolved) | `ghidra_decompile.py` |
| `output/ghidra_functions.json` | 9,185 funcs, **file-offset** addrs (external) | *(not in repo scripts)* |
| `output/yoto_factory.bndb` | Binary Ninja DB (ESPFirmware view) | `bn_load.py` |
| `analysis/*.py`, `*.java` | extraction + Ghidra/BIN scripts | — |

## Working notes

- **Remap `ghidra_functions.json`** before use: its addresses are file offsets;
  `decompiled_manifest.json` has the correct segment addresses.
- **Identify drivers by strings + config, not xrefs.** The L32R caveat means
  `string_xrefs.json`/`keyword_functions.json` are effectively empty.
- **`app_main` is not in `init_flow.json`.** To reach it, resolve the ROM
  function-pointer table (the entry decompilation shows the dispatch) or search
  IROM (`0x400D0000–0x40400000`) directly.
- **Ghidra is the bulk decompiler; Binary Ninja supplies ROM symbol names.**
  Cross-check Ghidra output against BN's ~2,000 ROM symbols (`< 0x40070000`).
