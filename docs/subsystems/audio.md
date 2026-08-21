---
icon: lucide/audio-lines
---

# Audio

Audio is built on **ESP-ADF** (Espressif Audio Development Framework). The
source paths embedded in the image confirm the full ADF pipeline:

```text
/opt/esp/adf/components/audio_hal/audio_hal.c
/opt/esp/adf/components/audio_pipeline/audio_element.c
/opt/esp/adf/components/audio_pipeline/audio_pipeline.c
/opt/esp/adf/components/audio_stream/http_stream.c
/opt/esp/adf/components/audio_stream/i2s_stream.c
/opt/esp/adf/components/audio_stream/lib/hls/hls_playlist.c
/builds/adf/esp-adf-libs-source/esp_codec/esp-mp3/src/pvmp3_framedecoder.cpp
/builds/adf/esp-adf-libs-source/esp_processing/esp-resample/src_fa/src/fa_resample.c
```

## Pipeline

```
NFC URL / SD file / HTTP stream
        │
        ▼
[esp-adf audio_pipeline] ── decode ──> resample ──> EQ
        │
        ▼
[i2s_stream] ── I2S ──> codec ──> speaker amp / headphone DAC
```

Streams include local file, **HTTP**, and **HLS** (`hls_playlist.c`,
`application/vnd.apple.mpegurl`). Playback is a streaming architecture with
ring buffers (`ringbuf.c`) and events (`audio_event_iface.c`).

## I2S pins (revision-dependent)

| Config | `mclk` | `bclk` | `lrclk` | `out` |
|--------|--------|--------|---------|-------|
| #01 (combined ES8388) | GPIO0 | GPIO25 | GPIO32 | GPIO33 |
| #00/#02/#04/#05 (split) | GPIO0 | GPIO5 | GPIO18 | GPIO19 |

## Codec (hardware revision dependent)

| Config | Type | Speaker amp | Headphone DAC | I2C addr (spk L/R, hp) |
|--------|------|-------------|---------------|------------------------|
| #01 | combined | **ES8388** | ES8388 | `0x10` |
| #00/#04 | split | **aw881xx** (×2) | **ES8156** | `0x34`/`0x34`, `0x08` |
| #02/#05 | split | **aw881xx** (×2) | **ES8156** | `0x34`/`0x37`, `0x09` |

The `aw881xx` is a "smart PA" speaker amplifier (the firmware strings
`aw881xx_smartpa_cfg` use that term); the `ES8156` is a dedicated headphone
DAC. The split config allows independent volume/EQ for speaker and headphone
paths. A mono-mix mode exists for the aw881xx
(`Mono-mix (speaker)` / `failed to create mono mix aw881xx driver instance`).

`pactrl` (amplifier power control) is `IOX.0.6` on #00/#01/#04 and `IOX.2.4` on
#02/#05. `hpdetect` (IOX1.1) senses headphone insertion to route audio:

```text
"audio": {
  "hpdetect": "IOX.1.1",
  "audiochip": { "type": "split", "spkrchip": "aw881xx", "i2cspkrl": "0x34",
                 "i2cspkrr": "0x37", "hpchip": "es8156", "i2chp": "0x09",
                 "pactrl": "IOX.2.4" }
}
```

Combined (ES8388) configs validate that both speaker addresses match
(`Combined audio config issue: both I2C spkr chip addresses must be set the same!`).

### Factory-matched speaker bring-up

The clean #04 factory app starts one AW88194A at 7-bit `0x34`: deterministic
`pactrl` LOW/HIGH reset, five chip-ID attempts for `0x1806`, register-table
load, firmware/SmartK DSP configuration, VCALB, I²S/PLL checks, speaker mode,
interrupt setup, and outer hard-unmute. The 35-entry register table starts at
DRAM `0x3ffbee7a`; its final `SYSCTRL=0x6440` and `PWMCTRL=0x300e` writes are
part of the table. Register values use an 8-bit I²C index plus big-endian
16-bit data. The DSP firmware is `0x7f4` bytes at `0x3ffbdf4d`; the rev #04
mono/channel-2 SmartK configuration is `0x39c` bytes at `0x3ffbeade`. Both
payloads are adjacent-byte-swapped and sent in 128-byte chunks through windows
`0x8c00` (firmware) and `0x8600` (configuration).

After SmartPA startup, the factory pipeline sets AW88194 volume register
`0x0f=0x0000` (100%) and settles I²S to APLL 44.1 kHz, 16-bit mono-left with
the ESP32 legacy mono WS polarity. The replacement reproduces those settings,
downmixes stereo MP3 frames before DMA, and validates I²S lock, power/mute,
volume, format, and DSP status. The repeating 1kHz boot tone and physical
speaker output are verified on rev #04 hardware.

## Decoders & stream types

- **Decoders**: MP3 (`pvmp3`), MP4/AAC (`DEC_AAC`), WAV (`DEC_WAV`),
  **OPUS** (`DEC_OPUS` / `OPUS_DECODER`), **OGG** (`DEC_OGG`).
- **MIME types**: `audio/mp4`, `audio/aacp`, `audio/wav`, `audio/opus`,
  `audio/x-scpls` (Shoutcast PLS), plus HLS (`application/vnd.apple.mpegurl`).
- **EQ**: `APP_EQ_PRESET` setting; `esp-resample` for sample-rate conversion.
- **Volume**: per-path (speaker / headphone), with a sleep timer that fades
  volume and restores it (`could not restore user volume after sleep timer
  expired`).
- **Diagnostics**: `/eq-gains`, `/vol-curve`, `/dump-aw881` (DSP register dump).

## BT audio

Bluetooth **A2DP/AVRC** (`AVRC passthrough cmd: key_code 0x%x`) lets the device
act as both an AV remote control target and controller, and play BT-audio
through the same ESP-ADF pipeline (`BT_AV_TAG`, `AUDIO_BOARD`, `AUDIO_COMBO`).

## Hardware init

Audio board init failure is fatal — the device reboots:

```text
E (%lu) %s: failed to initialise audio board HAL! Rebooting
E (%lu) %s: failed to initialise audio board! Rebooting
```

The board config is printed at startup (`param audio_cfg [%02x:…]`,
`param audio_stat [%02x:…]`), reflecting the codec registers.

## System sounds

Built-in sounds live under `/system/sounds/` (e.g. `low_battery`,
`connected_to_power`, `disconnected_from_power_2_of_3`, `battery_fault_beep`).
