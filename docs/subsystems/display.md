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

### Bring-up implementation (`firmware/components/gc9306/`, verified on hardware)

The driver replicates the stock factory image's GC9306 driver exactly
(function roles recovered with the Espressif objdump — see
[ai/decompile.md](../ai/decompile.md)):

- **Register init** — 21 command groups, 64 bytes, opening with the
  GalaxyCore inter-register enable pair `0xFE 0xEF`, then vendor registers;
  `COLMOD 0x06` = 18-bit RGB666. Init parameters are clocked at **DC low**
  (inter-register mode), CS asserted per group — matching the stock driver's
  byte stream (init function at `0x40108946`).
- **Drawing** — CASET/RASET/RAMWR (`0x2A/0x2B/0x2C`) with parameter/pixel
  data at **DC high**, CS held across the whole rect (stock `draw_rect` at
  `0x4010879c`); 3 bytes/pixel RGB666, chunked (stock pixel writer
  `0x401084d0`).
- **Backlight** — GPIO26 via **LEDC PWM at 40 kHz** (stock frequency literal
  `0x9C40` @ `0x400d4fd4`; brightness `duty = pct << 7` per the stock setter
  `0x401090c8`). A plain GPIO high does **not** light it — the panel LED
  rail is AC-coupled — and a lower PWM frequency is attenuated by the
  coupling cap, leaving the display so dim that colours wash out (green
  reads cyan, blue reads purple at 5 kHz; correct at 40 kHz full duty).
- **Level convertor** — the IOX default `p0Data=0x30` drives IOX.0.3
  (`levelconvertor`) **low**; the enable is **active-low**. Driving it high
  (the #05-style default) silently blocks the display SPI/backlight domain.
- **Colour transform (resolved)** — the GC9306 on this board renders the
  18-bit stream as `displayed = (XNOR(R,G), G, G XOR B)` per 6-bit channel.
  Recovered empirically: two 4-stripe tests (red/green/blue/white →
  black/cyan/magenta/yellow, then the transformed sends →
  blue/white/red/green) gave 8 data points that all fit the model. The
  driver sends the inverse `(XNOR(Rd,Gd), Gd, Gd XOR Bd)`; a
  red/green/blue/white stripe pattern verifies all channels.

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
