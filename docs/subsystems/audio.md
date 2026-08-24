---
icon: lucide/audio-lines
---

# Audio

The **stock firmware** is built on ESP-ADF (Espressif Audio Development
Framework). Source paths embedded in the authoritative factory image confirm
the full ADF pipeline:

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

The factory image establishes the application-level contract directly:

- `0x400e1d8d` builds the MP3 decoder config and calls the decoder initializer
  at `0x40144798`; adjacent branches call AAC `0x40143c5c` and OPUS
  `0x40145748`.
- `0x400e1f0e` stores `0xAC44` (44,100) as the resampler destination rate,
  stores complexity `1`, and calls `rsp_filter_init` at `0x401d2220`.
- Decoder-info handling at `0x400e24ef` passes the decoded sample rate and
  channel count to `rsp_filter_set_src_info` at `0x401d21a4`.
- The sink remains APLL, 16-bit, mono-left I²S. Thus compressed files are
  decoded first; they are never converted to an image or preconverted PCM.

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
propagates decoded source metadata, downmixes stereo, and statefully resamples
8–96 kHz MP3/AAC sources to the fixed 44.1 kHz sink across frame boundaries.
It also validates I²S lock, power/mute, volume, format, and DSP status.

TX descriptors auto-clear after transmission. Stop and natural-end transitions
mute the DAC, disable I²S, preload the complete cyclic DMA ring with zeroes,
and re-enable clocks. An idle transmitter therefore emits silence instead of
repeating its final millisecond-scale PCM fragment, and the next playback
starts from a cleared ring.

## Decoders & stream types

- **Oracle decoders**: Stagefright/pvmp3 (`DEC_MP3`), MP4/ADTS AAC
  (`DEC_AAC`), WAV (`DEC_WAV`), OPUS (`DEC_OPUS`), and OGG.
- **Oracle MIME types** include `audio/mp4`, `audio/aacp`, `audio/wav`,
  `audio/opus`, `audio/x-scpls`, and HLS
  (`application/vnd.apple.mpegurl`).
- **Replacement decoder path**: Espressif's supported ESP32 MP3 and AAC
  decoder component preserves the oracle's decoder family and decoded-PCM
  contract. Format selection inspects `ftyp`, ADTS, ID3, and Layer III frame
  headers instead of trusting the extension.
- **M4A**: streams `stsd`, `stsz`, `stsc`, and `stco`/`co64` sample tables,
  including variable sample sizes and multiple chunks; it does not allocate a
  whole-file copy.
- **AAC**: supports both raw AAC access units in M4A and standalone ADTS `.aac`.
- **Factory EQ/resampler**: `APP_EQ_PRESET`, `esp-resample`, FIR complexity 1.
  The replacement uses a bounded stateful linear resampler at the same pipeline
  position and destination format.
- **Memory placement**: the WROVER-E PSRAM is initialized at boot; `malloc`
  blocks larger than 4 KiB may use external RAM while 32 KiB stays reserved
  for internal/DMA allocations. Decoder instances exist only during playback.
- **Volume**: per-path (speaker / headphone), with a sleep timer that fades
  volume and restores it.
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

## Stock welcome sound

The normal boot plays only the stock welcome sound. It does not read that
sound from SD: `YOTO_VFS` exposes an embedded, read-only asset at
`/system/sounds/welcome`.

| Property | Value |
|---|---|
| Oracle DROM range | `0x3f45014b–0x3f4549fb` |
| Size | 18,608 bytes |
| SHA-256 | `595d30ff7074658b6cda26d0412655e7adfb1e92c69ead2cbc0080eced895e2d` |
| Container | M4A (`ftypM4A`) |
| Codec | AAC-LC, mono, 44.1 kHz, 16-bit PCM output |
| Samples | 167 AAC access units → 171,008 decoded samples |

The replacement embeds those exact bytes once, registers a read-only
`/system` VFS, opens the logical path after the SD mount, parses `stsd`,
`stsz`, and `stco`, decodes each raw AAC access unit, and writes PCM through
the existing volume-controlled I²S path. The bring-up test tone remains an
explicit test-mode facility; it is disabled in the normal boot configuration.
