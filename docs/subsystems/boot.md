---
icon: lucide/power
---

# Boot & OTA

## Boot sequence

1. **ROM bootloader** → **second-stage bootloader** at flash `0x1000`
   (image header `0xE9`, entry `0x400805d8`).
2. Bootloader reads the partition table at `0x8000` and boots the **factory**
   app (offset `0x40000`), unless OTA data selects `ota_0`/`ota_1`.
3. App entry point **`0x400813a8`** (IRAM `call_start_cpu0`) → FreeRTOS init →
   `app_main`.

The app tracks boot progress in NVS:

```text
FIRST_BOOT_DONE
BOOTCOUNT
BOOT_PART
Booting Up
```

### Decompilation cross-check

Ghidra (Xtensa) decompilation of the entry point confirms this sequence:
the entry sets the vector base (`wsr VECBASE`), calls through the ROM
function-pointer table, invokes `start_cpu0` (`0x400812f4`), then runs the
cache/ROM init helpers (`0x400988fc`, `0x400881bc`, `0x40090b88`). Auto-analysis
identified **81,667 functions** across the correct IRAM (`0x40080000`), IROM
(`0x400D0000`) and DROM (`0x3F400000`) segments.

## Reset / recovery

Multiple reset paths exist:

| Trigger | Mechanism |
|---------|-----------|
| Factory reset | `/sdcard/reset` file, `/boot-factory-fw` command, `SENT RESET: 0x01` |
| Console mode | `/dev/console` + UART echo command (`Starting up in Console Mode`) |
| Watchdog | RTC WDT (`RTC watchdog reset digital core and rtc module`) |

The device distinguishes reset reasons (power-on, brownout, watchdog, panic)
and can force boot into the factory partition:

```text
%s: No reset info collected
E (%lu) %s: Unkown reset reason
E (%lu) %s: %s: RTC watchdog resetted CPU
```

## OTA update

OTA follows the standard ESP-IDF `esp_ota` flow with the `otadata` partition
selecting the active slot. The firmware gates OTA on battery level and card
presence:

```text
BATT_THRS_OTA       # battery threshold below which OTA is deferred
DONE_BOOT_OTA
OTA_TRIGGER
D (%lu) %s: Cannot OTA because card is present.
```

`Cannot OTA because card is present` indicates OTA is blocked while an SD card
(a possible content-update path) is inserted. OTA downloads are integrity
checked (SHA-256) and the bootloader verifies the image on next boot
(`Image hash failed - image is corrupt`).

## Factory test

A full production test harness is compiled in, driven over the console
(`CONSOLE_INIT`, `CONSOLE START PASS`, `Factory app partition`, `test` labels)
and covers display (`/display-*`), NFC (`/nfc`), audio, buttons
(`/diag-buttons`), and power (`/power-icon-test`, `/sleep`).
