---
icon: lucide/contact-round
---

# NFC / RFID (CR95HF)

The card reader is an **STMicroelectronics CR95HF** — a 13.56 MHz NFC/RFID
transceiver. It is the interface through which the device reads **Yoto cards**
(NFC tags carrying an NDEF record), and the driver also **writes** tags (card
creation / blanking — see below).

## Interface

The CR95HF can be driven over UART or SPI; the firmware selects the transport
from the embedded `nfc.type` field:

| Config | Interface | Pins |
|--------|-----------|------|
| #01 | **SPI** | `mosi`=GPIO18, `miso`=GPIO21, `sclk`=GPIO2, `cs`=IOX1.4, `irqin`=IOX0.7, `irqout`=IOX1.0 |
| #00/#02/#04/#05 | **UART1, 57600 8-N-2** | ESP TX=GPIO32, ESP RX=GPIO33 |

The JSON pin names are from the reader's perspective: `nfc.rx=GPIO32` is
driven by the ESP32 TX signal, while `nfc.tx=GPIO33` drives ESP32 RX.

### UART startup

The stock boot path installs a 2048-byte RX buffer, applies 57600 8-N-2,
configures GPIO32 as output and GPIO33 as input, and routes UART1 to those
pins. Factory function `0x4010d270` constructs an internal request buffer with
a leading zero. Direct disassembly of UART callback `0x40110a58` shows that
`0x40110a6e–0x40110a75` removes this byte before transmission. Both stock and
the replacement therefore send bare `0x55` Echo and normal commands as
`[command][length][payload]`.

The replacement retains the stock **530-attempt** recovery bound and drains
non-Echo replies. It now leaves the ESP TX pin idle-high instead of forcing a
1 ms low pulse onto a reader that may already be awake after an ESP reset.
The first Echo supplies the falling edge for a sleeping reader; a 10 ms
response delay allows oscillator startup and matches stock RX pacing.
An absent reader still returns a timeout (under seven seconds in the host
model). After synchronization, IDN logs the device string/ROM byte.
Responses are `[result][length][payload]`.

**Verification status (rev #04, 2026-09-08):** the earlier prefixed-Echo image
recovered after 385 attempts. A later bare-Echo image that still forced the
GPIO wake pulse exhausted 530 attempts, restarted, then synchronized after
another 238; it returned IDN `NFC FS2JAST4` (ROM `0xce`) but the capture saw no
URI card. That hardware check failed, not passed.

The final idle-high/Echo-paced wake change builds for rev #04 and passes the
host suite, but its pending flash was stopped at the user's request to commit
and push. Cold-power startup, repeated-reset card reads, and RF reliability
remain unverified on that final image. Host regressions cover bounded recovery,
strict RF rejection, bounded NDEF reads, UID-only polling, and card transitions.

## Driver protocol (Type-A activation)

The driver (`_cr95hf_drv_*`) implements the ISO/IEC 14443-A activation
sequence recovered from the factory image:

1. The stock helpers at `0x4010d3f8` and `0x4010d428` pass the apparent tuning
   arrays `09 04 3A 00 60 04` and `09 04 68 01 01 D0` through **SendRecv**.
   They are RF payloads, not demonstrated reader-register writes; the earlier
   interpretation of these arrays as effective stock tuning was incorrect.
2. `_cr95hf_drv_reqa()` (`0x26,0x07`).
3. `_cr95hf_drv_antic1()` / `_cr95hf_drv_antic2()` — anti-collision cascade
   levels 1 and 2.
4. `_cr95hf_drv_select1()` / `_cr95hf_drv_select2()` — select/activate.
5. Read page 3 with `30 03 28`; factory helper `0x4010d718` copies the first
   six data bytes (CC plus TLV header) and next five NDEF bytes separately.

The factory full-reader calls WUPA only after its own lower-layer cleanup.
Replaying WUPA directly after a selected-tag transaction returned `0x87`.
The replacement sends WUPA only after a complete REQA no-response (`0x87`,
zero payload). Both must time out to establish absence; malformed frames,
CRC failures, and transport errors remain errors rather than removal.

Log strings confirm the flow:

```text
D (%lu) %s: _cr95hf_drv_antic1() response OK
D (%lu) %s: _cr95hf_drv_antic2() response OK
D (%lu) %s: ndeflen:0x%02X
D (%lu) %s: UID : %02X %02X %02X %02X %02X %02X %02X
D (%lu) %s: URL : %s
```

!!! note "Datasheet vs firmware"
    The byte values (e.g. REQA `0x26`), the terms ATQA/SEL/NVB, and the ISO
    standard numbers (14443 A/B, 15693) are CR95HF datasheet knowledge, not
    literal firmware strings. The firmware strings give the function names and
    log messages above.

`0x0E`/`0x87` in the timeout logs are CR95HF status/error codes printed as two
concatenated bytes (`0x0E0x87`, `0x87`).

## Tag structure

The tag is parsed as an **NFC Forum Type 2 Tag** — CC (capability container)
bytes followed by an NDEF TLV. The payload is a URL plus a UID:

```text
E (%lu) %s: Read NFC UID=%s, URL=%s
E (%lu) %s: NFC read value changed! UID=%s, URL=%s
E (%lu) %s: Current url: %s. uid: %s. source:%s(%d)
```

The **URL** is the content locator (the "card" identity used to look up audio
on the SD card or stream it — see [SD Card & Storage](storage.md)). The UID is
the tag serial, used to distinguish physically distinct cards carrying the same
content. Card presence/debounce is logged:

```text
D (%lu) %s: Previous card UID=%s
D (%lu) %s: New card UID=%s, URL=%s
D (%lu) %s: Card is ...        # presence events
```

The replacement currently consumes only these card fields:

| Field | Source | Use |
|---|---|---|
| UID | ISO14443-A cascade-level anticollision | card identity and write safety |
| ATQA / SAK | REQA/WUPA and SELECT responses | activation/cascade validation |
| Capability Container | memory page 3 | optional capacity and logical access flags |
| Static locks | page 2 bytes 2–3 (`READ 0`, data offsets 10–11) | reject permanently locked target pages |
| NDEF URI | Type-2 TLV in user pages 4+ | content URL |

Other profile metadata—originality signature, counters, password status,
GET_VERSION product name, text/empty NDEF records—is not used for playback.
An empty or non-URI NDEF record therefore produces a valid UID with a blank
URL.

Factory helper `0x4010d718` proves the page layout directly: the response to
`READ 3` begins with the four-byte CC, followed by the page-4 NDEF TLV. A
normal CR95HF response has 19 payload bytes: 16 tag-memory bytes plus three RF
status bytes. The replacement receives all 19, parses only the first 16, and
logs all pages 3–6 when the CC is invalid.

The replacement NDEF parser walks every record rather than assuming the URI is
first. A valid CC bounds the read to its capacity. A zero CC is a genuinely
unformatted tag; the replacement additionally probes only the minimum 48-byte
user area to recover data left by nonconforming writers.

The two external-reader profiles provide concrete card evidence:

| Profile | Technology | ATQA / SAK | NDEF status |
|---|---|---|---|
| `readable-card` | NXP MIFARE Ultralight EV1, 48 bytes | `0044` / `00` | unspecified; zero CC |
| `unreadable-card` | NXP MIFARE Ultralight EV1, 48 bytes | `0044` / `00` | `readWrite`, 3-byte empty record |

They require the same ISO14443-3A protocol. The second card's empty NDEF record
correctly yields a blank URL; its UID must still be reported.

## Tag writing

The replacement writes NDEF URLs through `cr95hf_write_url()`. The writer
reactivates the card, compares its physical UID with the requested UID, writes
page-wise with `A2`, then re-reads the URI and requires an exact match.

### Required Type-2 layout

Working cards must be NFC Forum Type 2 formatted. Page 3 is the Capability
Container (CC); user data begins at page 4:

```text
page 3:  E1 10 06 00       # NFC Forum Type 2, v1.0, 48 user bytes, read/write
page 4+: NDEF TLV
```

The verified on-card representation of `https://openyoto.com/admin` is:

```text
03 17                         # NDEF TLV, 23-byte value
D1 01 13                      # MB|ME|SR|TNF=well-known, type len=1, payload len=19
55                            # RTD-URI type ("U")
04                            # URI prefix: https://
6f 70 65 6e 79 6f 74 6f 2e 63 6f 6d 2f 61 64 6d 69 6e
                              # "openyoto.com/admin"
FE                            # TLV terminator
```

The observed page stream was:

```text
page 4: 03 17 D1 01
page 5: 13 55 04 6F
page 6: 70 65 6E 79
page 7: 6F 74 6F 2E
page 8: 63 6F 6D 2F
page 9: 61 64 6D 69
page 10: 6E FE 00 00
```

Bytes after `FE` are not part of the NDEF message. The writer stages an empty
NDEF length before page writes, publishes the final length only after the
payload is present, and verifies the final URI read-back. This prevents a
partially written card from presenting a valid new record.

For an all-zero CC, `cr95hf_format_blank_mf0ul11()` first identifies an
unlocked 48-byte MIFARE Ultralight/EV1-compatible tag, writes an empty NDEF
area, writes and verifies `E1 10 06 00` at page 3, then performs the normal
URI write. It does not format unknown or locked tags. Valid formatted cards
are overwritten only in pages 4+; manufacturer, lock, OTP, and CC pages are
never rewritten.

Factory helper `0x4010d770` constructs `A2 <page> <four data bytes> 28`.
CR95HF can report `87 00` after a successful physical page program, so the
writer treats that response as ambiguous and accepts it only when immediate
read-back matches.

### Corrupted anticollision diagnostic

An NFC memory or formatting change cannot fix a failure before Type-2 pages are
read. For a 7-byte UID, cascade-level 1 must return five tag bytes:

```text
88 UID0 UID1 UID2 BCC
```

For example, UID `04 34 6E EA 42 59 80` requires CL1
`88 04 34 6E D6`; CL2 then returns `EA 42 59 80 71`. A complete CR95HF
anticollision response has those five tag bytes followed by three RF status
bytes.

A corrupted clone can instead produce a partial response such as:

```text
code=0x90 len=6 raw=88 04 34 27 00 00
tag_bytes=3 status=0x27 significant_bits=7
```

Only `88 04 34` arrived; the final UID byte and BCC are missing. The driver
must reject this response: it cannot safely construct the subsequent
`93 70 <CL1 bytes> + CRC-A` SELECT frame. A later `SELECT L1` result `0x87`
is therefore downstream of the malformed anticollision reply, not the cause.

Varying partial prefixes can mean two cards are energized, but they can also
be corrupted output from one clone. Isolate other cards first. If one card
still emits partial CL1 frames, test its orientation and a few millimetres of
spacing; phone-reader success does not prove compatibility with this fixed
CR95HF antenna. The replacement removes its unvalidated TimerW `0x58` /
ARC_B `0xD3` overrides and waits 10 ms after Type-A ProtocolSelect. This is
not proof of an analog RF fix. No NDEF, lock, CC, or page-write change can
reconstruct missing UID bytes; truncated and CRC-invalid frames remain rejected.

The original-image power comparison found matching rev #04 output-before-
direction IOX setup and run-state levels: VINHOLD high, PWREN low, level
converter enabled. Battery and NFC startup order differences did not establish
a charger or NFC supply fault. No speculative charger changes were made for
these RF symptoms.

## Task model

Stock NFC is managed by a dedicated RTOS task guarded by a mutex:

```text
E (%lu) %s: Cannot take nfc mutex to init hal.
E (%lu) %s: Cannot take nfc mutex to start task
E (%lu) %s: Initialise NFC HAL before starting it
E (%lu) %s: Too many NFC driver errors: assuming card is removed
W (%lu) %s: NFC timeout, Reset CR9HF    # (sic — firmware typo for "CR95HF")
```

The HAL initialises the CR95HF, starts a poll task, and emits card
inserted/removed/UID-changed events.

The replacement polls in the player loop and serializes activation, reading,
and writing with its UART mutex. NDEF acquisition stops at a complete
classification; diagnostics retain only the acquired prefix, not all memory.
Reaching the 256-byte scan limit of a larger tag does not prove it blank.
After acquiring a card, held-card polls read only its UID. A changed UID
invalidates the previous playback/write target immediately; content is read on
the next poll. Initial unreadable content remains pending until a later valid
read, without replaying successful actions.

Removal requires two consecutive no-response polls or 21 consecutive driver
errors; presence resets both counts and opposite failure types interrupt each
streak. These are replacement policies, not a claim that the configurable stock
removal timing is identical. Payload errors alone do not remove a known UID.

While the replacement's admin server is active, non-magic scans are
capture-only: no mapping lookup, image render, or audio playback occurs. The
web UI polls capture state only on the Cards tab and only while its URL input
is empty. Repeated identical reports are idempotent. More importantly, a write
request is gated by captured UID—not a polling sequence number—and the driver
then rechecks the UID directly against the tag still in the RF field.

## Factory test / diagnostics

The firmware includes an NFC production-test mode (`/nfc`,
`/sdcard/prodtest.txt` → `/sdcard/prodtest_renamed.txt`):

```text
E (%lu) %s: Reading NFC %d times (@4Hz) & flagging any discrepancies
E (%lu) %s: NFC attempt %d - UID=%s, INVALID URL=%s
E (%lu) %s: NFC read test complete. %d uid/url change(s), %d url error(s)
E (%lu) %s: Detected removal of production test card before NFC test was completed!
```

A dedicated `APP_NFCERRCRRCT` setting counts NFC read errors across the test.

## Summary

| Aspect | Detail |
|--------|--------|
| Chip | ST CR95HF |
| Transport | UART (GPIO32/33) or SPI |
| Tag | NFC Forum Type 2 Tag (CC bytes + NDEF TLV) |
| Payload | NDEF → URL + UID |
| Read + write | reads cards; also writes tags (card creation) |
| Downstream | URL drives content lookup (SD card or HTTP stream) |
