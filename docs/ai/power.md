# Power & Battery — CW2215B/CW2015, SGM41513/41511/ETA6003, HUSB238, CV8013N/CV8085

Dense reference for AI agents reverse-engineering the Yoto ESP32 firmware power subsystem:
multi-source input (USB-C PD + Qi wireless + Li-ion battery), a fuel gauge, a charger controller,
and an IO-expander-driven power-control / power-sequencing path.

**Ground truth is `output/strings.txt` (line numbers) + `output/hwconfig_*.json` + `output/pinmap.json`.**
Ghidra generic Xtensa does **not** resolve L32R literal-pool string refs, so string→function xrefs
were recovered manually (see §5) by scanning the IROM code segment for 32-bit little-endian pointers
to rodata. Roles marked `[INFERENCE]` unless directly evidenced.

---

## 1. Chip / component inventory

| Function | Chips (by variant) | Interface | I2C addr (7-bit) | Evidence |
|---|---|---|---|---|
| Fuel gauge | **CW2215B** (#04/#05), **CW2015** (#00/#02), **ADC** (#01) | I2C / SAR-ADC | CW2215B `0x64`, CW2015 `0x62` | decompiled `FUN_seg4__400e83c8` calls I2C helper w/ `100`=0x64; `FUN_seg4__400e7d38` w/ `0x62` (§4.1) |
| Charger | **SGM41513** (#04/#05), **SGM41511** (#02), **ETA6003** (#00/#01) | I2C | SGM `0x6b`; second charger path `0x1a` | `FUN_seg4__400e5c78` branches on charger type: type 2 → `0x6b`, type 3 → `0x1a` (§4.2) |
| USB-C PD sink | **HUSB238** (#02/#05) | I2C | `0x08` `[INFERENCE]` (datasheet; not in config) | config key `usbc.type="HUSB238"` |
| Qi wireless RX | **CV8013N** (#05), **CV8085** (#02) | I2C | CV8085 `0x13` (explicit), CV8013N `null` → `0x50` `[INFERENCE]` (datasheet) | `hwconfig_02` `qi.i2caddr="0x13"`; `hwconfig_05` `qi.i2caddr=null` |
| IO expander | **PI4IOE5V6416** ×1 or ×2 (`iox.type` `"ET6416"`/`"PI4IOE5V6416"`) | I2C | (shared bus, see audio/storage docs) | all `iox` blocks |
| Light sensor | SAR-ADC1 CH0 = GPIO36 | ADC | — | `lightsensor.pin="GPIO.36"`, `adc="ADC.1.0"` |
| NTC temp sensor | SAR-ADC1 CH3 = GPIO39 | ADC | — | `tempsensor.pin="GPIO.39"`, `adc="ADC.1.3"` |
| Qi RX IR temp sensor | SAR-ADC1 CH7 = GPIO35 | ADC | — | `qirxtempsensor.pin="GPIO.35"`, `adc="ADC.1.7"` (#05 only) |

> **Correction to `docs/docs/hardware.md`:** `hwconfig_00` charger is **ETA6003**, not SGM41513
> (the JSON `battery.charger.type="ETA6003"` is authoritative). The 6-variant charger/fuel-gauge
> matrix below reflects the JSON, not the earlier prose table.

---

## 2. Per-variant power config (all 6 revisions)

`output/hwconfig_00..05_*.json` — filename encodes the stack
(`audiosplit|audiocombined`, `spk…`, `hp…`, `disp…`, `nfc…`, `sd…`, `bat…`). The filename's final
`bat<X>` token is the fuel-gauge/monitor type.

### 2.1 Battery monitor + charger + usbc + qi blocks

| Variant | monitor.type | monitor.btype | monitor.battalrt | charger.type | charger.plugstat | charger.chgstat | charger.chgbst |
|---|---|---|---|---|---|---|---|
| `hwconfig_00_…_batCW2015.json` | `CW2015` | `JY734352` | `IOX.1.0` | `ETA6003` | `IOX.1.5` | `IOX.1.7` | `IOX.0.7` |
| `hwconfig_01_…_batADC.json` | `ADC` | `AS-R18650-2600-112` | *(none)* | `ETA6003` | `IOX.1.5` | `IOX.1.7` | `null` |
| `hwconfig_02_…_batCW2015.json` | `CW2015` | `AS-21700-4500-113` | `IOX.0.6` | `SGM41511` | `null` | `IOX.1.4` | `null` |
| `hwconfig_03_audio?_…_bat?.json` | *(none)* | — | — | `SGM41513` | — | — | — |
| `hwconfig_04_…_batCW2215B.json` | `CW2215B` | `UTL-FD70X-2000` | `IOX.1.0` | `SGM41513` | `IOX.1.5` | `IOX.1.7` | `null` |
| `hwconfig_05_…_batCW2215B.json` | `CW2215B` | `LJDX30X-4500` | `IOX.0.6` | `SGM41513` | `null` | `IOX.1.4` | `null` |

`usbc` block (only #02, #05 — identical pins):

```json
"usbc": { "type": "HUSB238", "nvbusstat": "IOX.1.0", "nvbuschgen": "IOX.2.7" }
```

`qi` block (only #02, #05):

```json
// #02  (hwconfig_02)
"qi": { "type": "CV8085", "nqistat": "IOX.0.7", "nqichgen": "IOX.2.6",
        "qien5w": null, "qii2cint": null, "i2caddr": "0x13" }
// #05  (hwconfig_05)
"qi": { "type": "CV8013N", "nqistat": "IOX.0.7", "nqichgen": "IOX.2.6",
        "qien5w": "IOX.3.5", "qii2cint": "IOX.0.0", "i2caddr": null }
```

### 2.2 Power-control block (`powercontrol`) — single-IOX vs 2-IOX

```json
// single-IOX boards (#00, #04): no vouten
"powercontrol": { "levelconvertor": "IOX.0.3", "pwren": "IOX.1.4", "vinhold": "IOX.1.6" }
// #01 (ADC board): pwren/levelconvertor absent (power always on)
"powercontrol": { "levelconvertor": null, "pwren": null, "vinhold": "IOX.1.6" }
// 2-IOX boards (#02, #05): adds vouten
"powercontrol": { "levelconvertor": "IOX.3.0", "pwren": "IOX.2.5",
                  "vinhold": "IOX.3.1", "vouten": "IOX.3.3" }
```

Pin semantics (from `strings.txt:2752–2764` "Setting IOX pins to inputs … except for …"):

| Signal | Meaning | Evidence |
|---|---|---|
| `pwren` | main power-enable (latch the board ON) | `powercontrol.pwren` |
| `vinhold` | latch VIN while powered; **unlatched** at shutdown | `strings.txt:2816` `Unlatching VIN_HOLD` / `2817` `NOT unlatching VIN_HOLD` |
| `vouten` | downstream rail output enable (2-IOX only) | `powercontrol.vouten` |
| `levelconvertor` | level-shifter enable for the IOX bus | `strings.txt:2755` `… %s level convertor` |

### 2.3 ADC sensors (power-relevant)

| Variant | lightsensor | tempsensor (NTC) | qirxtempsensor (Qi RX IR) |
|---|---|---|---|
| #00 | `null` | `null` | `null` |
| #01 | `GPIO.36` / `ADC.1.0` | `null` | `null` |
| #02 | `GPIO.36` / `ADC.1.0` | `GPIO.39` / `ADC.1.3` | `null` |
| #04 | `null` | `null` | `null` |
| #05 | `GPIO.36` / `ADC.1.0` | `GPIO.39` / `ADC.1.3` | `GPIO.35` / `ADC.1.7` |

Power button: `buttons.power = "IOX.1.3"` on **all** variants (also `hwconfig_03`).

---

## 3. Battery profiles (`btype` → `batt_profiles[]`)

The `monitor.comment` in every config warns: **`MAKE SURE btype FIELD MATCHES batt_profiles[] in batt_profile.c`**
(`strings.txt:3536`-adjacent JSON; source file named in `strings.txt:3534`/`3750`).

Five distinct profiles appear across the six configs (exact strings):

| btype | Variant | monitor.type |
|---|---|---|
| `JY734352` | #00 | CW2015 |
| `AS-R18650-2600-112` | #01 | ADC |
| `AS-21700-4500-113` | #02 | CW2015 |
| `UTL-FD70X-2000` | #04 | CW2215B |
| `LJDX30X-4500` | #05 | CW2215B |

Profile lookup: `batt_prof_get_active` (fn `0x400e6fa0`, §5), profile data struct
`$@BATT_PROFILE` (strings.txt:959 `batt_chrg_get_power_caps`, `960` `$@BATT_PROFILE`).

---

## 4. Function addresses (flash IROM `0x400d0020–0x4025f0ec`, seg4)

Decompiled files: `output/decompiled/FUN_seg4__<addr>_<addr>.c`; manifest
`output/decompiled_manifest.json`. The power module is one contiguous cluster
**`0x400e6048`–`0x400e914c`** (62 functions) plus a power-management/shutdown cluster at
**`0x401036e8`–`0x40104808`**.

### 4.1 Fuel gauge (CW2215B / CW2015) driver

| Address | Size | Likely role | Evidence |
|---|---|---|---|
| `0x400e7d38` | 383 | **`cw2015_init`** | `__func__` string `cw2015_init` → literal `0x400d1c2c`; I2C writes addr `0x62` reg `10`/`8`, profile at regs `0x10+` |
| `0x400e7ef0` | 113 | CW2215B multi-byte reg read | called by `0x400e8048` with reg `4` (VCELL) and `2` (SOC) |
| `0x400e7f64` | 101 | **`_cw_get_battery_profile`** | `__func__` str `_cw_get_battery_profile` → `0x400d1c4c` |
| `0x400e7fcc` | 118 | **`_cw2215b_sleep`** | `__func__` str `_cw2215b_sleep` → `0x400d1c58` |
| `0x400e8048` | 166 | **`_cw2215b_get_voltage` + `_cw2215b_get_capacity_percentage`** (Ghidra merged two small fns) | refs both `__func__` strings (`0x400d1c68`,`0x400d1c70`); reads reg `4`→voltage (VCELL), reg `2`→SOC `= (val*5)>>4` % |
| `0x400e80f0` | 146 | **`cw2215b_get_temperature`** | `__func__` → `0x400d1c7c`; log `read 0x%02x = %d Celsius` (strings.txt:1063) |
| `0x400e8184` | 219 | **`cw2215b_get_current`** | `__func__` → `0x400d1c88`; logs `REGH`/`REGL` (strings.txt:1066) |
| `0x400e82a8` | 212 | **`cw2215b_set_alert_level`** | `__func__` → `0x400d1c9c`; log `Battery SOC alert set to %d%%` (strings.txt:1047) |
| `0x400e8380` | 70 | **`cw2215b_clear_alert`** | `__func__` → `0x400d1cb4` |
| `0x400e83c8` | 1048 | **`cw2215b_init`** (largest fuel-gauge fn; also calls/embeds `cw2215b_active`, `_cw2215b_write_profile`, `_cw2215b_config_start_ic`, `_cw2215b_get_state`) | `__func__` strings `0x400d1cc4/1d18/1d00/1cf0/1cd4`; I2C addr `0x64`; reads reg `0xab` (version), reg `0` (chip ID, must equal `0xA0`) |
| `0x400e87f0` | 206 | **`cw2215b_get_cycle_count`** | `__func__` → `0x400d1d3c`; log `Cycle count %u` (strings.txt:1100) |
| `0x400e88c4` | 141 | **`cw2215b_get_state_of_health`** | `__func__` → `0x400d1d4c` |

CW2215B register addresses observed in `0x400e83c8`: `0xab` (read at init), `0x00`, `0x08`, `0x0b`.
Registers by symbol name (from log strings, addresses not all pinned `[INFERENCE]`):
`CW2215B_REG_MODE_CONFIG`, `CW2215B_REG_SOC_INT`, `CW2215B_REG_VCELL_H`,
`CW2215B_REG_TEMP`, `CW2215B_REG_CURRENT_H/L`, `CW2215B_REG_GPIO_CONFIG`,
`CW2215B_REG_SOC_ALERT`, `CW2215B_REG_BAT_PROFILE`, `CW2215B_REG_CYCLE_H/L`,
`CW2215B_REG_SOH` (strings.txt:1056–1101).

### 4.2 Charger state machine / charging control

| Address | Size | Likely role | Evidence |
|---|---|---|---|
| `0x400e6194` | 136 | **`handle_chg_ramp_state`** | `__func__` → `0x400d19fc`; ramp state logs (strings.txt:922–925) |
| `0x400e6220` | 291 | **`set_charging`** (set charge current, ramp) | `__func__` → `0x400d1a08`; log `Charge Current request %u, Calculated register value %u` (strings.txt:928) |
| `0x400e5c78` | 120 | low-level charger register write (I2C) | called by `0x400e6220`; branches type2→`0x6b`, type3→`0x1a` |
| `0x400e662c` | 508 | **`set_charging_enabled`** (enable/disable + reasons) | `__func__` → `0x400d1a50`; logs `Battery charging (re-)enabled/disabled. Reason: %s` (strings.txt:947–948) |
| `0x400e6cd0` | 76 | **`batt_chrg_get_power_caps`** | `__func__` → `0x400d1aa8`; delegates to `0x400e64b4` via device struct |
| `0x400e6fa0` | 68 | **`batt_prof_get_active`** | `__func__` → `0x400d1b04` |
| `0x400e7398` | 295 | **`check_ntc_voltage`** | `__func__` → `0x400d1b88`; NTC logs (strings.txt:1023–1025) |
| `0x400e74c4` | 597 | **`get_charging_parameters`** (charger params read) | `__func__` → `0x400d1b98` |
| `0x400e8e08` | 699 | **`get_charging_parameters`** (larger variant / Qi+USB path) | `__func__` → `0x400d1db0`; Qi/USB power selection logs (strings.txt:953–956) |
| `0x400e90cc` | 64 | **`get_pd_values`** | `__func__` → `0x400d1df0`; PDO read |
| `0x400e9110` | 59 | **`get_non_pd_reg_value`** | `__func__` → `0x400d1df8` |

### 4.3 Battery UI / timer / events

| Address | Size | Likely role | Evidence |
|---|---|---|---|
| `0x400e7868` | 170 | **`battery_ui`** (icon selection) | `__func__` → `0x400d1bd4`; log `Finding icon for: powered=%d, charging=%d, percent=%d` (strings.txt:1038) |
| `0x400e7934` | 312 | **`battery_timer`** (+ `battery_ui` refs) | `__func__` `battery_timer` → `0x400d1bfc`; logs `Failed to create battery timer` (strings.txt:1044) |

### 4.4 Power management / shutdown / deep-sleep (cluster `0x401036e8`+)

| Address | Likely role | Evidence |
|---|---|---|
| `0x401036e8`, `0x40103a60`, `0x40103a90`, `0x40103ad8`, `0x40103cf8`, `0x40103e08` | `power_management` sub-functions (sleep config, IOX pin power-down) | refs `power_management` tag `0x400d4910`; logs `Setting IOX pins to inputs` / `…except for VIN_HOLD pin` (strings.txt:2757–2764) |
| `0x40104498` | `fw_power_man` orchestrator (references both `power_management` and `fw_power_man`) | `__func__` `fw_power_man` → `0x400d4a9c` |
| `0x401045f4`, `0x401046b4`, `0x40104704`, `0x401047a4`, `0x40104808` | `fw_power_man` shutdown / deep-sleep / power-off sequence | refs `fw_power_man` `0x400d4a9c/4ab4`; logs `Deep sleep or power down requested`, `Unlatching VIN_HOLD`, `Power button not pressed` (strings.txt:2807–2821) |

### 4.5 I2C + log primitives (shared)

| Address | Role | Evidence |
|---|---|---|
| `0x4010c930` | I2C **read** register `(addr, reg, &buf)` | called as `FUN_seg4__4010c930(100, 0xab, …)`, `(0x62, 0, …)` |
| `0x4010c948` | I2C **write** register `(addr, reg, val)` | `FUN_seg4__4010c948(0x62, 10, 0xf)` |
| `0x40116310` | `esp_log_timestamp` | log call wrapper |
| `0x40116320` | `esp_log_write(level, tag, line, fmt, ts, tag, …)` | log call wrapper |

### 4.6 Smart-cable debouncer

Source path (the only battery source path in strings): `strings.txt:1152`
`//opt/atlassian/pipelines/agent/build/components/battery/smartcable_debouncer.cpp`.
Log tag `sc_deb` / `SMART_DEB` (strings.txt:1147–1148); task create `Failed to create debouncer task`
(1149); state dump `handshakeToggleCount_ … nowCharging … powerSource_` (1150); USB attach/detach
debounce drives `VBUS_CHG_EN pin => %s` (1126) and PD detect (1130).

---

## 5. How the string→function map was recovered

Ghidra does not emit string xrefs for generic Xtensa. The addresses in §4 were derived by:
1. Compute each `__func__`/log string's rodata VA (`DROM 0x3F400020` + file offset − 32).
2. Scan IROM (`0x400D0020`, file offset `720928`) for the little-endian pointer to that VA
   (the literal-pool entry).
3. Grep `output/decompiled/*.c` for `DAT_seg4__<poolVA>`.

Example: `cw2215b_init` (file off `0xac65`) → rodata VA `0x3f40ac65` → literal pool `0x400d1cc4`
→ referenced only by `FUN_seg4__400e83c8_0x400e83c8.c`. Fuel-gauge `__func__` strings live
contiguously at `strings.txt:1103–1117` (rodata `0x3f40ab2a`–`0x3f40ac65`).

---

## 6. Protocol / flow detail

### 6.1 CW2215B init sequence (`0x400e83c8`)

Reconstructed from decompiled C + log strings (strings.txt:1056–1093):

1. Read chip ID via I2C `0x64` (checks `!= 0xA0` → `unexpected chip ID detected`).
2. Read `CW2215B_REG_MODE_CONFIG`; if non-active → set `restart + sleep`
   (`MODE_CONFIG set: restart + sleep`).
3. Verify current profile (`CW2215B_REG_BAT_PROFILE`); if stale → `_cw2215b_sleep`,
   `_cw2215b_write_profile` (`battery profile has been written to CW2215B`).
4. Clear `CW2215B_REG_SOC_ALERT` UPDATE flag; set alert level via `cw2215b_set_alert_level`.
5. Set `restart + active`, poll `_cw2215b_get_state` for ready (`ready-state timeout. Requesting sleep`).
6. Readback: voltage (VCELL `4`), SOC (`2`), temp, current (H/L), cycle count (H/L), SOH.

### 6.2 CW2015 init sequence (`0x400e7d38`)

I2C `0x62`: read reg `0` (version, logged `Initialised CW2015. Version = [%d]`, strings.txt:1049),
read reg `8`; select profile via `batt_prof_get_active`; write profile to regs `0x10+`;
write reg `10` (`0xf` then `0`), reg `8` (config bits). Register names from datasheet:
`0x00`=version, `0x02/0x03`=VCELL, `0x04/0x05`=SOC, `0x08`=CONFIG, `0x0A`=MODE, `0x10+`=profile `[INFERENCE]`.

### 6.3 Charging state machine + power-source arbitration

- Charge ramp: `handle_chg_ramp_state` (SLOW/FAST/CHRG_SMART enum strings at strings.txt:1118–1120)
  ramps `set_charging` current in steps; `set_charging_enabled` applies reasons.
- Source selection: `Best src is USB … / Best src is Qi …` (strings.txt:953–956) chooses between
  HUSB238 USB-C and CV8013N/CV8085 Qi; toggles `nvbuschgen`/`nqichgen` IOX pins.
- NTC temperature capping: `check_ntc_voltage` reads NTC over I2C
  (`Read NTC voltage 0x%02x%02x (%d)`, strings.txt:1024); Qi overheat → input-current capping
  (strings.txt:1026–1028); guards charge re-enable (`batt temp OK, but not re-enabling charging`,
  941).
- Charge-stop reasons (strings.txt:930–943): `system bootup`, `power source is disconnected`,
  `battery level below % threshold`, `battery temperature charge-stop threshold reached`,
  `Qi is disconnected`, `External power is connected but no longer charging`.

### 6.4 USB-C PD (HUSB238)

`get_pd_values` / `get_non_pd_reg_value` classify the source: PD charger (`detected PD charger,
voltage %d current %d`, strings.txt:1130), non-PD QC / Apple / DCP/CDP (1131–1134), with
`wasPowered_`/`notYetFound` handshake state. Factory prodtest reads PDOs
(`PDO: 5V/9V/12V/15V/18V/20V %lumA`, strings.txt:8751–8756; `DPDM: 0x%02X`, 8757).

### 6.5 Shutdown / deep-sleep (`fw_power_man`)

Shutdown reasons (strings.txt:2455–2463): `emptyBattery`, `userShutdown`, `mqttShutdown`,
`tickOverflow`, `inactivityShutdown`, `sdFail`, `lockError`, `batteryOverTemp`,
`batteryUnderTemp`, `batteryFault`. Sequence: power-off amps/display/ambient → unlatch `VIN_HOLD`
(2816) → `esp_deep_sleep` (2811). Wake on RTC alarm (`RTC_INT is LOW (alarm triggered);
cancelling unlatch and rebooting`, 2818) or power button.

---

## 7. Config / NVS keys (power-relevant)

From `strings.txt:7688–7838` (settings schema) and `7780–7838` (key list):

| Key | Default | Meaning |
|---|---|---|
| `POWER_MAN_MODE` | `2` (max `3`) | power-management mode |
| `PS_LIGHTSLEEP` | — | light-sleep enable |
| `BATT_PROFILE_ID` | — | active battery profile |
| `APP_BATFULL_PCT` | `51` | battery-full threshold % |
| `BATT_THRS_START` | `4` | power-on threshold % |
| `BATT_THRS_OTA` | `15` | OTA battery threshold % |
| `BATT_THRS_SAP` | — | SAP threshold % |
| `BATT_THRS_WRNG` | `7` | warning threshold % |
| `BATT_THRS_FLAT` | — | flat-battery threshold % |
| `APP_BATT_FAULT` | `0` (bool, external) | battery fault flag |
| `APP_V3BVOLCAPS` | — | V3 battery-voltage caps |
| `PS_SWITCHFREQ` | — | power switch frequency |

Events: `EVT_CONNECTED_TO_POWER` (strings.txt:3380), `EVT_DISCONNECTED_FROM_POWER` (3381).
Sound files: `/system/sounds/{connected_to_power,disconnected_from_power_low,low_battery,battery_fault_beep,…}`
(strings.txt:307–314).

---

## 8. Key exact strings (with `output/strings.txt` line numbers)

| Line(s) | String |
|---|---|
| 915–920 | `Previous state change reason: Qi disconnected / Battery full / Temperature stop / Unknown / Invalid` |
| 920 | `NTC OT suspected (%d CHG_STAT edge transitions within %d seconds, mV = %d)` |
| 922–926 | `handle_chg_ramp_state` / `set_charging` |
| 928 | `Charge Current request %u, Calculated register value %u` |
| 930–937 | charge-stop reasons (`system bootup`, `Qi is disconnected`, …) |
| 939 | `set_charging_enabled` |
| 947–948 | `Battery charging (re-)enabled, Reason: %s` / `disabled. Reason: %s` |
| 953–956 | `Best src is USB (USB power %lu is >= Qi power %lu)` / `Disabling Qi, Enabling USB power` / `Best src is Qi …` |
| 959–960 | `batt_chrg_get_power_caps` / `$@BATT_PROFILE` |
| 993 | `batt_prof_get_active` |
| 1019–1022 | `Disabled (HI)` / `QI_RX` / `QI_CHG_EN pin => %s` / `5W_QI_EN pin => %s` |
| 1023–1025 | `No NTC voltage read: Qi is not powered` / `Read NTC voltage 0x%02x%02x (%d)` / `Failed to read NTC voltage over I2C` |
| 1026–1028 | Qi overheat capping activate/deactivate |
| 1027 | `check_ntc_voltage` |
| 1030 | `get_charging_parameters` |
| 1037 | `battery_ui` |
| 1038 | `Finding icon for: powered=%d, charging=%d, percent=%d` |
| 1043 | `battery_timer` |
| 1046 | `Calculated New Reading of %dmV. Raw Data 0x%x` |
| 1047 | `Battery SOC alert set to %d%% (reg value 0x%02x)` |
| 1049 | `Initialised CW2015. Version = [%d]` |
| 1054 | `cw2015_init` |
| 1056–1101 | `CW2215B_REG_*` init/read log strings (MODE_CONFIG, SOC_INT, VCELL_H, TEMP, CURRENT_H/L, GPIO_CONFIG, SOC_ALERT, BAT_PROFILE, CYCLE_H/L, SOH) |
| 1073–1075 | `Initialised CW2215B. Version = [%d]` / `Chip ID = [%d]` / `unexpected chip ID detected (%d)` |
| 1103–1117 | CW2215B `__func__` symbol cluster (`cw2215b_get_state_of_health` … `cw2215b_init`) |
| 1118–1120 | `SLOW` / `FAST` / `CHRG_SMART` |
| 1121–1124 | `Smart Cable logic started/completed/toggled (%s)` |
| 1126 | `VBUS_CHG_EN pin => %s` |
| 1129–1134 | `detected PD charger, voltage %d current %d` / non-PD QC/Apple/DCP-CD/unknown |
| 1142 / 1144 | `get_pd_values` / `get_non_pd_reg_value` |
| 1147–1150 | `sc_deb` / `SMART_DEB` / `Failed to create debouncer task` / debouncer state dump |
| 1152 | `//opt/atlassian/pipelines/agent/build/components/battery/smartcable_debouncer.cpp` |
| 2451–2453 | `qirx_temp_sensor` / `Qi RX: tsADC=%d, Celsius=%d, milliVolts=%d (average of %d valid readings)` |
| 2455–2463 | shutdown-reason enums (`emptyBattery`, `batteryOverTemp`, `batteryFault`, …) |
| 2568–2606 | telemetry field names (`powerCaps`, `batteryLevel`, `batteryTemp`, `batteryData`, `batteryLevelRaw`, `batteryProfile`, `powerSrc`, `qiOtp`, `chgStatLevel`, `batteryFullPct`) |
| 2752 | `power_management` |
| 2755–2767 | `Setting IOX pins to inputs … except for VIN_HOLD / level convertor / USB VBUS_CHG_EN / QI_CHG_EN / CHG_BOOST / 5W_QI_EN / QI_I2C_INT / HP_DET pin` |
| 2789–2821 | deep-sleep / power-down / shutdown sequence (`Unlatching VIN_HOLD`, `Deep sleep due to power down`, `Power button not pressed; returning to deep sleep state`) |
| 2822 | `fw_power_man` |
| 3380–3381 | `EVT_CONNECTED_TO_POWER` / `EVT_DISCONNECTED_FROM_POWER` |
| 3401–3422 | config keys (`battalrt`, `HUSB238`, `nvbusstat`, `nvbuschgen`, `CV8085`, `CV8013N`, `nqistat`, `nqichgen`, `qien5w`, `qii2cint`, `ETA6003`, `SGM41511`, `SGM41513`, `plugstat`, `chgstat`, `chgbst`) |
| 3491–3496 | `powercontrol`, `levelconvertor`, `pwren`, `vinhold`, `vouten` |
| 3534 / 3750 | `"comment":"MAKE SURE btype FIELD MATCHES batt_profiles[] in batt_profile.c"` |
| 7780 / 7815 / 7823–7828 / 7838 | `BATT_PROFILE_ID`, `POWER_MAN_MODE` (default 2), `APP_BATFULL_PCT` (51), `BATT_THRS_*`, `PS_LIGHTSLEEP` |
| 8751–8757 | `PDO: 5V/9V/12V/15V/18V/20V %lumA` / `DPDM: 0x%02X` |
| 8782 | `Charger only supports 5V, 9V and 12V` |

---

## 9. Raw file pointers

- `output/strings.txt` — all strings (line numbers cited above).
- `output/hwconfig_00..05_*.json` — per-variant `battery.{monitor,usbc,qi,charger}`, `powercontrol`, sensors, `iox`.
- `output/pinmap.json` — flattened `GPIO.x` / `ADC.1.n` / `IOX.p.n` → JSON path (keys:
  `battery.monitor.battalrt`, `battery.charger.{plugstat,chgstat,chgbst}`, `battery.usbc.{nvbusstat,nvbuschgen}`,
  `battery.qi.{nqistat,nqichgen,qien5w,qii2cint}`, `powercontrol.{levelconvertor,pwren,vinhold,vouten}`,
  `lightsensor/tempsensor/qirxtempsensor.{pin,adc}`, `buttons.power`).
- `output/decompiled_manifest.json` + `output/decompiled/FUN_seg4__400e*.c` — decompiled functions (§4).
- `output/ghidra_functions.json` — full 10k function list (generic `FUN_*` names; power module fns not name-resolved there).
- `output/layout.json` — segment map (DROM `0x3F400020` rodata; IROM `0x400D0020` code).
- `output/strings_categorized.json` — `power` category (337 hits) for keyword fan-out.

> Manual literal-pool scan technique for re-deriving any string xref is described in §5.
