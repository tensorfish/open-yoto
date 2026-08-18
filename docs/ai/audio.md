---
icon: lucide/audio-lines
---

Dense reference for AI agents reverse-engineering the Yoto ESP32 firmware's
**audio** subsystem (ESP-ADF pipeline + codecs + I2S + Bluetooth A2DP/AVRC).

Workspace root: `/Users/k/Development/tensorfish/open-yoto`. All paths below are
relative to it. Ground-truth sources: `output/strings.txt` (25,732 unique
strings, line-numbered), `output/hwconfig_0*.json` (six hardware configs),
`output/pinmap.json`, `output/decompiled_manifest.json` (5,915 decompiled
functions, decimal VMA `addr`), `output/ghidra_functions.json` (9,185
functions, decimal **file-offset** `addr`), `output/decompiled/*.c`.

Firmware is ESP32 (Xtensa LX6), ESP-IDF + ESP-ADF. Factory image
`output/factory.bin` (2.45 MB), entry `0x400813a8`. DROM (rodata/strings) is
`0x3F400020` (seg0); IROM (flash-mapped code) is `0x400D0020` (seg4).

---

## 1. What this subsystem is

Audio is a full **ESP-ADF** pipeline: decode (MP3/AAC/OPUS/OGG/WAV/MP4) →
resample → EQ/ALC → I2S → codec. Sources are local SD file, HTTP/HLS stream, and
Bluetooth A2DP **sink** (the device acts as a Bluetooth speaker). Two codec
topologies, selected per hardware revision by the embedded hwconfig JSON:

- **split** — `aw881xx` "smart PA" speaker amp (×2, L/R) + `es8156` headphone
  DAC, independent I2C addresses, `pactrl` amp power-enable.
- **combined** — `es8388` codec (speaker + headphone in one chip, single I2C
  addr).

I2S pins **differ by revision** (combined #01 uses different GPIOs than split
#00/#02/#04/#05). Codec I2C addresses also differ between the single-IOX and
2-IOX split revisions. `output/decompiled/*.c` files are named
`FUN_segN__ADDR_0xADDR.c`.

---

## 2. Component / pin table (enumerate ALL variants — revisions differ)

`output/hwconfig_*.json` audio sections. IOX = I/O expander (PI4IOE5V6416 /
"ET6416"); `IOX.<bank>.<bit>`. `hpdetect` = `IOX.1.1` on every revision.

| hwconfig | audiochip.type | spkrchip | i2cspkrl | i2cspkrr | hpchip | i2chp | pactrl | I2S mclk/bclk/lrclk/out |
|---|---|---|---|---|---|---|---|---|
| `00` (gc9306, sd1, CW2015) | `split` | `aw881xx` | `0x34` | `0x34` | `es8156` | `0x08` | `IOX.0.6` | GPIO.0 / 5 / 18 / 19 |
| `01` (ht16d35x, **combined**, sd SPI, ADC) | `combined` | `es8388` | `0x10` | `0x10` | `es8388` | `0x10` | `IOX.0.6` | GPIO.0 / **25** / **32** / **33** |
| `02` (ht16d35x, sd1, CW2015) | `split` | `aw881xx` | `0x34` | **`0x37`** | `es8156` | **`0x09`** | **`IOX.2.4`** | GPIO.0 / 5 / 18 / 19 |
| `03` (placeholder) | *(none)* | — | — | — | — | — | — | *(none)* |
| `04` (gc9306, sd4, CW2215B) | `split` | `aw881xx` | `0x34` | `0x34` | `es8156` | `0x08` | `IOX.0.6` | GPIO.0 / 5 / 18 / 19 |
| `05` (ht16d35x, sd1, CW2215B) | `split` | `aw881xx` | `0x34` | **`0x37`** | `es8156` | **`0x09`** | **`IOX.2.4`** | GPIO.0 / 5 / 18 / 19 |

Exact JSON evidence (each `output/hwconfig_*.json`, `audio` object):

- `hwconfig_00…` lines 37-54: `"hpdetect":"IOX.1.1"`, `"i2s":{"mclk":"GPIO.0","bclk":"GPIO.5","lrclk":"GPIO.18","out":"GPIO.19"}`, `"audiochip":{"type":"split","spkrchip":"aw881xx","i2cspkrl":"0x34","i2cspkrr":"0x34","hpchip":"es8156","i2chp":"0x08","pactrl":"IOX.0.6"}`.
- `hwconfig_01…` lines 44-61: `"i2s":{"mclk":"GPIO.0","bclk":"GPIO.25","lrclk":"GPIO.32","out":"GPIO.33"}`, `"audiochip":{"type":"combined","spkrchip":"es8388","i2cspkrl":"0x10","i2cspkrr":"0x10","hpchip":"es8388","i2chp":"0x10","pactrl":"IOX.0.6"}`.
- `hwconfig_02…` lines 58-75 and `hwconfig_05…` lines 61-78: `"i2cspkrl":"0x34","i2cspkrr":"0x37","hpchip":"es8156","i2chp":"0x09","pactrl":"IOX.2.4"`.
- `hwconfig_04…` lines 37-54: identical to #00 (single-IOX split).
- `hwconfig_03…` (lines 1-38) has **no** `audio` object at all.

`output/pinmap.json` flattens the same values (audio keys only):

- `i2s.mclk`=GPIO.0, `i2s.bclk`=GPIO.5, `i2s.lrclk`=GPIO.18, `i2s.out`=GPIO.19 (00/02/04/05)
- `i2s.bclk`=GPIO.25, `i2s.lrclk`=GPIO.32, `i2s.out`=GPIO.33 (01)
- `audio.hpdetect`=IOX.1.1 (all); `audio.audiochip.pactrl`=IOX.0.6 (00/01/04) or IOX.2.4 (02/05)

Note: `output/hwconfig_merged.json` (lines 58-75) is a flattened union, **not** a
real board — do not use it for one board's pins.

---

## 3. Exact strings (ground truth, `output/strings.txt` line numbers)

### Codec driver — aw881xx "smart PA" (log tag `aw881xx_split`; version `v1.0.1`)
- `521` `combo_patch_aw881_init` · `522` `_group_es8156_init` · `523` `combo_set_voice_volume_hw_unsafe` · `524` `combo_deinit`
- `525` `aw881xx` · `527` `aw881xx_hw_reset` · `528` `aw881xx_split` · `530` `aw881xx_i2c_read` · `533` `aw881xx_i2c_write`
- `536` `aw881xx_soft_reset` · `537` `I … this chip is Aw881XX chipid=0x%x` · `539` `aw881xx_read_chipid` · `541` `aw881xx_init` · `542` `aw881xx driver version %s` · `543` `v1.0.1`
- `545` `channel %d, overridden to {0x%02x,0x%04x}` · `550` `aw881xx_run_pwd` · `552` `aw881xx_run_mute` · `553` `aw881xx_run_mute2` · `555` `aw881xx_dsp_set_vcalb` · `556` `aw881xx_stop` · `558` `aw881xx_load_reg_cfg` · `560` `aw881xx_fw_cfg` · `561` `aw881xx_dsp_container_update` · `563` `aw881xx_spk_dsp_cfg_`
- `567` `aw881xx_get_iis_status` · `569` `aw881xx_mode1_pll_check` · `571` `aw881xx_mode2_pll_check` · `575` `aw881xx_syspll_check` · `578` `aw881xx_start` · `581` `aw881xx_interrupt_setup` · `584` `aw881xx_interrupt_clear`
- `589` `aw881xx_dsp_enable` · `590` `MEMCLK_PLL` · `591` `MEMCLK_OSC` · `592` `aw881xx_memclk_select` · `595` `aw881xx_get_dsp_status` · `598` `aw881xx_load_dsp_cfg` · `603` `aw881xx_load_fw_cfg`
- `605` `SPK_MODE` · `606` `OFF_MODE` · `608` `aw881xx_set_mode` · `612` `__aw881xx_hw_params` · `623` `aw881xx_set_volume` · `624` `AW881 vol : %.1f` · `637` `aw881xx_cold_start` · `641` `aw881xx_smartpa_cfg` · `645` `aw881xx_ctrl_state` · `646` `PLAY_AUDIO`
- `503` `E … failed to create mono mix aw881xx driver instance` · `504` `E … failed to create left and/or right channel aw881xx driver instance(s)`

### Codec driver — ES8388 (combined; log tag `ES8388_DRIVER`)
- `842` `ES8388_DRIVER` · `843` `E … couldn't access i2c (er: %d)` · `844` `init,out:%02x, in:%02x` · `845` `D … es8388_start default is mode:%d` · `846` `Codec mode not support, default is decode mode` · `848` `es_write_reg` · `849` `es_read_reg`

### Codec driver — ES8156 (headphone DAC; log tag `ES8156`)
- `850` `ES8156` · `856` `E … Failed to read ES8156_DAC_SDP` · `861` `es8156_ctrl_state` · `862` `es8156_channel_mute` · `863` `es8156_set_voice_mute` · `864` `es8156_set_bits_per_sample` · `865` `es8156_set_voice_volume`
- `866` `es8156_read_reg` · `867` `es8156_config_fmt` · `868` `es8156_init` · `869` `es8156_reset` · `870` `es8156_deinit` · `871` `es8156_stop` · `872` `es8156_write_reg` · `873` `es8156_start`
- `499-501` `… not initialising / initialising / deinitialising ES8156 (HP)`

### Board HAL init (fatal on failure — reboots)
- `681` `E … failed to initialise audio board! Rebooting` · `682` `E … failed to initialise audio board HAL! Rebooting`
- `837` `/opt/atlassian/pipelines/agent/build/components/my_board/yoto_v2/board.c` · `838` `AUDIO_BOARD` · `840` `audio_board_codec_init` · `841` `audio_board_init`
- `491` `get_i2s_pins` · `488` `E … i2s port %d is not supported` · `487` `i2c port %d is not supported` · `493` `AUDIO_COMBO` (log tag)

### HW-config JSON keys (audio object)
- `3438` `hpdetect` · `3439` `mclk` · `3440` `bclk` · `3441` `lrclk` · `3442` `audiochip` · `3443` `pactrl` · `3444` `combined` · `3445` `spkrchip` · `3446` `es8388` · `3447` `i2cspkrl` · `3448` `i2cspkrr` · `3449` `hpchip` · `3450` `es8156` · `3451` `i2chp`
- `3452` `Combined audio config issue: both I2C spkr chip addresses must be set the same!` · `3453` `No speaker amp chip is configured!`

### Pipeline / ADF library (source paths = `__FILE__` strings)
- `723` `Local audio stream reader` · `724` `Decoder already initialised!` · `725` `Unknown decoder!` · `726` `_audio_decoder` · `727-730` resampler init/add-to-pipeline · `776` `yoto_file_reader` · `777` `_create_register_decoder` · `778` `_create_register_reader` · `779` `_start_audio`
- `791` `[3.2] Register all elements to audio pipeline` · `795` `[ 6 ] Start audio_pipeline` · `806` `Stopping audio_pipeline` · `807` `Stopping Bluetooth audio pipeline`
- `8148` `/opt/esp/adf/components/audio_pipeline/audio_element.c` · `8192` `…/audio_event_iface.c` · `8200` `…/audio_pipeline.c` · `8219` `…/ringbuf.c` · `8233` `…/audio_hal/audio_hal.c` · `8242` `…/audio_stream/i2s_stream.c` · `8255` `HTTP_STREAM` · `8141` `…/esp_codec/filter_resample.c` · `8142` `resample` · `8143` `rsp_filter_init` · `9801` `…/esp-resample/src_fa/src/fa_resample.c` · `9802` `FIR_RESAMPLE`

### Decoders & MIME types
- `211` `ADF_BIT_STREAM` · `212` `AAC_DECODER` · `213` `STAGEFRIGHTMP3_DECODER` · `4254` `DEC_AAC` · `4302` `DEC_MP3` · `4328` `DEC_OPUS` · `4329` `OPUS_DECODER` · `4337` `DEC_OGG` · `4433` `TS_DECODER` · `4447` `DEC_WAV`
- `4296-4301` `aac_decoder_init/process/close/open` + source `aac_decoder.c` · `4323-4327` `mp3_decoder_*` + `mp3_decoder.c` · `4353-4357` `decoder_opus_init` / `opus_decoder_*` + `opus_decoder.c` · `4464` `pvmp3_framedecoder.cpp` · `4466` `pvmp3_InitDecoder`
- MIME: `8267` `audio/x-aac` · `8268` `audio/mp4` · `8269` `audio/aacp` · `8270` `video/MP2T` · `8271` `audio/wav` · `8272` `audio/opus` · `8273` `application/vnd.apple.mpegurl` (HLS) · `8274` `vnd.apple.mpegURL` · `8275` `audio/x-scpls` (Shoutcast PLS)

### ALC / EQ / volume
- `8112` `Failed to create ALC handle` · `8114` `…/esp_codec/audio_alc.c` · `8115` `alc_volume` · `8116` `alc_volume_setup_init` · `8249` `The ALC don't be used. It can not be set.` · `498` `Changing %s volume (via ALC)` · `461` `Setting ALC volume: %d`
- `8117` `EQUALIZER` · `732` `Equalizer Preset "%s" found in NVS APP settings` · `733` `Equalizer has been set to off` · `7610-7611` `"APP_EQ_PRESET"` (NVS key, default `"default"`) · `7791` `APP_EQ_PRESET`
- `810` `Speaker vol curve [%02d] = %d` · `811` `Headphone vol curve [%02d] = %d` · `812` `Volume table entries must all be in range 0 to 100` · `813` `Volume table does not contain 17 entries`

### Sleep timer / volume fade
- `1707` `Sleep timer not initialised` · `1710` `Dropping volume` · `1712` `could not restore user volume after sleep timer expired` · `1713` `_step_down_volume` · `1714` `stimer_process` · `1715` `stimer_stop`

### Bluetooth A2DP / AVRC
- `1914` `BT_AV_TAG` · `1915` `AUDIO_PIPELINE` · `1917` `play_startup_audio` · `783` `PLAY_BLUETOOTH` · `784` `bluetoothtask` · `789` `= ^_^ Welcome to Yoto Sink Mode ^_^ =` · `792` `[3.3] Link it together [Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->[codec_chip]`
- `375` `AVRC conn_state evt` · `376` `AVRC passthrough cmd: key_code 0x%x, key_state %d` · `378` `AVRC set absolute volume: %d%%` · `392-395` `A2DP connection/audio state` · `396` `A2DP audio stream configuration, codec type %d` · `397` `Bluetooth configured, sample rate=%d` · `399` `a2dp media ready checking` · `408` `a2dp media ready, starting` · `409-412` media START/SUSPEND/STOP
- `469` `bt_avrc_ct_cb` · `470` `_system2avrc` · `471` `_avrc2system` · `472` `bt_avrc_tg_cb` · `473` `bt_a2d_source_cb` · `474` `bt_app_gap_cb` · `442` `…/components/ybluetooth/blue_service.c` · `466` `blue_service_create_stream` · `467` `blue_service_create_alc` · `468` `blue_service_destroy`
- `6075` `Advanced Audio Sink` · `6076` `AV Remote Control Target` · `6077` `AV Remote Control Controller`

### Headphone routing / mono-mix / diagnostics
- `3338` `Headphones removed: switching amps on if there is audio` · `3339` `Headphones inserted: muting amps` · `3373` `EVT_HEADPHONES_IN` · `3375` `EVT_HEADPHONES_OUT`
- `1838` `Mono-mix (speaker)` · `1840` `Mono-mix (headphone)` · `1855` `Mono-mix (speaker) audio test failed` · `1859` `Mono-mix (headphone) audio test failed` · `1856` `play_freq_sweep`
- `2694` `/eq-gains` · `2695` `/vol-curve` · `2705` `/dump-aw881` (DSP register dump HTTP endpoints)

### System sounds (stored under `/system/sounds/`)
- `304-314`: `setup_complete_no_voice`, `setup_next`, `battery_fault_beep`, `test_no_sound`, `setup_fail`, `welcome`, `low_battery`, `disconnected_from_power_3_of_3`, `disconnected_from_power_2_of_3`, `disconnected_from_power_low`, `connected_to_power`

---

## 4. Function addresses (decompiled; mark inference)

All are Ghidra auto-named `FUN_segN__ADDR`. String→function mapping was recovered
manually: find each string's DROM address → locate its LE32 literal in the IROM
segment (the L32R literal-pool slot) → grep `output/decompiled/*.c` for the
`DAT_seg4__<literal-addr>` reference. Ghidra does **not** resolve L32R literal
pools on generic Xtensa, so `output/string_xrefs.json` is effectively empty
(one entry). Roles marked `[INFERENCE]` come from log strings + call structure,
not symbol names.

### Main audio module (orchestration)
| Address | Size | Role |
|---|---|---|
| `0x400e193c` | 4624 | **`_start_audio` / `_audio_decoder`** — the top-level audio-module function (references both `_start_audio` and `_audio_decoder` fn-name strings). Builds the pipeline: reader → decoder → (resampler) → EQ → I2S `[INFERENCE]` |
| `0x401137e8` | 4407 | **hwconfig audio parser** — reads the `audio` JSON object (`hpdetect`/`audiochip`/`pactrl`/`i2cspkrl`/`i2cspkrr`/`i2chp` keys @ `DAT_seg4__4010ecb8/…/…`) and drives codec/amp selection |

### Board HAL (board.c — `audio_board_codec_init` / `audio_board_init`)
| Address | Size | Role |
|---|---|---|
| `0x400e4324` | 82 | `audio_board_codec_init` — references `audio_board_codec_init`, `board.c`, `AUDIO_BOARD`, `audio_board` strings |
| `0x400e4378` | 118 | board helper (reads/writes codec regs, references `board.c`/`AUDIO_BOARD`) |

### aw881xx driver (log tag `aw881xx_split` @ literal `0x400d0f00`; 31 functions total)
The whole driver is `FUN_seg4__400def4c` … `FUN_seg4__400e0390`. Named members:
| Address | Size | Role |
|---|---|---|
| `0x400df270` | 227 | `aw881xx_init` (references `aw881xx_init` fn-name) |
| `0x400df0d0` | 108 | `aw881xx_i2c_write` (references `aw881xx_i2c_write`) |
| `0x400df1f4` | 124 | `aw881xx_read_chipid` (references `aw881xx_read_chipid`) |
| `0x400e02bc` | 210 | `aw881xx_smartpa_cfg` (references `aw881xx_smartpa_cfg`) |
| `0x400deae0` | 129 | `combo_patch_aw881_init` (references `combo_patch_aw881_init`) |

### ES8388 driver (log tag `ES8388_DRIVER` @ literal `0x400d180c`)
| Address | Size | Role |
|---|---|---|
| `0x400e441c` | 50 | ES8388 helper (read/write) `[INFERENCE]` |
| `0x400e447c` | 50 | ES8388 helper `[INFERENCE]` |
| `0x400e47f0` | 207 | ES8388 codec init/start (references `es8388_start` default mode) `[INFERENCE]` |

### ES8156 driver (headphone DAC)
| Address | Size | Role |
|---|---|---|
| `0x400e4a24` | 74 | `es8156_read_reg` (references `es8156_read_reg`) |
| `0x400e4c60` | 137 | `es8156_config_fmt` (references `es8156_config_fmt`) |
| *(nearby `0x400e4xxx`)* | — | `es8156_init`/`set_voice_volume`/`set_voice_mute`/`write_reg` are in this cluster; not individually xref'd `[INFERENCE]` |

### Decoder wrappers (esp-wrapper; each references its `__FILE__`)
| Address | Size | Source file |
|---|---|---|
| `0x40143c5c` | 265 | `aac_decoder.c` |
| `0x40144798` | 272 | `mp3_decoder.c` |
| `0x40145748` | 248 | `opus_decoder.c` |
| `0x4014bd3c` | 609 | `pvmp3_framedecoder.cpp` (libmp3) |

### ADF audio_pipeline / audio_stream library (source-path xrefs)
| Address | Size | Source file |
|---|---|---|
| `0x401d24a4` / `0x401d24f8` / `0x401d2a50` / `0x401d2fc8` | 82/…/531 | `audio_element.c` (`0x401d2fc8` = `audio_element_init`, refs `audio_element_init`) |
| `0x401d395c` / `0x401d3c60` | 243/… | `audio_event_iface.c` |
| `0x401d3e04` | 221 | `audio_pipeline.c` |
| `0x401d4564` | 187 | `ringbuf.c` |
| `0x401d4fa4` / `0x401d5408` | 132/467 | `i2s_stream.c` (`0x401d5408` = `i2s_stream_init`, refs `i2s_stream_init`) |
| `0x401d5bec` | 1238 | `http_stream.c` |
| `0x401f540c` / `0x401f553c` / `0x401f5844` | 297/…/3065 | `fa_resample.c` (`0x401f5844` = `fa_resample_open`) |

### Bluetooth service (blue_service.c)
| Address | Size | Role |
|---|---|---|
| `0x400dd978` | 981 | BT service — A2DP source/sink stream setup `[INFERENCE]` |
| `0x400ddd58` / `0x400dde88` / `0x400ddf3c` | 194/179/196 | BT service helpers (all reference `blue_service.c` `__FILE__`) |

### Not recovered (Ghidra gaps — literal pools exist but no decompiled owner)
`audio_hal.c` (@ lit `0x401c7688`), `hls_playlist.c` (@ `0x401c7874`),
`filter_resample.c` (@ `0x401c7434`), and the `Mono-mix (speaker)` string (@
`0x400d2fc0`) have literal-pool slots but **no** decompiled referencing
function — their code lives in regions Ghidra did not disassemble into
functions.

### DROM string address ↔ IROM literal-pool slot (for future xref work)
| String | DROM addr | literal slot (IROM) |
|---|---|---|
| `board.c` (path) | `0x3f407c5c` | `0x400d17e0` |
| `AUDIO_BOARD` (tag) | `0x3f407c64` | `0x400d17e4` |
| `audio_board_codec_init` | `0x3f407cc1` | `0x400d17f0` |
| `aw881xx_split` (tag) | `0x3f404bba` | `0x400d0f00` |
| `aw881xx_smartpa_cfg` | `0x3f405abb` | `0x400d114c` |
| `blue_service.c` (path) | `0x3f40408d` | `0x400d0cb4` |
| `aac_decoder.c` / `mp3_decoder.c` / `opus_decoder.c` | `0x3f42b575` / `0x3f42ba15` / `0x3f42bf58` | `0x4010f838` / `0x4010f8f0` / `0x4010f9ec` |
| `pvmp3_framedecoder.cpp` | `0x3f42d823` | `0x40147f28` |
| `audio_element.c` / `audio_event_iface.c` / `audio_pipeline.c` / `ringbuf.c` / `audio_hal.c` / `i2s_stream.c` / `http_stream.c` / `hls_playlist.c` | `0x3f47162d` … `0x3f472ccd` | `0x401c7478` … `0x401c7874` |
| `fa_resample.c` / `filter_resample.c` | `0x3f4864d0` / `0x3f471538` | `0x401c8e38` / `0x401c7434` |
| `hpdetect` / `audiochip` / `pactrl` (JSON keys) | `0x3f420e30` / `0x3f420e49` / `0x3f420e53` | `0x4010ecb8` / `0x4010ecdc` / `0x4010ece4` |

---

## 5. Protocol / flow detail

### Boot & codec selection
1. Boot parses the embedded hwconfig JSON (`FUN_seg4__401137e8`): `audio.audiochip.type` = `"split"` | `"combined"` (strings 3444), `spkrchip`, `hpchip`, `i2cspkrl/r`, `i2chp`, `pactrl`, `hpdetect` (strings 3445-3451, 3438, 3443).
2. Validation: combined requires `i2cspkrl == i2cspkrr` (string 3452); no speaker chip → error (3453).
3. `audio_board_init` / `audio_board_codec_init` (board.c, strings 840/841) configure the codec + I2S. Failure is **fatal** — `failed to initialise audio board! Rebooting` (681) / `…HAL! Rebooting` (682) then reset.

### I2S
- Pins via `get_i2s_pins` (491); only one I2S port is wired (`i2s port %d is not supported`, 488). mclk must be GPIO0/1/3 on ESP32 (`ESP32 only support to set GPIO0/GPIO1/GPIO3 as mclk`, strings.txt ~9872).
- Split: `mclk`=GPIO.0, `bclk`=GPIO.5, `lrclk`=GPIO.18, `out`=GPIO.19. Combined: `bclk`=GPIO.25, `lrclk`=GPIO.32, `out`=GPIO.33 (GPIO32/33 are otherwise NFC UART on split boards — pins are re-purposed per revision).
- `audio_hal.c` (`audio_hal_init`, 8239) owns the codec + I2S; `i2s_stream.c` (`i2s_stream_init`, 8254) is the ADF stream writer. Clocking: `i2s_set_clk`/`i2s_calculate_clock` (9925/9924), mclk division errors 9838-9842.

### Playback pipeline (local / HTTP / HLS)
```
source (yoto_file_reader / http_stream / hls_playlist)
   └─> _create_register_reader (778) ── reader element
   └─> _create_register_decoder (777) ── decoder element (_audio_decoder, 726)
        │  MP3 (pvmp3/mp3_decoder) | AAC (aac_decoder, DEC_AAC) | OPUS (opus_decoder, DEC_OPUS)
        │  | OGG (DEC_OGG) | WAV (DEC_WAV) | MP4/AAC-LC | TS (TS_DECODER)
   └─> resampler (fa_resample.c / filter_resample.c, rsp_filter_init 8143)
   └─> EQ (EQUALIZER 8117, APP_EQ_PRESET) / ALC (audio_alc.c 8114, alc_volume 8115)
   └─> i2s_stream_writer ── I2S ──> codec ──> aw881xx speaker amp / es8156 headphones
```
- Pipeline wiring logged as `[3.2] Register all elements` → `[ 6 ] Start audio_pipeline` (791, 795); `audio_pipeline.c` (`audio_pipeline_init` 8216, `audio_pipeline_run` 8214, `audio_pipeline_register` 8215).
- HLS detected via `application/vnd.apple.mpegurl` (8273/8274); Shoutcast via `audio/x-scpls` (8275).
- MIME→decoder: `audio/mp4`/`audio/aacp`/`audio/x-aac` → AAC (8268/8269/8267); `audio/wav` (8271); `audio/opus` (8272); `video/MP2T` → TS (8270).

### aw881xx "smart PA" DSP flow
1. `aw881xx_init` (541) → `read_chipid` (539, logs `chipid=0x%x` 537).
2. `aw881xx_load_fw_cfg`/`aw881xx_load_dsp_cfg`/`aw881xx_dsp_container_update` (603/598/561) upload firmware + DSP config; `aw881xx_cold_start` (637) on boot.
3. `aw881xx_start` (578) with I2S signal check (`get_iis_status` 567, `syspll_check` 575, mode1/2 PLL checks 569/571) before `dsp_enable` (589).
4. `aw881xx_set_mode` (608) SPK_MODE/OFF_MODE (605/606); `set_volume` (623) writes `AW881 vol`; `__aw881xx_hw_params` (612) applies sample rate/bit width.
5. Mono-mix path: `failed to create mono mix aw881xx driver instance` (503) / `left and/or right channel …` (504).

### Headphone routing
- `hpdetect` (IOX.1.1) senses jack insertion: `Headphones inserted: muting amps` (3339) / `Headphones removed: switching amps on` (3338); events `EVT_HEADPHONES_IN/OUT` (3373/3375).
- Split config: speaker volume via aw881xx, headphone via `es8156_set_voice_volume` (865); ES8156 init/deinit gated on shutdown (`not initialising ES8156 (HP)` 499).
- Combined config: one ES8388 handles both paths (`combo_set_voice_volume_hw_unsafe` 523).

### Bluetooth A2DP sink
- Device boots into `Bluetooth Sink Mode` (3321) when enabled (strings 3306/3307, 3312); `bluetoothtask` (784) drives `PLAY_BLUETOOTH` (783).
- `blue_service.c` (442) sets up the A2DP **source→sink** stream: `blue_service_create_stream` (466) / `create_alc` (467); `bt_a2d_source_cb` (473).
- AVRC: `bt_avrc_ct_cb` (469, controller) + `bt_avrc_tg_cb` (472, target); passthrough key codes (`AVRC passthrough cmd: key_code 0x%x` 376) mapped to system events via `_avrc2system` (471) / `_system2avrc` (470); absolute volume (378) applied as ALC volume (461).
- Sink pipeline (792): `[Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->[codec_chip]`; media state machine logs `a2dp media ready/START/SUSPEND/STOP` (408-412).
- SBC codec config reported at `A2DP audio stream configuration, codec type %d` (396) and `Bluetooth configured, sample rate=%d` (397).

### Volume / EQ / sleep timer
- Per-path volume curves: `Speaker vol curve` (810) / `Headphone vol curve` (811), 17 entries (813).
- Sleep timer: `_step_down_volume` (1713) fades volume (`Dropping volume` 1710), restores on expiry (1712); driven by `stimer_process` (1714).
- HTTP diagnostics: `/eq-gains`, `/vol-curve`, `/dump-aw881` (2694/2695/2705) dump EQ gains, volume curve, and aw881xx DSP registers.

---

## 6. Raw file pointers

- Strings: `output/strings.txt` (line numbers cited inline above)
- HW configs: `output/hwconfig_00…05_*.json` (audio object lines listed in §2), `output/hwconfig_merged.json` (union, don't use per-board)
- Pin map: `output/pinmap.json`
- Function lists: `output/decompiled_manifest.json` (5,915 entries, **decimal VMA** `addr`), `output/ghidra_functions.json` (9,185 entries, **decimal file-offset** `addr`)
- Decompiled C: `output/decompiled/FUN_segN__ADDR_0xADDR.c`
- String xrefs (sparse/unreliable here): `output/string_xrefs.json` (1 entry)
- Extraction/decompile scripts: `analysis/extract_strings.py`, `analysis/extract_hwconfig.py`, `analysis/ghidra_dump.py`, `analysis/ghidra_decompile.py`
- Layout/segments: `output/layout.json` (DROM `0x3F400020`, IROM `0x400D0020`)

Caveat for future agents: Ghidra generic Xtensa does not resolve L32R
literal-pool references, so string→function xrefs are sparse. The
string-address ↔ literal-slot table in §4 is the reliable manual map; re-derive
by LE32-searching `output/factory.bin` for a string's DROM address and grepping
`output/decompiled/*.c` for the resulting `DAT_seg4__<addr>` label.
