---
icon: lucide/monitor
---

# Display

The front face is a **16×16 pixel** display used to render card icons, status,
and animations. Two controller families appear across revisions:

| Config | Controller | Technology | Interface |
|--------|-----------|------------|-----------|
| #01/#02/#05 | **HT16D35x** (Holtek) | LED matrix (constant-current driver) | SPI, 4× chip-select via IOX |
| #00/#04 | **GC9306** | TFT LCD | SPI (`pwm`/`cs`/`dc`/`reset`) |

Only `ht16d35x` and `gc9306` appear as display-controller strings in the image;
no GC9A01/ST7789/ILI9341/SSD1306 references exist.

## HT16D35x (LED matrix)

Four chip-select lines (`csn0..csn3`) drive the LED matrix through the IO
expander. The `csn` mapping differs by revision:

```text
"display": { "type": "ht16d35x", "csn0": "IOX.2.0", "csn1": "IOX.2.1",
             "csn2": "IOX.2.2", "csn3": "IOX.2.3" }   # 2-IOX boards (#02/#05)

"display": { "type": "ht16d35x", "csn3": "IOX.0.0", "csn2": "IOX.0.1",
             "csn1": "IOX.0.2", "csn0": "IOX.0.3" }   # single-IOX board (#01)
```

The log tag is `DISP_HLTK` (the HAL symbol is spelled `ht1d35x_hal` in the
image — a firmware typo for `ht16d35x_hal`).

## GC9306 (TFT)

```text
"display": {
  "type": "gc9306",
  "pwm": "GPIO.26",     # backlight PWM
  "cs": "IOX.0.0",
  "dc": "IOX.0.1",
  "reset": "IOX.0.2"
}
```

The log tag is `@DISP_GC9306` (the `@` is part of the extracted string; the
overlay tag is likewise `@UI_OVERLAY`).

### Stock-matched GC9306 implementation (`firmware/components/gc9306/`)

The replacement follows the recovered stock command/transaction protocol and
applies only device-measured output corrections:

- **Electrical configuration** — SPI2 host 1, mode 0, 80 MHz,
  `SPI_DEVICE_NO_DUMMY`, no hardware CS, queue depth 1; MOSI=GPIO22,
  SCLK=GPIO23, CS/DC/reset=IOX.0.0/.1/.2.
- **Controller state** — stock reset (high 50ms, low 50ms, high 120ms),
  the 21 vendor groups through F2, then separate DC-low CS groups
  `35 00`, `44 00 0A`, `21`, `11`, 120ms, and `29`. The explicit
  later resume sequence is `11`, 120ms, then `53 00 29` under one CS group.
- **Pixels** — stock queues one 1364-pixel transaction at a time and waits
  before buffer reuse. CASET/RASET/RAMWR use DC-low commands and DC-high
  payloads. On this device, a known red RGB source rendered blue with the
  recovered `MADCTL` mode, so `gc9306_store_rgb()` swaps R/B at the RGB666
  output boundary; it does not alter source assets, alpha, or any other
  channel. This is an empirical panel correction, not a claim about the
  factory binary's scaler.
- **RGBA asset/layout** — the stock low-SOC battery_ui table selects ID 10
  (powered, not charging, SOC≤10), resolving through the hardware icon table
  to PNG `0x3F468D61`. `analysis/extract_stock_battery_icon.py` extracts its
  exact 16×16 RGBA payload and checks its SHA-256. The stock compositor uses
  `floor(rgb * alpha / 255)`, then nearest-neighbour scale 12 into inclusive
  GRAM window `(24,27)..(215,218)`; the replacement applies its measured
  40px vertical physical-panel correction to the centered 192px test frame.
- **Remote color images** — current OYIM v1 files carry a 16×16
  little-endian RGB565 frame after an 8-byte header. The GC9306 expands it 12×
  through the same color path as stock-sized icons; HT16D35x converts each
  color directly to one luminance pixel. Older 64×64 OYIM files remain
  readable through the row-streamed 3× compatibility path.
- **Backlight/IOX** — stock drives IOX.0.3 low in `p0Data=0x30`; the physical
  panel requires that state. Backlight is GPIO26 LEDC PWM at stock 40 kHz.

### Full-raster geometry and orientation

The GC9306 controller accepts a 240×320 GRAM window, but this unit's verified
visible content region is the upper 240×240 area. The stock 192×192 icon window
`(24,27)..(215,218)` is the safe reference. Critical UI placed below roughly
Y=218 can be partially clipped even though the controller accepts the write.

The recovered `MADCTL=0x48` mode also presents full-raster RAMWR data mirrored
horizontally on the physical panel. For native masks, convert visual X to GRAM
X before packing:

```c
gram_x = 239 - visual_x;
```

This mirror correction is separate from the measured R/B byte swap at the
RGB666 output boundary. Source assets remain in ordinary orientation and color;
both RGB565 sizes reverse source X only while generating the RAMWR stream, and
swap R/B only at the final RGB666 output boundary.

The six-character admin code demonstrates the safe layout: native 5×7 glyphs
at 9× scale, three per row, centered horizontally, with rows at Y=40 and
Y=145. Their last set pixels are at Y=207, above the clipping boundary.


### Factory-art audit

`assets/` contains 115 CRC-checked PNG streams extracted byte-for-byte from
`output/factory.bin`, with flash offsets and SHA-256 hashes in
`assets/manifest.json`. Every embedded PNG is 16×16: 67 RGBA, 30 RGB, and 18
grayscale+alpha. The ID-10 asset is a segmented semi-transparent white low-SOC
visual, not a traditional battery-outline PNG, so do not describe it as one.
`factory-069-0x06709D-16x16.png` is the stock red low-battery slash, and
`firmware/icons/battery-empty.png` is that exact byte stream (SHA-256
`b1595cdd…c283e2d`), so it is the one factory asset the replacement firmware
still compiles in — as the depleted-battery icon. Every other battery state
comes from the authored `firmware/icons/battery-*.png` set.

## Rendering model

The display renders **icons** (16×16 sprites) with optional **overlays**, plus
status elements:

```text
D (%lu) %s: Rendering icon %d. Overlay:%llu
D (%lu) %s: Rendering icon %s. Overlay:%llu
SET_OVERLAY_MODE
SET_OVERLAY_PERSISTS_MODE
CLEAR_OVERLAY
```

Icon sources live under `/sdcard/icons` and `/system/icons` (built-in faces
such as `leftwink`, `rightwink`). Icons are cached and tracked by `IconId`.
Brightness is controllable (`DISPLAY_REFRESH_BRIGHTNESS`) and the display
supports animations (`DISPLAY_ANIMATION`, `icon_anm`).

## Night light

A separate **AW2028H** LED driver provides the RGB "night light" (three
channels, `red`/`green`/`blue`). On config #01 the channels are direct GPIO
(`red`=GPIO12, `green`=GPIO23, `blue`=GPIO19); configs #00/#04 have
`nightlight: null`. Configs #02/#05 use the AW2028H (register access — the
image has `AW2028 Version 0x%02X`). This is distinct from the 16×16 display
and is used for the ambient/night-light feature (`APP_AMBIENT_RGB`,
`APP_NAMBIENTRGB`).

!!! note
    The AW2028H's bus is not stated in the config JSON; "I2C" is an inference
    from the register-access strings, not a literal config field.

## Factory-test diagnostics

A `DISPLAY` command (plus `/display-bar`, `/display-calculate-md5`,
`/display-get-md5`, `/icon-preview`) exposes subcommands for production
verification:

- draw horizontal/vertical lines in a given RGB colour
- draw a single pixel (`PIXEL FAIL: invalid co-ordinates`)
- set/clear overlay and overlay persist mode
- set/get brightness
- calculate/retrieve an MD5 of the last rendered frame

```text
DISPLAY FAIL: --horzlines and --vertlines cannot be used together
DISPLAY FAIL: --line cannot be used with --horzlines/--vertlines
DISPLAY FAIL: invalid RGB value
DISPLAY FAIL: invalid line
```
