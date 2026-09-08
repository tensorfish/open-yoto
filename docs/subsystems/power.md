---
icon: lucide/battery-charging
---

# Power & Battery

Power management is multi-source (USB-C PD, Qi wireless, battery) with a fuel
gauge, charger controller, and an IO-expander-driven power-control path. Two
distinct power-control blocks exist: older **single-IOX** boards and newer
**2-IOX** boards.

## Devices

| Function | Chips | Interface | Notes |
|----------|-------|-----------|-------|
| Fuel gauge | **CW2215B** (#04/#05), **CW2015** (#00/#02), **ADC** (#01) | I2C / ADC | `battalrt`: IOX0.6 (2-IOX) / IOX1.0 (single-IOX) |
| Charger | **SGM41513** (#03/#04/#05), **SGM41511** (#02), **ETA6003** (#00/#01) | I2C | `chgstat`: IOX1.4 (2-IOX) / IOX1.7 (single-IOX); `plugstat`/`chgbst` on some |
| USB-C PD sink | **HUSB238** | I2C | VBUS status IOX1.0, charge-en IOX2.7 |
| Qi RX | **CV8013N** (#05), **CV8085** (#02) | I2C | status IOX0.7, charge-en IOX2.6; 5 W-en IOX3.5 only on CV8013N |

The earliest config (#01) reads battery voltage via ADC (`ADC.1.3`) with no
fuel gauge. Later units use `CW2015`/`CW2215B`, which report voltage and
capacity percentage (`_cw2215b_get_capacity_percentage`,
`_cw2215b_get_voltage`).

## Power-control path

The clean factory app confirms the #04 single-ET6416 run state:

```text
ET6416 @ 0x20
VINHOLD        IOX.1.6 = HIGH
PWREN          IOX.1.4 = LOW
levelconvertor IOX.0.3 = HIGH
PACTRL         IOX.0.6 = LOW, then LOW/HIGH during amp reset
```

The IOX latch defaults precede these explicit `app_main` transitions. They
must not be treated as the final pin state. These rails precede speaker
initialization; do not apply #05's second-expander pins to this board.

2-IOX boards:

```text
"powercontrol": {
  "levelconvertor": "IOX.3.0",
  "pwren":          "IOX.2.5",
  "vinhold":        "IOX.3.1",
  "vouten":         "IOX.3.3"
}
```

- `pwren` — main power-enable.
- `vinhold` — latch input voltage while the device is on.
- `vouten` — output enable to downstream rails.
- `levelconvertor` — enables the level shifter for the IO-expander bus.

Older single-IOX boards use a different mapping (see `output/hwconfig_*.json`).
The **power button** is `IOX1.3`; battery/charger status lines gate charging:

```text
D (%lu) %s: batt temp OK, but not re-enabling charging (as charge-capping logic disabled it)
D (%lu) %s: ignoring charging request to re-enable charging (as temperature logic disabled it)
```

## Battery profiles

Battery chemistry is selected by a `btype` string matched against
`batt_profiles[]` in `batt_profile.c`:

```text
"monitor": {
  "comment": "MAKE SURE btype FIELD MATCHES batt_profiles[] in batt_profile.c",
  "btype": "LJDX30X-4500",
  "type": "CW2215B"
}
```

Five profiles appear across the configs: `JY734352`,
`AS-R18650-2600-112`, `AS-21700-4500-113`, `LJDX30X-4500`, and
`UTL-FD70X-2000`. NTC temperature monitoring is present
(`check_ntc_voltage`, `No NTC voltage read: Qi is not powered`).

## Sensors

| Sensor | Pin | ADC |
|--------|-----|-----|
| Light sensor | GPIO36 | ADC1 CH0 |
| Temp sensor (NTC) | GPIO39 | ADC1 CH3 |
| IR RX temp sensor | GPIO35 | ADC1 CH7 |

!!! note
    The `GPIO39 = ADC1 CH3` mapping is corroborated by the `tempsensor` config
    (`pin: GPIO.39`, `adc: ADC.1.3`); the battery monitor JSON names only the
    ADC channel (`vbat: "ADC.1.3"`), not the GPIO number.

## Replacement firmware charging

The replacement currently supports the CW2215B/SGM41513 stack on revisions
`#04` and `#05`. Initialization loads the exact 80-byte battery profile
selected at build time, verifies the gauge is active, then reads VCELL and SOC.

The SGM41513 is configured whenever `REG08.PG_STAT` reports external input and
is configured again after every unplug/replug:

| Board | Battery charge request | Input-current policy |
|-------|------------------------|----------------------|
| #04 | 2220 mA (`ICHG=0x36`) | Preserve the SGM41513 D+/D− source-detector result in `REG00.IINDPM` |
| #05 | 1020 mA (`ICHG=0x28`) | Use the HUSB238 negotiated contract, capped at 2.4 A; preserve the detector result if no contract is readable |

Preserving #04's detected input limit matters: forcing 2.4 A from a weak USB
supply can pull VBUS into input-voltage DPM or repeated supply resets, producing
less effective charging current. The battery-current request remains the stock
2220 mA ceiling; the charger still limits it to what the source can deliver.

Configuration disables OTG and the charger watchdog, applies the stock
board-specific VINDPM/OVP and temperature-control values, and enables charging
only after those settings are in place. Every 30 seconds while power-good stays
asserted, a read-only masked audit checks the complete owned configuration and
repairs reset/watchdog drift. The audit preserves source-authoritative IINDPM
and never clears a latched `BATFET_DIS`, which can represent discharge
over-current protection. UI state comes from `CHRG_STAT`, not the board's
unreliable raw status pins.

## Replacement firmware standby

A **three-second power-button hold** or **30 minutes idle** enters one
serialized, wakeable battery-saving state on rev #04 and rev #05:

1. Publish shutdown and stop Bluetooth/admin without holding the player mutex
   across service teardown; stop audio and blank the display.
2. Power down the LIS2DH12 accelerometer before disabling the level converter.
3. Disable the amplifier, tri-state active-low `PWREN` and rev #05 `VOUTEN`,
   and leave `VIN_HOLD` asserted. Wait for the power button to be released.
4. Clear stale IOX input transitions, unmask the rev #05 power-button interrupt,
   arm GPIO34 EXT1 `ALL_LOW`, isolate GPIO12, and enter deep sleep. If wake
   setup fails, restart rather than sleeping without a working wake source.

**Hold the power button for two seconds to turn it on again.** EXT1 wakes poll
the button every 250 ms; released/spurious interrupts and failed I2C reads
cannot qualify a boot. A rejected wake disconnects restored rails and sleeps
again before battery/display/audio/NFC/SD driver initialization. A qualified boot
releases GPIO12 isolation so rev #04 SD D2 works again. The encoder consumes a
power button already held at startup through release, preventing a second
shutdown from the same wake gesture.

Encoder activity and NFC insertion/removal restart the 30-minute interval.
Playback and an active admin session defer standby. This is ESP deep sleep,
not a periodic light-sleep button-polling loop. Retaining `VIN_HOLD` on battery
is an intentional departure from stock physical-off behavior: normal “off”
keeps the ESP/button wake domain alive instead of relying on a physical re-latch.
Standby current and physical wake reliability still need measurement on hardware.

Stock settings fix normal battery-only startup at 4% SOC and the low-battery
warning at 7%. The stock schema exposes a separate `BATT_THRS_FLAT` but does
not embed its product/profile-specific default. The replacement conservatively
uses 3%: two consecutive fresh five-second samples at or below that value force
the battery-protection transition only when a contemporaneous SGM41513 read
reports `PG_STAT=0`. Invalid I²C samples, noncritical SOC, and external power
clear the confirmation streak. This depleted-battery path releases `VIN_HOLD`
on both revisions; it is distinct from normal wakeable standby.

### Recovery after battery depletion

Depletion is not a persistent firmware lockout. After charging, hold power for
two seconds to start the player. Battery-only startup requires at least 4% SOC;
with valid external power, even a reported 0% SOC is accepted. The SGM41513
supports autonomous precharge and system power with a depleted battery, so
charging does not depend on the player application already running. A damaged
cell, unsafe temperature, or inadequate power source can still prevent recovery.

The shutdown sequence arms GPIO34 wake **before** releasing `VIN_HOLD`. If
power actually disappears, the next startup is a cold boot. If the latch remains
powered or a charger is connected during cutoff, the deep-sleep fallback retains
button wake instead of sleeping with no wake source. No automatic boot-on-charge
behavior is promised; use the power button.

Host regressions cover depleted shutdown fallback, charged cold-start eligibility,
USB-powered zero-SOC wake, and real charger/gauge initialization against a
register model after reset. Physical discharge/recharge remains a hardware check.

## Original firmware reference

The original behavior below was recovered from
`~/Downloads/yoto-player-v2.bin` (SHA-256
`1db52091f892e05a9aec97605890b406f6734213214afef792e247353a75449e`)
with Ghidra 12.0.4. Reproducible artifacts are under
`output/ghidra-power-analysis/`; the focused project maps the ESP image
segments to their linked Xtensa addresses.

Key stock findings:

- `shutdownTimeout` defaults to 3600 seconds (range 30–10800), and
  `FUN_400ebd78` requires every activity-age predicate to reach
  `shutdownTimeout * 1000` before dispatching automatic shutdown.
- `FUN_40103ad8` always submits `light_sleep_enable = false`; a request for
  automatic light sleep logs that `FREERTOS_USE_TICKLESS_IDLE` is not enabled.
- `FUN_40103e08` serializes shutdown, rechecks flat battery, tears down
  download/audio/TDMA/NFC/Wi-Fi/display/UI state, isolates RTC GPIO12, and then
  selects physical unlatch or ESP deep sleep.
- The hardware-family getter maps `2=v2`, `3=mini`, `4=v3`, `5=v2rev3`,
  `6=minie`, and `7=v3e`. `FUN_40103a24` always unlatches `v2`/`v2rev3`;
  families `v3`, `minie`, and `v3e` all unlatch only without external power.
  Both supported rev #04 and rev #05 therefore physically unlatch on battery.
- `FUN_401036e8` disables amps, puts the accelerometer into low-power mode, and
  changes unused IO-expander pins to inputs while preserving required power,
  charger, display, and interrupt outputs.
- `FUN_40103cf8` arms a 64-bit GPIO mask with mode 0, matching EXT1 `ALL_LOW`.
  Both embedded board configs specify `ioxInt=GPIO.34`; the physical power
  button is `IOX.1.3`. The unsupported case is an IOX descriptor for `ioxInt`
  itself, not the supported expander interrupt wired to a direct ESP GPIO.
- Deep-sleep reset enters a wake-qualification loop in `FUN_40104808`. It polls
  in 250 ms steps, normally requires a two-second held power button, accepts
  RTC `AR_USER`/`AR_TEST` alarms, and sends unqualified wakeups back to sleep.
- Battery monitoring supports ADC, CW2015 (`0x62`), and CW2215B (`0x64`).
  Charger enable is SGM41513 register 1 bit 4 **active-high**; the separate Qi
  and USB path-enable GPIOs are active-low.
- Stock warning/start/OTA defaults are 7%/4%/15%. Charge-temperature limits
  form nested inclusive windows: 0–44 °C for charging and −19–59 °C for
  battery operation. The Ghidra evidence does not show separate thermal
  hysteresis thresholds.

The binary also contains battery-full charge capping, USB-versus-Qi source
selection by advertised power (USB wins ties), a 450 mA charge-ramp floor, Qi
NTC capping with raw thresholds `0x590`/`0x5aa`, and smart-cable USB-C
attach/detach handling.

## Implementation references

- [SG Micro SGM41513/SGM41513A/SGM41513D datasheet](https://www.sg-micro.com/rect/assets/58a4fe4d-da1d-49a3-b312-5664211d9016/SGM41513_SGM41513A_SGM41513D.pdf) —
  register masks, watchdog reset behavior, DPM, and latched BATFET protection.
- [ESP-IDF 5.5 GPIO/RTC GPIO reference](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/peripherals/gpio.html#_CPPv416rtc_gpio_isolate10gpio_num_t) —
  Espressif's explicit `rtc_gpio_isolate(GPIO_NUM_12)` guidance for
  ESP32-WROVER deep-sleep current.
