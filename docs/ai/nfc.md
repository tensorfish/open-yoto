---
icon: lucide/contact-round
---

# NFC / RFID — ST CR95HF

Dense reference for AI agents reverse-engineering the Yoto ESP32 firmware NFC subsystem.

**Chip:** ST CR95HF NFC/RFID transceiver (13.56 MHz, ISO14443-A/B, ISO15693, NFC Forum Type 2).
Identified in strings.txt:232 `Initialising CR95HF NFC chip` and the entire `_cr95hf_*` driver
family (strings.txt:3168–3295). This is the **card-reader** for Yoto audio content cards:
each card is a **NFC Forum Type 2 tag** holding a **UID** and an **NDEF URI** (`https://yoto.io/<id>`),
which the firmware resolves to a content playlist.

---

## 1. Transport: UART vs SPI (revision-dependent)

The CR95HF is driven over **either UART or SPI**, selected by the per-revision `nfc` block in
`output/hwconfig_*.json` (see §2). The driver/HAL stack is identical; only the physical transport
and its pins differ. The transport interface is abstracted through a **function pointer** stored in
the driver data block (see §4, `0x400d5444` → `0x4024bac4`).

Strings confirming both paths:
- `strings.txt:3168` `UART`
- `strings.txt:3248` `Reset over UART using echo command after %u attempts`
- `strings.txt:3249` `Failed to reset over UART`
- `strings.txt:3250` `Too much data in UART buffer - limiting read to avoid RX overflow`
- `strings.txt:3275` `Unexpected NFC setup params (type %d, rx type %d, tx type %d`
- `strings.txt:3276` `Unknown NFC Interface`
- `strings.txt:3500` `Unknown NFC Interface Type`

UART reset is done by sending the **Echo (0x55)** command and expecting `Echo Received.`
(`strings.txt:3198`), `Echo Sent` (`3278`); driver messages `SENT RESET: 0x01` (`3170`) /
`Resetting NFC module` (`3279`).

---

## 2. Component / pin table (all 6 variants)

`output/pinmap.json` flattened mapping + `output/hwconfig_0[0-5]_*.json` source blocks.
**Note:** GPIO.32/GPIO.33 are *UART NFC* in most revisions, but are re-purposed as I2S audio in the
SPI-NFC revision. The SPI bus is a shared `spi` block (mosi/miso/sclk).

| Variant (file) | NFC type | Pins |
|---|---|---|
| `hwconfig_00_…_nfcuart_sdsd1_batCW2015.json` | `uart` | rx=`GPIO.32`, tx=`GPIO.33` |
| `hwconfig_01_…_nfcspi_sdspi_batADC.json` | `spi` | cs=`IOX.1.4`, irqin=`IOX.0.7`, irqout=`IOX.1.0` |
| `hwconfig_02_…_nfcuart_sdsd1_batCW2015.json` | `uart` | rx=`GPIO.32`, tx=`GPIO.33` |
| `hwconfig_03_audio?_…_nfc?_…json` | *(absent)* | no `nfc` block (unpopulated variant) |
| `hwconfig_04_…_nfcuart_sdsd4_batCW2215B.json` | `uart` | rx=`GPIO.32`, tx=`GPIO.33` |
| `hwconfig_05_…_nfcuart_sdsd1_batCW2215B.json` | `uart` | rx=`GPIO.32`, tx=`GPIO.33` |

Exact JSON blocks:

- UART variant (`hwconfig_00/02/04/05`, identical):
  ```json
  "nfc": { "type": "uart", "rx": "GPIO.32", "tx": "GPIO.33" }
  ```
- SPI variant (`hwconfig_01`, file lines 104–109):
  ```json
  "nfc": { "type": "spi", "cs": "IOX.1.4", "irqin": "IOX.0.7", "irqout": "IOX.1.0" }
  ```
- SPI bus (shared) — `hwconfig_01` lines 62–67:
  ```json
  "spi": { "comment": "Do we add speed?", "mosi": "GPIO.18", "miso": "GPIO.21", "sclk": "GPIO.2" }
  ```

**Important cross-revision collision:** in `hwconfig_01` (SPI NFC), `GPIO.32`/`GPIO.33` are
`audio.i2s.lrclk` / `audio.i2s.out` (pinmap.json:167–172), and `GPIO.34` = `sd.miso` (194–196).
The SPI-NFC build therefore uses the IO-expander pins for NFC CS/IRQ, freeing 32/33 for I2S.

`pinmap.json` keys (flattened): `nfc.rx`, `nfc.tx`, `nfc.cs`, `nfc.irqin`, `nfc.irqout`.

---

## 3. CR95HF driver protocol flow

Driver family names come from the format strings (Ghidra does **not** resolve Xtensa literal-pool
string refs, so names below are reconstructed from `strings.txt` + decompiled C; role mappings are
`[INFERENCE]` unless noted).

### 3.1 CR95HF host command bytes (datasheet + code)

| Byte | Command | Evidence |
|---|---|---|
| `0x01` | IDN / reset | `strings.txt:3170` `SENT RESET: 0x01` |
| `0x02` | Protocol Select | `_cr95hf_Select_protocol()` (`3196–3197`) |
| `0x04` | SendRecv (raw tag frame) | decompiled `FUN_seg4__4010d270`: `buf[1] = 4` |
| `0x55` | Echo | `3198`/`3278`; UART resync via echo |
| `0x0E` | error response code | `timeout: 0x0E0x87` (`3174`) |
| `0x87` | no-response / frame-waiting timeout | `timeout: 0x87` (`3175`) |

Frame layout (SendRecv, from `FUN_seg4__4010d270`, decompiled C):
`[0]=0x00 (control=cmd)`, `[1]=0x04 (SendRecv)`, `[2]=len`, `[3..]=tag frame`.

### 3.2 ISO14443-A tag command bytes (initialized DRAM, seg2)

Tag command buffers live in initialized DRAM at `0x3ffbf8c0–0x3ffbf8f0` (seg2 `0x3ffbdb60`).
Observed values (raw hex dump of seg2):

- `0x26` **REQA** (at `0x3ffbf8ee`, pointed to by driver data `0x400d5450`)
- `0x52` **WUPA** (at `0x3ffbf8ec`)
- `0x93 0x20` **ANTICOLLISION / Select cascade L1** (at `0x3ffbf8d9`)
- `0x93 0x70` **SELECT cascade L1** (at `0x3ffbf8d5`)
- `0x95 0x20` / `0x95 0x70` **Select L2** (at `0x3ffbf8d0`)

### 3.3 Anti-collision / select sequence (Type A)

Reconstructed from strings + the `FUN_seg4__4010d2ac`/`4010d458` decompilation:

1. **REQA** (`_cr95hf_drv_reqa`) — send `0x26`, expect ATQA. Strings: `_cr95hf_drv_reqa() : REQA failed` (3176–3177), `_cr95hf_drv_reqa() timeout` (3174–3175).
2. **WUPA** (reset card) — `Wupa error, failed to reset card` (3178).
3. **ANTICOLLISION L1** (`_cr95hf_drv_antic1`) — `0x93 0x20`; strings 3181–3186.
4. **SELECT L1** (`_cr95hf_drv_select1`) — `0x93 0x70` + 4-byte UID + BCC; string 3187.
5. **ANTICOLLISION L2 / SELECT L2** (`_cr95hf_drv_antic2`/`select2`) — `0x95`; strings 3188–3192.
6. **Read A0/A3** (`_cr95hf_read_card` `Read A0/A3 failed`, 3193/3206) — reads tag memory blocks.

Driver tuning calls: `_cr95hf_drv_adjust_timew()` (Set TimerW, 3179),
`_cr95hf_drv_modulation_gain()` (Set Mod/Gain, 3180), `_cr95hf_drv_calibration()` (3195),
`_cr95hf_Select_protocol()` (3196–3197).

Card presence: `cr95hf_drv_card_present timeout` (3202), `Card in NFC field != 1` (3204),
`REQA Failed` (3203), `Card is%s present` (3286).

---

## 4. Function addresses (seg4 flash IROM, `output/decompiled_manifest.json`)

Address-space reminder: rodata/strings at `0x3f40xxxx`, app code in IROM `0x400d0020–0x4025f0ec`
(`output/layout.json`, seg4). Decompiled files are named `FUN_seg4__<addr>_<addr>.c` under
`output/decompiled/`.

### 4.1 Driver literal-pool / data block (CR95HF low-level — NOT individually decompiled)

The low-level driver code sits in the IROM gap `0x400d5000–0x400d5534` (Ghidra did not emit
functions for this region; its literal pool + data block are present and referenced by the HAL).
Authoritative string refs were located by scanning the code segment for the 32-bit little-endian
rodata addresses.

| Address | Contents | Evidence |
|---|---|---|
| `0x400d5428` | `DAT` → driver runtime struct ptr (`0x3ffbf8f4`) | `FUN_seg4__4010e4d0` reads it |
| `0x400d5438` | log tag `"NFC driver"` (`0x3f41ec7e`) | literal ref scan |
| `0x400d5444` | **fn ptr → `0x4024bac4`** (transport send/recv) | `PTR_FUN_seg4__4024bac4_seg4__400d5444` |
| `0x400d5450` | ptr → REQA buffer `0x3ffbf8ee` | `FUN_seg4__4010d2ac` uses it |
| `0x400d5454` | `_cr95hf_drv_reqa() timeout` str `0x3f41ed3b` | literal ref |
| `0x400d545c` | `_cr95hf_drv_reqa() : REQA failed` `0x3f41ed9b` | literal ref |
| `0x400d5480` | `_cr95hf_drv_antic1() : Anticollsion 1 failed` `0x3f41eec6` | literal ref |
| `0x400d5484` | `_cr95hf_drv_antic1() response OK` `0x3f41ef00` | literal ref |
| `0x400d54a0` | `_cr95hf_drv_select1() : Select 1 failed` `0x3f41f008` | literal ref |
| `0x400d54d8` | `_cr95hf_Select_protocol() Error` `0x3f41f1de` | literal ref |
| `0x400d5500` | `Card in NFC field != 1` `0x3f41f348` | literal ref |
| `0x400d5504/508/50c` | counter variables (error/read counts) | `FUN_seg4__4010e4d0` increments |
| `0x400d0730` | `Initialising CR95HF NFC chip` `0x3f402162` | literal ref (init) |
| `0x4024bac4` | **transport send/recv** (UART or SPI) | fn ptr target; sits between decompiled `FUN_seg4__4024ba74` (ends 0x4024bac4) and `FUN_seg4__4024bbcc` |

### 4.2 HAL (upper layer) — decompiled, `seg4`

| Address | Size | Likely role | Evidence / confidence |
|---|---|---|---|
| `0x4010d020` | 159 | HAL init helper | [INFERENCE] |
| `0x4010d16c` | 255 | frame send/receive wrapper | `FUN_seg4__4010d270` calls it |
| `0x4010d270` | 58 | **build SendRecv frame** (cmd `0x04`) | decompiled: `buf[1]=4`, `buf[2]=len` |
| `0x4010d2ac` | 282 | **`_cr95hf_drv_reqa`** (REQA/WUPA) | logs REQA strings @0x400d5454/545c; checks `0x0E`/`0x87` |
| `0x4010d3c8` | 47 | short protocol step | returns `0x107` |
| `0x4010d3f8` | 47 | short protocol step | " |
| `0x4010d428` | 47 | short protocol step | " |
| `0x4010d458` | 307 | **ANTICOLLISION / SELECT** (`antic1`/`select1`) | [INFERENCE] |
| `0x4010d598` | 67 | select/read sub-step | [INFERENCE] |
| `0x4010d5dc` | 243 | select/read sub-step | [INFERENCE] |
| `0x4010d6d4` | 67 | select/read sub-step | [INFERENCE] |
| `0x4010d718` | 85 | read-block (CC/TLV) | parses `0xE1 0x10 0x06/0x12` |
| `0x4010db20` | 2477 | **main read-card orchestrator** | largest HAL fn; calls all steps + `0x400f25e0` |
| `0x4010e4d0` | 356 | **read UID** (`nfc_hal_read_carduid`) | reads 7 bytes; logs UID; calls `0x4010d3f8/428/2ac/458` |
| `0x4010fc24` | 1373 | **read card + NDEF URI extraction** | parses CC (`0xE1 0x10`) + NDEF record (`0xD1`+`U`+`0x04`) |

Log wrapper: `FUN_seg4__40116320` (216) = ESP-IDF `esp_log_write` (arg order: level, tag, line,
format, timestamp, tag, …). `FUN_seg4__40116310` = `esp_log_timestamp`.

### 4.3 HAL data / literal block (string refs, all seg4)

| Address | Contents |
|---|---|
| `0x4010e63c` | log tag (same `"NFC driver"` string `0x3f41ec7e`) |
| `0x4010e648` | config/struct ptr |
| `0x4010e65c` | fn ptr → `0x4024bac4` (transport) |
| `0x4010e6c0`, `0x4010e6c8`, `0x4010e7b8`, `0x4010f834`, `0x4010fba0` | fn ptrs (lock/free/read primitives) |
| `0x4010e784` | `Initialise NFC HAL before starting it` `0x3f41fb3e` |
| `0x4010e794` | `nfc_hal_gate` (task name) `0x3f41fbfc` |
| `0x4010e7ac` | `NFC_HAL_TAG` `0x3f41fc44` |
| `0x4010e7bc` | `nfc_hal_read_carduid() : invalid length arguments` `0x3f41fccb` |
| `0x4010e840` | `Resetting NFC module` `0x3f41fe24` |
| `0x4010e8a4` | `nfc_get_card_id` `0x3f42006e` |
| `0x400d3210` | `nfc_poll_task` (task name) `0x3f4120b7` |

Task names / entry points: `nfc_poll_task` (`strings.txt:1922`), `nfc_hal_gate` (`3261`),
`NFC_HAL_TAG` (`3265`), `nfc_get_card_id` (`3295`).

### 4.4 Factory prodtest (NFC test) — seg4

`strings.txt:1576–1586`, `8602–8606`. Literal refs:
`NFC TST` `0x3f40f28b` → `0x400d2918`; `NFC ERR` `0x3f40f366` → `0x400d292c`;
`NFC PASS` `0x3f4177d4` → `0x400d449c` and `0x401c8304`; `NFC N/A` `0x3f417abf` → `0x400d454c`.

---

## 5. Tag structure — NFC Forum Type 2 tag (NTAG21x-class)

Read flow confirmed from `FUN_seg4__4010fc24` decompiled C:

### 5.1 Capability Container (CC) — block 3

Magic/version/size/access bytes. Decompiled check:
`cStack_5d == 0xE1 && cStack_5c == 0x10 && (cStack_5b == 0x06 || cStack_5b == 0x12)`.

- `0xE1` = NFC Forum Type 2 Tag magic (`strings.txt:3211` `Magic  :0x%02X`)
- `0x10` = version 1.0 (`3212` `Ver    :0x%02X`)
- `0x06` = 48 bytes / `0x12` = 144 bytes tag size (`3213` `Size   :0x%02X`)
- access/`Flags` byte (`3214` `Flags  :0x%02X`)

### 5.2 NDEF TLV → URI record

NDEF record header bytes decoded in `FUN_seg4__4010fc24`:
`0xD1 0x01 0x55 0x04` →
- `0xD1` = MB=1 ME=1 SR=1 TNF=1 (Well-Known Type) — `strings.txt:3216` `Tnf`
- `0x01` = type length 1 — `3217` `Typelen`
- `0x55` = type `'U'` (URI) — `3219` `Type`
- `0x04` = URI identifier prefix **`https://`** (RFC NDEF URI code 4)
- then the URI payload (the card path), length via `3218` `Length`

Other strings: `ndeflen:0x%02X` (3215), `Problem reading NDEF record - TNF:0x%02X, TLEN:0x%02X TYPE:0x%02X ID:0x%02X` (3209), `ndef.length too long (%d)` (3210), `Read URL from card: %s` (3221), `Read /card-play URI from card: %s` (3222), `Read invalid URL from card: %s` (3223).

### 5.3 UID

7-byte UID (double-size tag), logged as `UID : %02X %02X %02X %02X %02X %02X %02X` (3205),
formatted `%02x%02x%02x%02x%02x%02x%02x` (1813) into `nfcUid` (1814). Read by `FUN_seg4__4010e4d0`
(7 bytes written to out param). `X-Yoto-Card-UID` HTTP header (2392) → literal `0x400d3e50`.

### 5.4 URL semantics

- Card URL scheme: `https://yoto.io/%s` (8530), action `action NFC %s` (8529).
- Card linking API: `https://api.yotoplay.com/card/%s/linkUrl?uid=%02X%02X%02X%02X%02X%02X%02X&originalUrl=%s` (8446); `UID is filled with zeros!` (8449).
- Play endpoint: `/card-play` (2638), `/nfc` (2627). Card type events: `inserted card type=eCT_RHB_PRESS` (1467), `eCT_ALARM` (1468).

---

## 6. Writing tags

`_cr95hf_write()` family (`strings.txt:3225–3241`): `No text to write` (3225), `--- Writing ---`
(3226), `More than one card detected` (3227), `Normal card` (3228), `Blank card` (3229),
`Problem reading card` (3230), `Step 0..7 : %d` (3231–3238), `RESULT : %d` (3239),
`Total reads` (3240), `Total errors` (3241). `Write A%d failed` (3194), `written %s` (3281),
`read %s` (3282).

Factory write test: `NFC WRITE PASS` (8602), `NFC WRITE FAIL` (8603), `Write <text> to card.` (8604).

`[INFERENCE]` The 8-step `Step 0..7` write sequence writes the CC + NDEF URI record to a Type 2
tag: authenticate/read UID, write CC block, write NDEF TLV, read-back verify, report errors.

---

## 7. Key exact strings (with `output/strings.txt` line numbers)

| Line | String |
|---|---|
| 232 | `I (%lu) %s: Initialising CR95HF NFC chip...` |
| 1429 | `W (%lu) %s: _nfc_url is empty (or invalid) string, so ignoring` |
| 1813–1814 | `%02x%02x%02x%02x%02x%02x%02x` / `nfcUid` |
| 1922 | `nfc_poll_task` |
| 1935–1937 | `I (%lu) %s: NFC fired` / `NC Poll` / `--- Play card ---` |
| 2392 | `X-Yoto-Card-UID` |
| 2584 | `nfcErrs` |
| 2797 | `Shutting down NFC HAL task (if running)` |
| 3168–3169 | `UART` / `NFC driver` |
| 3170 | `SENT RESET: 0x01` |
| 3172 | `cr95hf_drv_send_recv() response was non-integer number of frames` |
| 3173 | `NFC timeout, Reset CR9HF` |
| 3174–3177 | `_cr95hf_drv_reqa() timeout: 0x0E0x87` / `0x87` / `REQA failed` |
| 3178 | `Wupa error, failed to reset card` |
| 3179–3180 | `_cr95hf_drv_adjust_timew()` / `_cr95hf_drv_modulation_gain()` |
| 3181–3186 | `_cr95hf_drv_antic1()` (failed/OK/timeout/bad response) |
| 3187 | `_cr95hf_drv_select1() : Select 1 failed` |
| 3188–3192 | `_cr95hf_drv_antic2()` / `select2()` |
| 3193 / 3206 | `_cr95hf_read_card() : Read A3 failed` / `Read A0 failed` |
| 3195 | `_cr95hf_drv_calibration() : NFC timeout` |
| 3196–3197 | `_cr95hf_Select_protocol() Error` |
| 3198–3199 | `Echo Received.` / `EXPECTED 0x%02x GOT 0x%02x` |
| 3200–3204 | hibernate / `cr95hf_drv_card_present timeout` / `REQA Failed` / `Card in NFC field != 1` |
| 3205 | `UID : %02X %02X %02X %02X %02X %02X %02X` |
| 3208 | `URL : %s` |
| 3209 | `Problem reading NDEF record - TNF:… TLEN:… TYPE:… ID:…` |
| 3211–3220 | CC (`Magic/Ver/Size/Flags`) + NDEF (`ndeflen/Tnf/Typelen/Length/Type/Id`) |
| 3221–3223 | `Read URL from card` / `Read /card-play URI` / `Read invalid URL` |
| 3226–3241 | write flow (`--- Writing ---` … `Step 0..7`, `RESULT`, `Total reads/errors`) |
| 3242 | `NFC HAL` |
| 3243–3244 | `Previous card UID=%s` / `New card UID=%s, URL=%s` |
| 3248–3249 | `Reset over UART using echo command after %u attempts` / `Failed to reset over UART` |
| 3258 | `NFC gate task is already running: sd=%s rd=%s` |
| 3261 / 3265 | `nfc_hal_gate` / `NFC_HAL_TAG` |
| 3266–3268 | `nfc_hal_read_card()` / `nfc_hal_read_carduid()` invalid length |
| 3275–3276 | `Unexpected NFC setup params (type %d, rx type %d, tx type %d` / `Unknown NFC Interface` |
| 3283 | `nfc_start_mock: failed to take lock` |
| 3287 | `Current url: %s. uid: %s. source:%s(%d)` |
| 3295 | `nfc_get_card_id` |
| 3384 | `EVT_NFC` |
| 3497 | `NFC Settings` |
| 3608–3611 | embedded `"nfc": { "type":"uart", "rx":"GPIO.32", "tx":"GPIO.33" }` |
| 7875–7876 | `NFCERRFACTORY` / `NFCERRNEWEST` |
| 8446 | `https://api.yotoplay.com/card/%s/linkUrl?uid=…&originalUrl=%s` |
| 8530 | `https://yoto.io/%s` |
| 8602–8606 | `NFC WRITE PASS` / `NFC WRITE FAIL` / `Write <text> to card.` |

---

## 8. Raw file pointers

- `output/strings.txt` — ground-truth strings (lines cited above).
- `output/strings_categorized.json` — category `rfid_nfc` (99 strings).
- `output/hwconfig_0[0-5]_*.json` — per-revision `nfc` + `spi` blocks.
- `output/pinmap.json` — flattened `nfc.*` / `GPIO.*` / `IOX.*` mapping.
- `output/layout.json` — segment map (seg4 IROM `0x400d0020`, seg0 rodata `0x3f400020`, seg2 DRAM `0x3ffbdb60`).
- `output/decompiled_manifest.json` — function list (addr/size/file).
- `output/ghidra_functions.json` — 9,185-function list (raw-offset addrs; use manifest for virtual addrs).
- `output/decompiled/FUN_seg4__4010d270_0x4010d270.c` — SendRecv frame builder (cmd 0x04).
- `output/decompiled/FUN_seg4__4010d2ac_0x4010d2ac.c` — REQA (`_cr95hf_drv_reqa`).
- `output/decompiled/FUN_seg4__4010e4d0_0x4010e4d0.c` — read UID.
- `output/decompiled/FUN_seg4__4010fc24_0x4010fc24.c` — NDEF/URI extraction (CC + record parse).
- `output/decompiled/FUN_seg4__4010db20_0x4010db20.c` — read-card orchestrator (2477 B).

---

## 9. Reconstruction caveats

- Ghidra Xtensa does **not** resolve L32R literal-pool references, so string→function xrefs are
  sparse. Function roles above are recovered by (a) scanning code segments for 32-bit rodata
  addresses (string literal pools) and (b) matching decompiled constants (`0x04` SendRecv, `0x26`
  REQA, `0x52` WUPA, `0x93/0x95`, `0xE1 0x10` CC magic, `0xD1` NDEF, `0x55 'U'`, `0x04 https://`).
- The CR95HF low-level driver body (`0x400d5000–0x400d5534`) was not decomposed into functions by
  Ghidra; its presence/location is evidenced by its literal-pool/data block (`0x400d5400–0x400d550c`)
  and the function pointer it exports (`0x4024bac4` transport).
- The write path step-by-step block-level semantics are `[INFERENCE]`; the write strings/step count
  are exact.
