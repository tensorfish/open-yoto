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

## Replacement firmware shutdown

A three-second power-button hold and the 30-minute inactivity deadline share
one shutdown path:

1. Publish shutdown, stop the admin server without holding the player mutex,
   stop audio, and blank the display.
2. Wait for a held power button to be released.
3. Disable the amplifier and downstream rails; on rev #05, unmask only the
   `IOX.1.3` interrupt and clear stale IOX transitions.
4. Arm GPIO34 for the active-low IOX interrupt where supported.
5. Retain `VIN_HOLD` and use 100 ms timer-backed light sleep. Rev #04's ET6416
   does not accept the PI4IOE5V6416 interrupt-mask registers, so the timer is
   its primary wake path. A one-second button hold restores the rails and
   restarts the ESP32.

Normal manual/inactivity shutdown deliberately retains the latch: rev #04
cannot observe its IOX-only power button after a physical power cut. Playback
and an active admin session defer inactivity shutdown; encoder events and NFC
card insertion/removal restart the full 30-minute interval.

Battery safety is separate from display and idle policy. Two consecutive fresh
five-second samples at or below 3% SOC force shutdown only when a successful
contemporaneous SGM41513 read reports `PG_STAT=0`. Invalid I²C samples,
noncritical SOC, and external power clear the confirmation streak, so read
sentinels or charge-complete state cannot masquerade as a critical battery.

After critical battery is confirmed, the safety path isolates GPIO12, releases
`VIN_HOLD`, and enters deep sleep as a fallback. This hard cutoff is
intentionally non-wakeable until external power is attached.

## Original firmware reference

The stock image also contains ESP-IDF light-sleep settings (`PS_LIGHTSLEEP`,
`POWER_MAN_MODE`, `BATT_THRS_OTA`), sleep timers with volume fades, battery-full
charge capping, and a smart-cable USB-C attach/detach state machine:

```text
D (%lu) %s: %s: detected PD charger, voltage %d current %d
D (%lu) %s: USB disconnected so resetting state variables
```

## Implementation references

- [SG Micro SGM41513/SGM41513A/SGM41513D datasheet](https://www.sg-micro.com/rect/assets/58a4fe4d-da1d-49a3-b312-5664211d9016/SGM41513_SGM41513A_SGM41513D.pdf) —
  register masks, watchdog reset behavior, DPM, and latched BATFET protection.
- [ESP-IDF 5.5 GPIO/RTC GPIO reference](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/peripherals/gpio.html#_CPPv416rtc_gpio_isolate10gpio_num_t) —
  Espressif's explicit `rtc_gpio_isolate(GPIO_NUM_12)` guidance for
  ESP32-WROVER deep-sleep current.
