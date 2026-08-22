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
pins. It then synchronizes with `[0x00, 0x55]` Echo frames before sending
ProtocolSelect `[0x00, 0x02, 0x02, 0x02, 0x00]`.

Normal commands use `[0x00][command][length][payload]`; responses use
`[result][length][payload]`.

## Driver protocol (Type-A activation)

The driver (`_cr95hf_drv_*`) implements the ISO/IEC 14443-A activation
sequence:

1. `_cr95hf_drv_reqa()` — request card (REQA).
2. `_cr95hf_drv_antic1()` / `_cr95hf_drv_antic2()` — anti-collision cascade
   levels 1 and 2.
3. `_cr95hf_drv_select1()` / `_cr95hf_drv_select2()` — select/activate.
4. NDEF read — `ndeflen:0x%02X` (length from the tag's CC bytes / NDEF TLV).

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

## Tag writing

The driver also writes tags (used by the "Make Your Own" card feature):

```text
--- Writing ---
```

plus control steps `_cr95hf_drv_adjust_timew()`,
`_cr95hf_drv_modulation_gain()`, and the UART reset mechanism
(`SENT RESET: 0x01`, `Reset over UART using echo command after %u attempts`).

## Task model

NFC is managed by a dedicated RTOS task guarded by a mutex:

```text
E (%lu) %s: Cannot take nfc mutex to init hal.
E (%lu) %s: Cannot take nfc mutex to start task
E (%lu) %s: Initialise NFC HAL before starting it
E (%lu) %s: Too many NFC driver errors: assuming card is removed
W (%lu) %s: NFC timeout, Reset CR9HF    # (sic — firmware typo for "CR95HF")
```

The HAL initialises the CR95HF, starts a poll task, and emits card
inserted/removed/UID-changed events.

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
