# Display & Nightlight (HT16D35x LED matrix, GC9306 TFT, AW2028H nightlight)

Dense reference for AI agents reverse-engineering the Yoto ESP32 firmware.

Workspace root: `/Users/k/Development/tensorfish/open-yoto`. All paths below are
relative to it. Ground-truth sources: `output/strings.txt` (25,732 unique
strings, line-numbered), `output/hwconfig_0*.json`, `output/pinmap.json`,
`output/decompiled_manifest.json`, `output/ghidra_functions.json`,
`output/decompiled/*.c`.

Firmware is ESP32 (Xtensa LX6), ESP-IDF + ESP-ADF. Factory image
`output/factory.bin` (2.45 MB), entry `0x400813a8`.

---

## 1. What this subsystem is

Two interchangeable 16×16 pixel displays (selected by hwconfig `display.type`),
a shared UI/image pipeline, and an optional nightlight (ambient RGB LED):

- **HT16D35x** — Holtek 16×16 LED matrix driver, SPI, 4 chip-selects
  (`csn0..csn3`). Driver tag `DISP_HLTK` / `ht1d35x_hal`.
- **GC9306** — TFT LCD controller, SPI + LEDC PWM backlight. Driver tag
  `@DISP_GC9306`; backlight tag `GC9306_BACKLIGHT`.
- **AW2028H** — I2C 3-channel RGB LED driver used as the nightlight/ambient
  LED. Alternative "MCU" nightlight type = 3 GPIO PWM (LEDC) channels.
- UI is always **16×16 RGBA32** (`16*16*4 = 1024 = 0x400` bytes). The GC9306
  TFT upscales this 16×16 image (`gc9306_upscale_draw_pixels`).

Both display drivers are fed by the same high-level display module (tag
`DISP`) and PNG/font/icon pipeline. `output/decompiled/*.c` files are named
`FUN_segN__ADDR_0xADDR.c`.

---

## 2. Component / pin table (enumerate ALL variants — revisions differ)

Display + nightlight pins per `output/hwconfig_*.json`. IOX = I/O expander
(PI4IOE5V6416 / ET6416); `IOX.<bank>.<bit>`.

| hwconfig | display.type | display pins | nightlight | SPI (display data) | I2C (AW2028H) |
|---|---|---|---|---|---|
| `00` (gc9306, sd1, CW2015) | `gc9306` | `pwm` GPIO.26, `cs` IOX.0.0, `dc` IOX.0.1, `reset` IOX.0.2 | `null` | mosi GPIO.22, sclk GPIO.23, miso null | — |
| `01` (ht16d35x, es8388 combined, sd SPI, ADC) | `ht16d35x` | `csn0` IOX.0.3, `csn1` IOX.0.2, `csn2` IOX.0.1, `csn3` IOX.0.0 | `MCU`: red GPIO.12, green GPIO.23, blue GPIO.19 | mosi GPIO.18, miso GPIO.21, sclk GPIO.2 | sda GPIO.26, sck GPIO.27 |
| `02` (ht16d35x, sd1, CW2015) | `ht16d35x` | `csn0` IOX.2.0, `csn1` IOX.2.1, `csn2` IOX.2.2, `csn3` IOX.2.3 | `AW2028H` | mosi GPIO.22, miso GPIO.26, sclk GPIO.23 | sda GPIO.21, sck GPIO.25 |
| `03` (placeholder) | *(none)* | *(none)* | *(none)* | — | — |
| `04` (gc9306, sd4, CW2215B) | `gc9306` | `pwm` GPIO.26, `cs` IOX.0.0, `dc` IOX.0.1, `reset` IOX.0.2 | `null` | mosi GPIO.22, sclk GPIO.23, miso null | — |
| `05` (ht16d35x, sd1, CW2215B) | `ht16d35x` | `csn0` IOX.2.0, `csn1` IOX.2.1, `csn2` IOX.2.2, `csn3` IOX.2.3 | `AW2028H` | mosi GPIO.22, miso GPIO.26, sclk GPIO.23 | sda GPIO.21, sck GPIO.25 |

Exact JSON evidence (all under `output/hwconfig_*.json`):

- `hwconfig_00…/04…`: `"display": {"type":"gc9306","pwm":"GPIO.26","cs":"IOX.0.0","dc":"IOX.0.1","reset":"IOX.0.2"}` (00: lines 80-86; 04: lines 83-89).
- `hwconfig_01…`: `"display": {"type":"ht16d35x","csn0":"IOX.0.3","csn1":"IOX.0.2","csn2":"IOX.0.1","csn3":"IOX.0.0"}` (lines 92-98); `"nightlight": {"type":"MCU","red":"GPIO.12","green":"GPIO.23","blue":"GPIO.19"}` (lines 32-37).
- `hwconfig_02…`/`05…`: `"display": {"type":"ht16d35x","csn0":"IOX.2.0","csn1":"IOX.2.1","csn2":"IOX.2.2","csn3":"IOX.2.3"}` (02: lines 106-112; 05: lines 117-123); `"nightlight": {"type":"AW2028H"}` (02: lines 46-48; 05: lines 46-48).
- `hwconfig_03…` has **no** display/nightlight section.

`output/pinmap.json` flattens these (display/nightlight keys only):

- `display.pwm` = GPIO.26 (00, 04) — lines 79, 459
- `display.cs/dc/reset` = IOX.0.0/0.1/0.2 (00, 04) — lines 82/85/88, 462/465/468
- `display.csn0..3` = IOX.0.3/0.2/0.1/0.0 (01) — lines 204/207/210/213
- `display.csn0..3` = IOX.2.0/2.1/2.2/2.3 (02, 05) — lines 329/332/335/338, 597/600/603/606
- `nightlight.red/green/blue` = GPIO.12/23/19 (01) — lines 144/147/150

Note: `output/hwconfig_merged.json` is a flattened union, NOT a real board —
its display section (lines 101-107) mixes gc9306 + ht16d35x keys. Do not use it
for a single board's pins.

---

## 3. Exact strings (ground truth, `output/strings.txt` line numbers)

### Display command enum (queue messages sent to `display_task`)
Lines 2854-2876:
```
2854:UPDATE                2855:CLEAR_BUFFERS         2856:CLEAR_OVERLAY
2857:SET_IMAGE_ICON        2858:SET_IMAGE_FILE         2859:SET_IMAGE_BUFF
2860:SET_OVERLAY_MODE      2861:SET_OVERLAY_PERSISTS_MODE
2862:SET_OVERLAY_ACTIVE    2863:SET_OVERLAY_PIXEL      2864:SHOW_OVERLAY_PIXEL
2865:SHOW_ALL_OVERLAY_PIXELS
2866:SET_FONT_COLOUR       2867:SET_DISPLAY_TEXT       2868:SET_BORDER_DARKEN_RATIO
2869:DISPLAY_ANIMATION    2870:PUSH_BACKGROUND        2871:POP_BACKGROUND
2872:DISPLAY_REFRESH_BRIGHTNESS
2873:RESET_ANIMATION       2874:DRAW_VOLUME_BAR        2875:DRAW_PROGRESS_BAR
2876:SET_GIF_FILE
```

### Display module log strings (tag `DISP`)
- `2850` `E ... set_border_darken_ratio requires non-zero value`
- `2851` `E ... font_get_text_width error: contains new line character`
- `2852` `E ... font_get_text_width error : num pixels too wide %d`
- `2853` `E ... Trying to read ALS on HW without a light sensor! Defaulting to max brightness instead`
- `2882` `display_task` (FreeRTOS task name)
- `2884` `E ... called by task other than display_task; this is not thread-safe!`
- `2886` `D ... Rendering icon %d. Overlay:%llu`
- `2888` `D ... Rendering icon %s. Overlay:%llu`
- `2930` `DISP` (log tag)
- `2934` `E ... RGBA image size != DISPLAY_UI_RGBA32_SIZE`
- `2943` `E ... Cannot set brightness, display not initialised`
- `2944-2946` `... overlay mode / persist ... not initialised`
- `2956` `E ... Cannot get brightness, display not initialised`
- `2958-2959` `... MD5 calculation / last image MD5 ... not initialised`

### PNG / image pipeline (libspng; version `1.2.11` at line 3042)
- `2907` `D ... Read %d RGBA bytes from png`
- `2908` `E ... Only 16x16 RGBA 32-bit PNGs are supported. got %d bytes`
- `2913` `I ... acTL found at: %d` (animated PNG)
- `2915` `I ... Static PNG detected (size %d, name %s)`
- `2916` `_verify_png_buffer`   `2917` `_image_size_valid`
- `2918` `display_pngseq`       `2919-2920` PNG sequence task started/ended
- `2925` `pngseq_play_frames`
- `3043-3063` libspng error strings (IHDR/PLTE/IHDR/chunk checks, etc.)

### Render helpers (function-name literals, used as `%s`/`__func__` in logs)
- `2898` `play_welcome_animation`   `2899` `blend_overlay_rgba`
- `2900` `display_from_rgba`        `2902` `decode_png`

### GC9306 TFT driver (tag `@DISP_GC9306`; backlight tag `GC9306_BACKLIGHT`)
- `2963` `@DISP_GC9306`
- `2964` `E ... Failed to initialised spi`
- `2965` `E ... Failed to initialise TFT`
- `2966` `E ... Failed to initialised backlight`
- `2985` `gc9306_upscale_draw_pixels`   `2986` `spi_master_write_addr`
- `2987` `gc9306_draw_rect`             `2988` `gc9306_display_on`
- `2989` `gc9306_display_power_reset`   `2990` `gc9306_display_off`
- `2991` `_spi_master_write_command_unsafe`
- `2992` `_spi_master_write_data_byte_unsafe`
- `2993` `_gc9306_reset`                `2994` `gc9306_display_init`
- `2995` `gc9306_spi_init`              `2996` `GC9306_BACKLIGHT`
- `2997-3001` LEDC PWM errors (`ledc_timer_config` / `ledc_channel_config` /
  `_init_pwm_timer`)

### HT16D35x driver (tag `DISP_HLTK` / `ht1d35x_hal`)
- `2960` `DISP_HLTK` (log tag)     `3002` `ht1d35x_hal`
- `3003` `W ... unable to initialise a DMA-capable buffer`
- `3004` `E ... Holtek SPI is already initialised`
- `3005` font glyph set ` !"#$%&&'()**+,-./0123456789:;<=>?@ABDEFGHIKLMNPQRTUVXYZ\]^`acdfgijlmoprsuwxz|}`
- `3006` `SYSTEM_ICON`
- `3007-3010` icon errors (`Index out of range`, `Unexpected hardware platform`,
  `found NULL icon ptr(s)`, `icon not valid for HW family`)
- `3011` `platform_icon_fetch`

### Nightlight / ambient LED (tags `NIGHT_LIGHT`, `LEDS`)
- `3340` `NIGHT_LIGHT` (log tag)
- `3341` `I ... New (PERC:%d) Red: %d, Green: %d, Blue: %d`
- `3342` `0x%02x%02x%02x` (RGB color format)
- `3428` `nightlight`   `3429` `I ... Nightlight Settings`   `3430` `AW2028H`
- `1902` `LEDS` (ambient driver log tag)
- `1903` `I ... AW2028 Version 0x%02X`
- `1904` `I ... using SYNC RGB mode, setting PWM1 to max (0xFF) and ILED1/2/3 to 0x00`
- `1907` `I ... using SYNC RGB mode, setting PWM1 to min (0x00) and using LCFG1/2/3 to fade down`
- `1909` `I ... using ASYNC RGB mode, setting ILED1/2/3 max (0xFF) and using PWM1/2/3 for colour value`
- `1908` `ramp_down_ambients`   `1910` `ambient_leds_set_nocap`
- `1911` `ambient_get_hw_params`   `1912` `init_ledc_timer`

### HW-config parse keys (display + nightlight + brightness settings)
- `3483` `I ... Display Settings`    `3484` `ht16d35x`   `3485` `gc9306`
- `3486` `csn0`   `3487` `csn1`   `3488` `csn2`   `3489` `csn3`
- `3490` `E ... Unknown Display Type`
- `1270` `icon16x16`   `1272` `overlayLabel`
- `1810` `nightLightMode`   `2599` `nightlightMode`
- `2593` `dnowBrightness`   `2594` `dayBright`   `2595` `nightBright`
- `1385` `_proc_brightness`

---

## 4. Function addresses (decompiled; mark inference)

All are Ghidra auto-named `FUN_segN__ADDR`. String→function mapping was
recovered manually by locating each string's DROM literal-pool entry in the
IROM segment and grepping `output/decompiled/*.c` for the `DAT_segN__ADDR`
reference (Ghidra does **not** resolve L32R literal pools on generic Xtensa,
so `string_xrefs.json` is effectively empty — one entry only). Roles marked
`[INFERENCE]` are inferred from log strings + call structure, not from symbol
names.

### High-level display module (tag `DISP` @ `0x3f40b398`)
| Address | File | Likely role |
|---|---|---|
| `0x40104d20` | `FUN_seg4__40104d20…` | `set_border_darken_ratio` (logs line 2850) |
| `0x4010517c` | `FUN_seg4__4010517c…` | `font_get_text_width` (logs 2851/2852) `[INFERENCE]` |
| `0x40105334` | `FUN_seg4__40105334…` | reset display-buffer/`display_from_rgba` flags |
| `0x40105508` | `FUN_seg4__40105508…` | `display_from_rgba` (log `display_from_rgba`) `[INFERENCE]` |
| `0x40105608` | `FUN_seg4__40105608…` | display module init (spawns `display_task`) |
| `0x40105770` | `FUN_seg4__40105770…` | creates FreeRTOS task named `display_task` (xTaskCreate) |
| `0x401058fc` | `FUN_seg4__401058fc…` | `blend_overlay_rgba` (log) |
| `0x40105a28` | `FUN_seg4__40105a28…` | clear display buffer (memset 0x400) |
| `0x40105a50` | `FUN_seg4__40105a50…` | display command dispatch loop (large switch) `[INFERENCE]` |
| `0x401056dc` | `FUN_seg4__401056dc…` | render/push frame `[INFERENCE]` |

### PNG / image pipeline (`decode_png` @ `0x3f41c19a`, `display_pngseq` @ `0x3f41c42f`)
| Address | Likely role |
|---|---|
| `0x40106834` | `decode_png` entry (16×16 RGBA PNG) |
| `0x40106898` | `_verify_png_buffer` / decode helper (log 2903) |
| `0x401069e8` | PNG size validation — logs "Only 16x16 RGBA 32-bit PNGs" (2908) |
| `0x40106b18` | PNG decode / render |
| `0x40106dc8` | `display_pngseq` — add element (log 2921) |
| `0x40106e40` | `display_pngseq` — play frames (log 2924) |
| `0x40106efc` | `pngseq_play_frames` task loop (log 2925) |
| `0x40106f3c` | PNG sequence init |
| `0x40107144` | image-size validation — logs "RGBA image size != DISPLAY_UI_RGBA32_SIZE" (2934) |
| `0x401071f4` | same validation path `[INFERENCE]` |

### HT16D35x driver (tags `DISP_HLTK` @ `0x3f41cd74`, `ht1d35x_hal` @ `0x3f41d341`, `SYSTEM_ICON` @ `0x3f41d4bc`)
| Address | Likely role |
|---|---|
| `0x40107a20` | HT16D35x init (logs "Holtek SPI"/DISP_HLTK errors) |
| `0x40107aac` | HT16D35x brightness/attribute helper (scales `>>2`) `[INFERENCE]` |
| `0x40107de8` | HT16D35x power/reset + draw (calls GC9306-path helpers for shared render) `[INFERENCE]` |
| `0x4010965c` | HT16D35x SPI init — logs "Holtek SPI is already initialised" (3004) |
| `0x401096f0` | HT16D35x icon render / `platform_icon_fetch` (tag `SYSTEM_ICON`) |
| `0x401090c8` | brightness set: clamps 0-100, `duty = pct * 0x80`, writes PWM `[INFERENCE]` |
| `0x40109104` | backlight PWM init + set-brightness wrapper (logs `ht1d35x_hal`) |

### GC9306 TFT driver (tag `@DISP_GC9306` @ `0x3f41ce0b`)
| Address | Likely role |
|---|---|
| `0x40108380` | `gc9306_spi_init` — logs "Failed to initialised spi" (2964) |
| `0x401083ec` | `gc9306_display_on` (sends 0x29) |
| `0x40108454` | GC9306 command helper |
| `0x4010854c` | `gc9306_display_init` — main init; allocates 2× frame buffers via `calloc(0x2d0,0x808)`; references `display_off/draw_rect/display_init/display_on/power_reset` |
| `0x40108674` | GC9306 command helper |
| `0x401086e0` | `gc9306_display_power_reset` / `_gc9306_reset` (references `_gc9306_reset` + `gc9306_upscale_draw_pixels`) |
| `0x40108718` | GC9306 command helper (display-off path) |
| `0x4010879c` | `gc9306_draw_rect` |
| `0x401088e8` | `gc9306_upscale_draw_pixels` (large; upscales 16×16 → TFT) |

### Nightlight / ambient LED (tags `NIGHT_LIGHT` @ `0x3f420668`, `LEDS` @ `0x3f411e14`)
| Address | Likely role |
|---|---|
| `0x400f5f44` | ambient LED **init** — reads HW params (type 1=PWM "MCU", type 2=AW2028H); AW2028H path logs "AW2028 Version" + "SYNC RGB mode PWM1 max", writes registers 0x00=0x55 etc. |
| `0x400f610c` | ambient LED set (arg validation; logs "invalid parameter(s)") |
| `0x400f6160` | `ambient_leds_set` (color → LEDC PWM or AW2028H I2C) — called by nightlight |
| `0x400f6614` | ambient color setter `[INFERENCE]` |
| `0x400f65bc` | `ramp_down_ambients` (fade down; calls 0x400f6160 then schedules delayed write) |
| `0x401124c4` | **nightlight** color set — logs "New (PERC:%d) Red:%d Green:%d Blue:%d" (tag `NIGHT_LIGHT`), then calls `ambient_leds_set` (0x400f6160) |
| `0x401137e8` | nightlight/ambient **config init** — reads hwconfig `nightlight` type (`AW2028H` string @ `0x3f41...`, referenced via `DAT_seg4__4010ec74`); large (4.4 KB) `[INFERENCE]` |

Ambient driver notes:
- HW type discriminator stored at `*DAT_seg4__400d3114`: `1` = "MCU" (3× LEDC PWM GPIO), `2` = AW2028H (I2C).
- AW2028H I2C register writes go through `FUN_seg4__4010c948(0x65, <reg>, <val>)`.
  Registers written at init: `0x00=0x55` (GCR), then `0x01, 0x03, 0x04=0,
  0x05=0, 0x06=0, 0x07, 0x08, 0x1c=0xff, 0x10=0, 0x11=0, 0x12=0, 0x01`.
  `0x65` = I2C device selector/address for the AW2028H `[INFERENCE]` (the
  hwconfig JSON carries no nightlight I2C address — it is hardcoded in the
  driver). Registers match AW2028H layout: GCR, LCTR/ILEDx current, PWMx duty,
  LCFGx fade.

---

## 5. Protocol / flow detail

### Display selection & init
1. Boot reads hwconfig `display.type` (`"gc9306"` | `"ht16d35x"`; strings 3484/3485).
2. `"Unknown Display Type"` (3490) logged if neither.
3. GC9306 path: `gc9306_spi_init` (SPI over `spi.mosi/sclk` bus) →
   `_gc9306_reset` → `gc9306_display_init` (allocates 2 buffers, writes init
   sequence) → `_init_pwm_timer`/`ledc_*` on `display.pwm` (GPIO.26) for
   backlight.
4. HT16D35x path: Holtek SPI init over `spi.mosi/miso/sclk`, selects one of
   `csn0..3` via IOX, DMA-capable row buffer (string 3003).

### 16×16 rendering
- Canonical framebuffer is 16×16×RGBA32 = 1024 bytes (`DISPLAY_UI_RGBA32_SIZE`,
  `0x400`; seen as `memset(...,0x400)` and `=0x400` in 0x40105a28/0x401058fc).
- Icons are `icon16x16` 16×16 RGBA PNGs (strings 1270, 2908). Animated content
  uses APNG (`acTL`, 2913) or GIF (`SET_GIF_FILE`, 2876) / PNG sequences
  (`display_pngseq`, `pngseq_play_frames`).
- Overlay compositing via `blend_overlay_rgba` + `display_from_rgba`; overlay
  modes `SET_OVERLAY_MODE`, `SET_OVERLAY_ACTIVE`, `SET_OVERLAY_PIXEL`, etc.
- HT16D35x renders 16×16 natively; GC9306 upscales via
  `gc9306_upscale_draw_pixels`.

### Brightness
- Display brightness: `DISPLAY_REFRESH_BRIGHTNESS` command (2872); setter
  `0x401090c8` clamps 0-100 and maps to duty `pct * 0x80` (0-128).
- ALS (ambient light sensor) optional; missing sensor → max brightness (2853).
- NVS/UI keys: `dnowBrightness`, `dayBright`, `nightBright`, `nightLightMode`.

### Nightlight / ambient
- `nightlight.type`: `"MCU"` (3 GPIO PWM via LEDC: red/green/blue) or
  `"AW2028H"` (I2C). `ambient_get_hw_params` (1911) reports which.
- AW2028H: SYNC RGB mode (PWM1 = master duty, ILED1/2/3 = current) or ASYNC RGB
  mode (ILED = max current, PWM1/2/3 = per-channel duty); fade via LCFG1/2/3.
  `ramp_down_ambients` fades to off.
- Nightlight color set logs `New (PERC:%d) Red:%d Green:%d Blue:%d` (3341).

---

## 6. Raw file pointers

- Strings: `output/strings.txt` (lines cited inline above)
- HW configs: `output/hwconfig_00…05_*.json`, `output/hwconfig_merged.json`
- Pin map: `output/pinmap.json`
- Function lists: `output/decompiled_manifest.json` (5915 entries, decimal
  addr), `output/ghidra_functions.json` (10k entries)
- Decompiled C: `output/decompiled/FUN_segN__ADDR_0xADDR.c`
- String xrefs (sparse/unreliable here): `output/string_xrefs.json`
- Extraction/decompile scripts: `analysis/extract_strings.py`,
  `analysis/extract_hwconfig.py`, `analysis/ghidra_dump.py`,
  `analysis/ghidra_decompile.py`

Key string→function resolution trick (for future work): find string VA in
DROM (segment `0x3F400000` @ file offset 32), then find the 4-byte
little-endian VA in the IROM segment (`0x400D0000` @ file offset 720928) to get
the literal-pool `DAT_segN__ADDR`, then grep `output/decompiled/*.c` for that
`DAT_` label.
