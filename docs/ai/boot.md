---
icon: lucide/power
---

# Boot & OTA — AI-detail reference

Dense, evidence-backed reference for AI agents working on the **boot, reset,
OTA, and factory-test** subsystem of the Yoto ESP32 firmware. Every address,
string, and pin below is the exact value from the repo's extraction artifacts
(`output/*`), not a guess. Human narrative lives in
[subsystems/boot.md](../subsystems/boot.md); full per-revision pin tables live in
[hardware.md](../hardware.md). This page is the machine-oriented companion.

## 1. Target at a glance

| Attribute | Value | Source |
|-----------|-------|--------|
| SoC | ESP32 (Xtensa LX6), chip_id `0x0000` | `output/layout.json` `images.factory.chip_id` |
| Flash size | 8,388,608 B (8 MiB) | `output/layout.json` `flash_size` |
| App image | `output/factory.bin`, 2,454,473 B, 7 segments | `output/layout.json` `images.factory.image_size` |
| **App entry** | **`0x400813a8`** = `call_start_cpu0` (IRAM) | `output/layout.json` `images.factory.entry` (= `1074271144`) |
| Firmware version | `v2.21.4-7` | `output/strings.txt:217`; literal at `0x400d33fc` |
| Build env | `prod` | `output/strings.txt:219` |
| Framework | ESP-IDF + ESP-ADF (IDF SHA `d7b0a45d…`, ADF SHA `49f80aaf…`) | `output/strings.txt:8513-8516` |

## 2. Partition table (exact)

From `output/layout.json` `partitions[]` (offset/size are decimal there; hex
here). Partition-table magic `AA 50`, 32-byte entries at flash `0x8000`
(`analysis/extract_app.py:33-49`).

| Label | Type | Subtype | Offset (hex) | Offset (dec) | Size (hex) | Size (dec) |
|-------|------|---------|--------------|--------------|------------|------------|
| `nvs` | data (1) | nvs (2) | `0x009000` | 36,864 | `0x30000` | 196,608 |
| `otadata` | data (1) | ota (0) | `0x039000` | 233,472 | `0x2000` | 8,192 |
| `phy_init` | data (1) | phy (1) | `0x03b000` | 241,664 | `0x1000` | 4,096 |
| `factory` | app (0) | factory (0) | `0x040000` | 262,144 | `0x280000` | 2,621,440 |
| `ota_0` | app (0) | ota_0 (16) | `0x2c0000` | 2,883,584 | `0x280000` | 2,621,440 |
| `ota_1` | app (0) | ota_1 (17) | `0x540000` | 5,505,024 | `0x280000` | 2,621,440 |

`ota_0` / `ota_1` are empty (0xFF) in the shipped factory image — OTA writes a
new image there and `otadata` selects it on next boot. Factory rollback =
"returning to factory partition" (`output/strings.txt:1827`).

## 3. App-image memory map & entry point

Decode of the 0xE9 app-image header (`analysis/ghidra_esp32.py:37-53`),
hex values from `output/layout.json` `images.factory.segment_map`:

| seg | Load address | Size | Region |
|-----|--------------|------|--------|
| 0 | `0x3F400020` | `0xA63B0` (680,880) | DROM — flash-mapped rodata (strings, settings JSON) |
| 1 | `0x3FF80063` | 8 | RTC_DRAM (fast) |
| 2 | `0x3FFBDB60` | `0x77B8` (30,648) | DRAM |
| 3 | `0x40080000` | `0x2470` (9,328) | IRAM — **contains entry `0x400813a8`** |
| 4 | `0x400D0020` | `0x18F0CC` (1,634,508) | IROM — flash-mapped code + `.literal` pools |
| 5 | `0x40082470` | `0x18248` (98,888) | IRAM |
| 6 | `0x400C0000` | `0x64` (100) | RTC_IRAM (fast) |

- The **literal-pool / rodata section begins at `0x400D0020`** (seg 4 start);
  actual `.text` functions in IROM start at `0x400D5534` (first decompiled IROM
  function; ghidra's first IROM function is `0x400D5518`). The OTA module's
  literal pool occupies `0x400d3320–0x400d34e8` (see §7).
- Entry `0x400813a8` is inside seg 3 (IRAM). Decompiled in
  `output/decompiled/FUN_seg3__400813a8_0x400813a8.c` (size 698): sets
  `wsr VECBASE`, calls the ROM function-pointer table (`DAT_seg3__400804c0..d8`),
  zeroes `.bss`, then calls `FUN_seg3__400812f4` (= `start_cpu0`) and ROM helpers
  `func_0x400881bc`, `func_0x40090b88`, `func_0x400988fc`.
- Second-stage bootloader (flash `0x1000`, entry `0x400805d8`) is **not** in the
  app image — see [subsystems/boot.md](../subsystems/boot.md).

## 4. Boot sequence (app_main flow)

Reconstructed from log strings in order (`output/strings.txt:214-238` plus the
reset/wakeup block). The `START-UP INFO` tag is emitted by
`FUN_seg4__400eb908`/`FUN_seg4__400ebaf8` (see §9).

1. `Starting app cpu, entry point is %p` — PRO CPU brings up APP CPU (`:9`).
2. `Wake up reason : %d` (`:222`) — see §5 for the enum.
3. Identity: `DeviceId`, `DeviceKeyId`, `Version: v2.21.4-7`, `BuildEnv: prod`,
   `Batch: %s` (`:214-220`).
4. `Initialising SPI/I2C buses...` (`:223`).
5. I2C recovery: `Number of I2C init failure reboots so far = %d` (`:224`),
   `rebooting to see if we can recover from core I2C error(s)` (`:226`) —
   boot-loop recovery for a dead I2C bus.
6. `Accelerometer Low-Power mode set...` (`:227`).
7. `Checking for emergency factory reset request...` (`:229`) — button-held
   reset gate (§5).
8. `Voltage too low to start; shutting down` (`:230`); low-voltage path
   `Low voltage, run low power routine.` (`:238`).
9. `Initialising audio board...` (`:231`), `Initialising CR95HF NFC chip...`
   (`:232`), `Initialising Wi-Fi event groups...` (`:233`).
10. `Starting up in Console Mode` (`:235`) or `Checking battery level then
    starting user mode` (`:237`).

Boot-progress NVS/SD keys: `FIRST_BOOT_DONE` (`:7862`), `DONE_BOOT_OTA`
(`:7852`), `SETUP_COMPLETE` (`:7853`), `USER_STARTUP` (`:7858`), `BOOTCOUNT`
(`:8074`, SD status JSON), `BOOT_PART` (`:3803`). SD boot counter file is
`/sdcard/boot_cnt` (`:8011`).

## 5. Reset paths

### 5.1 Reset reason decode

`output/strings.txt:1313-1342`. Digital-core (ESP32 `rtc_get_reset_reason`)
strings `:1313-1329`, plus the IDF `esp_reset_reason_t` enum strings
`ESP_RST_UNKNOWN/POWERON/EXT/SW/PANIC/INT_WDT/TASK_WDT/WDT/DEEPSLEEP/BROWNOUT/SDIO`
(`:1331-1341`) and `Unexpected system reset reason: %d` (`:1342`).
Wake-up causes `:1343-1355` (`External event 0/1`, `GPIO`, `Timer`, `UART0/1`,
`Touch`, `BT`, `Unknown/unspecified wakeup cause`).

### 5.2 Factory reset (multi-path)

- Emergency (button-held): `Checking for emergency factory reset request...`
  (`:229`), `Emergency factory reset triggered by user!` (`:1829`), `cancelled by
  user` (`:1830`), `prompt timed out` (`:1831`). Tag `factory_reset` (`:1826`).
- Effect: `FACTORY RESET triggered: clearing Wi-Fi/NVS, cleaning up SD card and
  returning to factory partition` (`:1827`) or `…formatting SD card…` (`:1828`).
- Card/NFC paths: `RESET_CARD` (`:1571`), `Performing Factory Reset` (`:1572`),
  `RESET_NO_FACTORY_CARD` (`:1573`), `Performing Factory Reset (skipping revert
  to factory partition)` (`:1574`).
- Console/UART: `/boot-factory-fw` (`:2702`), `Performing factory reset` (`:1881`),
  `FACTORY RESET PASS` (`:8722`), `Revert player back to factory state…` (`:8723`).
- Reset-tracking: `%s: No reset info collected` (`:4041`),
  `Event storage (%d) smaller than largest reset reason (%d)` (`:4042`),
  `memfault_reboot_tracking_collect_reset_info` (`:4043`) — Memfault crash/reset
  reporting is compiled in.

### 5.3 Shutdown / deep sleep

`output/strings.txt:2784-2841`. Sequence: `Start of shutdown sequence...`
(`:2786`) → shut down modules (walkman/NFC/OTA/Wi-Fi/ambient/memcheck,
`:2793-2801`) → `Powering off audio amps` (`:2802`) → `Switching off display`
(`:2804`) → `Unlatching VIN_HOLD` (`:2816`) → `Deep sleep ...` (`:2814`).
Wake triggers: `AR_USER alarm trigger detected` (`:2837`), `AR_TEST alarm trigger`
(`:2838`), RTC_INT low (`:2815`). `fw_power_man` (`:2822`),
`fw_pseudo_standby_processing` (`:2841`). See also `Rebooting...` (`:117`),
`Restarting the system with esp_restart` (`:2806`).

## 6. OTA gating (the gates an agent must model)

The OTA gate is battery-gated, card-gated, and time-gated. Config keys (NVS,
settings JSON) are listed in `output/strings.txt:7673-7755`:

| Key | Default | Min/Max | Meaning | Source |
|-----|---------|---------|---------|--------|
| `BATT_THRS_OTA` | `15` | — | battery % below which OTA is deferred | `:7748-7749` |
| `OTA_TRIGGER` | `3420` (s = 57 min) | `60` / `10800` | timer period between OTA checks | `:7710-7713` |
| `BATT_THRS_START` | `4` | — | % to start boot | `:7746-7747` |
| `BATT_THRS_WRNG` | `7` | — | % low-battery warning | `:7751-7752` |
| `DONE_BOOT_OTA` | — | — | flag: OTA gate ran post-boot | `:7673`, `:7852` |

Gating decision strings (tag `ota gate`, `:1946`):

- No network/config: `No wifi/configs, deleting OTA task ...` (`:1949`).
- Triggers: `OTA is triggered by external event` (`:1956`), `OTA is triggered by
  timer` (`:1957`), retries 1–5 (`:1951-1955`).
- **Card-present block**: `Cannot OTA because card is present.` (`:1958`) and
  `Yoto Player skipped OTA because a card is playing` (`:1964`).
- **Battery block**: `Not enough power to OTA` (`:1960`); battery driver side:
  `OTA startup voltage %lumV %u%% (raw)` (`:893`), `Insufficient power for OTA :
  %lumV` (`:894`), `… : %u%% (raw)` (`:895`).
- Lifecycle: `OTA gate is paused…` (`:1966`), `…shutting down…` (`:1967`),
  `Deleting OTA task ...` (`:1968`), `OTA gate task is already running` (`:1970`),
  `Failed to initialise ota module lock` (`:1971`).
- Per-run disable: `Disabling OTA update for the current run session` (`:1603`);
  re-enable via `OTA CARD: OTA checks are (re-)enabled` (`:1589`).

## 7. OTA update flow & version API

### 7.1 Update state machine (tag `ota_update`, `:1975`)

`output/strings.txt:1976-2018`:

1. `deviceID: %s` / `releaseChannel: %s` / `version: %s` / `versionURL: %s`
   (`:1976-1979`).
2. `Test release channel - forcing OTA of same version %s` (`:1980`) — test
   channel bypasses version-compare.
3. `Firmware is already up to date` (`:1983`) vs `Starting OTA update...`
   (`:1984`).
4. Partition sanity: `Configured OTA boot partition at offset 0x%08lx, but
   running from offset 0x%08lx` (`:1985`), `Running partition type %d subtype %d
   (offset 0x%08lx)` (`:1987`).
5. `Start to Connect to Server.... %s` (`:1988`) → HTTP/S (root cert literal at
   `0x400d3428`) → `esp_ota_begin succeeded` (`:1993`) / `esp_ota_begin failed
   (%s)` (`:1992`).
6. `Writing to partition subtype %d at offset 0x%lx` (`:1991`); UI vs silent:
   `OTA is running in UI animation mode` (`:1994`), `OTA is running in silent
   mode (no UI)` (`:1995`); `Total Write binary data length : %d` (`:1999`).
7. `esp_ota_end failed!` (`:2000`) → `esp_ota_set_boot_partition failed (%s)!`
   (`:2002`) → `OTA Complete, it will apply only after restart` (`:1959`).
8. Result codes: `OTA OK` (`:2017`), `OTA FAILED (%s)` (`:2018`),
   `Unknown OTA status` (`:1965`).

Underlying IDF OTA strings: `esp_ota_ops` (`:5513`), `not found otadata`
(`:5514`), `ota data invalid, no current app. Assuming factory` (`:5520`), and
the `ESP_ERR_OTA_*` enum (`:5662-5668`), `ESP_ERR_HTTPS_OTA_*` (`:5808-5809`).

### 7.2 Version API (endpoint & response)

- Endpoint: `https://api.yotoplay.com/device-v2/version` (`:2005`).
- URL assembly fragments: `?channel=` (`:2010`), `&version=` (`:2011`),
  `?version=` (`:2012`), `&rev=` (`:2013`), template `%s%s%s%s%s` (`:2006`).
- Response fields: `releaseChannel` (`:1972`), `firmwareDownloadUrl` (`:1973`),
  `firmwareVersion` (`:1974`), `deviceId` (`:1976`).
- Auth: `Sending JWT as authorisation to: %s` (`:2008`); JWT/creds NVS keys
  `JWT` (`:7672`), `AWS_CERT` (`:7670`), `AWS_PRIVATE_KEY` (`:7671`),
  `DVKEY`/`DVKEYID`/`DVID` (`:7661-7667`).
- Markers `OTA_VAPI_START` (`:2003`) / `OTA_VAPI_END` (`:2009`); release-channel
  URLs `https://ota` (`:1619`), `https://ota-silent` (`:1620`).
- MQTT response topic: `/system/ver_resp` (`:2019`); HTTP client failures
  `OTA: failed to create HTTP client` (`:4017`), `Failed to build OTA URL`
  (`:4018`), `OTA Query Failure. Status Code: %d` (`:4023`).

### 7.3 OTA literal pool (`0x400d3320–0x400d34e8`) — verified layout

Dumping seg-4 rodata shows the OTA module's whole string table in order
(method: L32R pointer decode, see [decompile.md](decompile.md) §"L32R caveat"):

| Literal addr | Value | String |
|--------------|-------|--------|
| `0x400d3320` | `0x3f41239b` | `ota gate` (TAG) |
| `0x400d3324` | `0x3f4123a4` | `I (%lu) %s: Starting OTA gate` |
| `0x400d3384` | `0x3f412585` | `D (%lu) %s: Cannot OTA because card is present.` |
| `0x400d338c` | `0x3f4125f2` | `I (%lu) %s: Not enough power to OTA` |
| `0x400d33bc` | `0x3f412782` | `ota_gate` (task name) |
| `0x400d33c0` | `0x400f793c` | **code ptr → ota_gate task body** |
| `0x400d33e0` | `0x3f41281e` | `ota_update` (TAG) |
| `0x400d33fc` | `0x3f401ebc` | `v2.21.4-7` (current fw version) |
| `0x400d3414` | `0x3f412933` | `I (%lu) %s: Starting OTA update...` |
| `0x400d343c` | `0x3f412b33` | `E (%lu) %s: esp_ota_begin failed (%s)` |
| `0x400d3440` | `0x3f412b5a` | `I (%lu) %s: esp_ota_begin succeeded` |
| `0x400d3468` | `0x3f412d05` | `E (%lu) %s: esp_ota_set_boot_partition failed (%s)` |
| `0x400d3494` | `0x3f412d3a` | `OTA_VAPI_START` |
| `0x400d349c-0x400d34b0` | — | `?channel=` / `&version=` / `?version=` / `&rev=` |
| `0x400d34a4` | `0x3f412d54` | `https://api.yotoplay.com/device-v2/version` |
| `0x400d34c4` | `0x3f412edf` | `/system/ver_resp` |
| `0x400d34c8` | `0x3f412ded` | `OTA_VAPI_END` |
| `0x400d34d4` | `0x400f7f48` | **code ptr → OTA update flow entry** |

Function pointers used by the update function (imports into libs): `0x4017479c`,
`0x401743d0` (called via `PTR_FUN_seg4__4017479c_seg4__400d346c` /
`PTR_FUN_seg4__401743d0_seg4__400d30ac` in `FUN_seg4__400f8110`).

## 8. Factory test / production console

The factory console is the `esp32>` prompt (`output/strings.txt:8523`), reached
in Console Mode; banner:

```
============  Yoto V2 Player  ============   (:8497-8498)
================  Yoto Mini Player  ============  (:8500-8501)
==============  Yoto V3 Player  ============  (:8502-8503)
==================  Yoto Mini-E Player  ============  (:8504-8505)
Yoto V3-E Player                              (:8506)
This is the Yoto factory console.             (:8509)
Type 'help' to get the list of commands.      (:8510)
Version: %s / Build Env: %s / IDF SHA: %s / ADF SHA: %s  (:8511-8516)
```

Boot-time hardware/header self-check (`:8517-8522`):
`ERROR: failed to load bootloader image header` → `CONSOLE START FAIL` vs
`BOOTLDR HDR SIZE = %s` / `FW BUILD SIZE = %s` → `CONSOLE START PASS`;
`ERROR: unit was not programmed using flash size %s`; `ERROR: unexpected
hardware family ID: %d` (`:8508`); `Unknown hardware, use the 'batchkey' then
'restart' command to set hardware ID` (`:8499`). Provisioning mismatch strings
`PROVISION FAIL: …` (`:8639-8643`). `batchkey` command (`:8742`); product-family
tokens `PLAYERV1/V2`, `MINIPLV1`, `PLAYERV3`, `MINIEPLV1`, `PLAYERV3E`
(`:8728-8733`).

Factory-test alarm (RTC-based, only on RTC-equipped revisions — see §10 pin
table): `RTC_TEST_ALARM` (`:7686`), `FACTORY TEST ALARM HAS WOKEN US - RESULT
WILL BE DISPLAYED` (`:1157`), `RTC_INT is LOW (factory test alarm has
triggered)` (`:1214`), `Power up was due to factory test alarm triggering`
(`:1220`), `factory test alarm triggered correctly` (`:8484`), `…but with
invalid state` (`:8485`), `rtc_alarm_check_for_factory_test` (`:8486`),
`start_rtc_alarm_test` (`:8488`).

Production-test commands (console, `output/strings.txt:1575-1603, 8659-8722`):
- `perform_end_of_line_checks` (`:1588`), `play_prodtest_audio` (`:1575`),
  `https://prodtest` (`:1581`), `PRODUCTION_TEST_MODE` (`:1597`), `INSPECTION_TEST_MODE`
  (`:1602`), `START/STOP RSSI_TEST_MODE` (`:1599-1600`).
- SD: `Testing the SD card` (`:8697`), `SD PASS`/`SD FAIL` (`:8695-8696`),
  `FORMAT ERROR` (`:8694`), `PRELOAD ERROR` (`:8691`), `MOUNT ERROR` (`:8699`).
- UI: `UITEST PASS`/`UITEST FAIL` (`:8708-8709`), `but0/but1/but2/enc1/enc2`
  (`:8711-8719`).
- NFC test: `NFC test failure rate = %s` (`:1586`), `Reading NFC %d times (@4Hz)`
  (categorized), `NFC test cancelled: prodtest card was removed` (`:1582`).
- Reset/restart: `Software reset of the chip (i.e. force a reboot)` (`:8672`),
  `cmd_restart` (`:8670`), `cmd_system` (`:8673`), `FACTORY RESET PASS`
  (`:8722`).

## 9. Boot-relevant pins by revision

Six hardware configs (`output/hwconfig_00..05_*.json`) are the device's own
per-revision pin map. Boot/power-relevant assignments:

| Pin role | hw00 | hw01 | hw02 | hw03 | hw04 | hw05 |
|----------|------|------|------|------|------|------|
| IOX type / count | `ET6416` ×1 | `PI4IOE5V6416` ×1 | `ET6416` ×2 | `PI4IOE5V6416` ×2 | `ET6416` ×1 | `PI4IOE5V6416` ×2 |
| IOX interrupt (`ioxInt`) | `GPIO.34` | **`null`** | `GPIO.34` | `GPIO.34` | `GPIO.34` | `GPIO.34` |
| Power button | `IOX.1.3` | `IOX.1.3` | `IOX.1.3` | `IOX.1.3` | `IOX.1.3` | `IOX.1.3` |
| `pwren` | `IOX.1.4` | `null` | `IOX.2.5` | — | `IOX.1.4` | `IOX.2.5` |
| `vinhold` | `IOX.1.6` | `IOX.1.6` | `IOX.3.1` | — | `IOX.1.6` | `IOX.3.1` |
| `levelconvertor` | `IOX.0.3` | `null` | `IOX.3.0` | — | `IOX.0.3` | `IOX.3.0` |
| `vouten` | — | — | `IOX.3.3` | — | — | `IOX.3.3` |
| Charger IC | `ETA6003` | `ETA6003` | `SGM41511` | `SGM41513` | `SGM41513` | `SGM41513` |
| `plugstat` | `IOX.1.5` | `IOX.1.5` | `null` | — | `IOX.1.5` | `null` |
| `chgstat` | `IOX.1.7` | `IOX.1.7` | `IOX.1.4` | — | `IOX.1.7` | `IOX.1.4` |
| `chgbst` | `IOX.0.7` | `null` | `null` | — | `null` | `null` |
| Battery monitor | `CW2015` | `ADC` (`ADC.1.3`) | `CW2015` | — | `CW2215B` | `CW2215B` |
| `battalrt` (wake) | `IOX.1.0` | — | `IOX.0.6` | — | `IOX.1.0` | `IOX.0.6` |
| USB-C | — | — | `HUSB238` | — | — | `HUSB238` |
| Qi | — | — | `CV8085` (`0x13`) | — | — | `CV8013N` |
| RTC | **none** | **none** | `it8563` `0x51` | — | **none** | `it8563` `0x51` |
| `rtcint` | — | — | `IOX.0.1` | — | — | `IOX.0.1` |

Notes (from each file's JSON fields):
- `hw01` has `ioxInt: null` and `hasPull: true` with explicit pull configs
  (`p0PullEn/Dir 0x30`, `p1PullEn/Dir 0xA8`) — no shared IOX interrupt line.
- `hw02`/`hw05` are the dual-IOX ("number":"2") revisions with HUSB238 USB-C +
  Qi + IT8563 RTC; `hw05` adds Qi `qien5w IOX.3.5`, `qii2cint IOX.0.0` and
  `qirxtempsensor GPIO.35 / ADC.1.7`.
- `hw03` is an incomplete config (only `battery.charger`, `iox`, `buttons`
  populated) — treat as a stub.
- `hwconfig_merged.json` mirrors the dual-IOX (`number":"2"`) layout.

Flattened pin→function map is `output/pinmap.json` (per-revision
`GPIO.x`/`ADC.x`/`IOX.p.n` → dotted function path, e.g.
`"GPIO.34": ["iox.ioxInt"]`, `"IOX.1.3": ["buttons.power"]`).

## 10. Function address list

All addresses are IROM/IROM-flash unless noted. Names are auto-generated
(`FUN_segN__ADDR`); the **evidence** column records why the role is asserted.
`[VERIFIED]` = directly observed (entry-point label, or the literal-pool
pointer decode described in §7.3). `[INFERENCE]` = string/pointer proximity or
call-pattern reasoning, per the L32R caveat in
[decompile.md](decompile.md).

### 10.1 Entry / boot (IRAM)

| Address | Ghidra name | Role | Evidence |
|---------|-------------|------|----------|
| `0x400813a8` | `FUN_seg3__400813a8` | **`call_start_cpu0`** — app entry | `[VERIFIED]` `layout.json` `entry`; decompiled in `output/decompiled/FUN_seg3__400813a8_0x400813a8.c` |
| `0x400812f4` | `FUN_seg3__400812f4` | `start_cpu0` (cache/clock config) | `[VERIFIED]` called by entry at line 37 of its `.c`; name per [subsystems/boot.md](../subsystems/boot.md) |
| `0x400811f8` | `FUN_seg3__400811f8` | ROM-id check helper (compares ROM func ids) | `[INFERENCE]` first IRAM function, called before entry |

### 10.2 Reset / wakeup / power

| Address | Ghidra name | Role | Evidence |
|---------|-------------|------|----------|
| `0x400ea330` | `FUN_seg4__400ea330` | `checkForAlarmActivation` (RTC alarm) | `[VERIFIED]` refs string `checkForAlarmActivation` |
| `0x400ea444` | `FUN_seg4__400ea444` | `checkWakeupReason` (returns wake cause) | `[VERIFIED]` refs string `checkWakeupReason`; calls `0x400ea330` |
| `0x400e94d4` | `FUN_seg4__400e94d4` | factory-test-alarm wake handler | `[VERIFIED]` refs `FACTORY TEST ALARM HAS WOKEN US` |
| `0x400eb908` | `FUN_seg4__400eb908` | startup info logger | `[VERIFIED]` refs `START-UP INFO` |
| `0x400ebaf8` | `FUN_seg4__400ebaf8` | reset-reason decode/log (`START-UP INFO`) | `[VERIFIED]` refs `Unexpected system reset reason` + `START-UP INFO` |
| `0x400d6624` | `FUN_seg4__400d6624` | reboot (`esp_restart` wrapper) | `[VERIFIED]` refs `Rebooting...` |
| `0x40103e08` | `FUN_seg4__40103e08` | power-manager shutdown/deep-sleep/restart | `[VERIFIED]` refs `Restarting the system with esp_restart`, `Deep sleep due to power down`, `Entering Deep Sleep as requested`, `Unlatching VIN_HOLD` |
| `0x40104704` | `FUN_seg4__40104704` | power-mgmt init (cold boot) | `[VERIFIED]` refs `Cold boot, so no previous power management` |
| `0x401d038c` | `FUN_seg4__401d038c` | SD boot-count read/write | `[VERIFIED]` refs `Read boot count`; file `/sdcard/boot_cnt` |

### 10.3 OTA gate & update

| Address | Ghidra name | Role | Evidence |
|---------|-------------|------|----------|
| `0x400f7dc8` | `FUN_seg4__400f7dc8` (110 B) | `ota_gate` task **starter** | `[VERIFIED]` refs `ota_gate` TAG (`0x400d33bc`); takes mutex → `xTaskCreate`(body `0x400f793c`) → releases |
| `0x400f793c` | *(not decompiled)* | `ota_gate` task loop body | `[INFERENCE]` code ptr at literal `0x400d33c0` |
| `0x400f7f48` | *(not decompiled)* | OTA update flow entry (callback) | `[INFERENCE]` code ptr at literal `0x400d34d4` |
| `0x400f8110` | `FUN_seg4__400f8110` (782 B) | **`ota_update`** main (begin/write/end) | `[VERIFIED]` refs `ota_update` TAG, `esp_ota_begin succeeded`, `Starting OTA update` |
| `0x400f8424` | `FUN_seg4__400f8424` (371 B) | URL build + HTTP client | `[INFERENCE]` refs `?channel=`/`&version=` fragments, `0x400f85ac`/`0x400f8640` call it |
| `0x400f85ac` | `FUN_seg4__400f85ac` (144 B) | OTA status wrapper | `[INFERENCE]` refs `ota_update` TAG |
| `0x400f8640` | `FUN_seg4__400f8640` (236 B) | OTA status wrapper (UI/silent) | `[INFERENCE]` refs `ota_update` TAG |
| `0x400f75f0` | `FUN_seg4__400f75f0` (485 B) | version-API query (`OTA_VAPI_*`) | `[INFERENCE]` size + cluster location |
| `0x400f8b68` | `FUN_seg4__400f8b68` | OTA precheck helper | `[INFERENCE]` called by `0x400f85ac`/`0x400f8640` |

### 10.4 Shared lib helpers (identified by call signature)

| Address | Ghidra name | Role | Evidence |
|---------|-------------|------|----------|
| `0x40116310` | `FUN_seg4__40116310` | log timestamp getter (`%lu` ms) | `[INFERENCE]` called immediately before every logger call |
| `0x40116320` | `FUN_seg4__40116320` | logger (`level, tag, line, fmt, time, …`) | `[INFERENCE]` 6-arg vprintf-style; format `E/I/W/D (%lu) %s: …` |
| `0x40114cac` | `FUN_seg4__40114cac` | task create (`xTaskCreate`) | `[INFERENCE]` called by `0x400f7dc8` with fn ptr + stack `0x2000` |
| `0x40115454` / `0x40115470` | `FUN_seg4__40115454` / `…470` | mutex lock / unlock | `[INFERENCE]` acquire/release pair around OTA gate |

### 10.5 Factory console command handlers

| Address | Ghidra name | Role | Evidence |
|---------|-------------|------|----------|
| `0x401de654` | `FUN_seg4__401de654` | `cmd_restart` | `[VERIFIED]` refs `cmd_restart` |
| `0x401de6a8` | `FUN_seg4__401de6a8` | `cmd_system` (and `0x401de8b0`, `0x401dea4c`) | `[VERIFIED]` refs `cmd_system` |

## 11. Raw file pointers

| Artifact | Content |
|----------|---------|
| `output/layout.json` | partition table (`partitions[]`), flash size, app segments + entry (`images.factory`) |
| `output/factory.bin` | 2.45 MB app image (entry `0x400813a8`) |
| `output/strings.txt` | 25,732 unique printable strings — **ground truth** for §4–§8 |
| `output/strings_categorized.json` | strings grouped by subsystem keyword |
| `output/hwconfig_00..05_*.json` | six per-revision pin maps |
| `output/pinmap.json` | flattened `GPIO.x`/`ADC.x`/`IOX.p.n` → dotted function path |
| `output/hwconfig_merged.json` | merged default (dual-IOX) config |
| `output/decompiled/FUN_seg3__400813a8_0x400813a8.c` | decompiled `call_start_cpu0` |
| `output/decompiled/FUN_seg4__400f7dc8_*.c`, `FUN_seg4__400f8110_*.c` | OTA gate starter / OTA update |
| `output/decompiled_manifest.json` | 5,915 decompiled functions (addr/name/size/file) |
| `output/ghidra_functions.json` | 9,185 functions (file-offset addr/name/size) |
| `output/string_xrefs.json` | sparse L32R xrefs (near-empty — see caveat) |
| `analysis/extract_app.py`, `extract_strings.py`, `extract_hwconfig.py` | extraction scripts |
| `analysis/ghidra_decompile.py`, `ghidra_dump.py`, `ghidra_esp32.py`, `MapESP32.java` | decompile/mapping scripts |

## 12. Caveats

- **L32R caveat** (from [decompile.md](decompile.md)): Ghidra's generic Xtensa
  does not resolve `L32R` literal-pool refs, so string→function xrefs are
  sparse (`output/string_xrefs.json` has one entry; `keyword_functions.json` is
  `{}`). §10 addresses marked `[INFERENCE]` were recovered by manually decoding
  the literal pool (§7.3) or by call-pattern analysis, not by Ghidra xrefs.
- The second-stage bootloader (flash `0x1000`) is **not** covered by these
  artifacts; `output/layout.json` only describes the factory **app** image.
- `BOOTCOUNT` (`strings.txt:8074`) is an SD-status JSON key, distinct from the
  SD boot-counter file `/sdcard/boot_cnt` (`:8011`).
