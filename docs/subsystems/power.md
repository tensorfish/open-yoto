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
| Charger | **SGM41513** (#00/#04/#05), **SGM41511** (#02), **ETA6003** (#01) | I2C | `chgstat`: IOX1.4 (2-IOX) / IOX1.7 (single-IOX); `plugstat`/`chgbst` on some |
| USB-C PD sink | **HUSB238** | I2C | VBUS status IOX1.0, charge-en IOX2.7 |
| Qi RX | **CV8013N** (#05), **CV8085** (#02) | I2C | status IOX0.7, charge-en IOX2.6; 5 W-en IOX3.5 only on CV8013N |

The earliest config (#01) reads battery voltage via ADC (`ADC.1.3`) with no
fuel gauge. Later units use `CW2015`/`CW2215B`, which report voltage and
capacity percentage (`_cw2215b_get_capacity_percentage`,
`_cw2215b_get_voltage`).

## Power-control path

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
`AS-R18650-2600-112`, `LJDX30X-4500`, `UTL-FD70X-2000`, and one more
(see `output/hwconfig_*.json`). NTC temperature monitoring is present
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

## Sleep / power management

ESP-IDF power management is enabled with light-sleep (`PS_LIGHTSLEEP`,
`POWER_MAN_MODE`, `BATT_THRS_OTA`). Sleep timers fade volume and can wake on
timer or button. `BATT_PROFILE_ID`/`APP_BATFULL_PCT` tune charge capping and
battery-full thresholds.

A "smart cable" debouncer handles USB-C attach/detach
(`components/battery/smartcable_debouncer.cpp`), and PD negotiation is logged:

```text
D (%lu) %s: %s: detected PD charger, voltage %d current %d
D (%lu) %s: USB disconnected so resetting state variables
```
