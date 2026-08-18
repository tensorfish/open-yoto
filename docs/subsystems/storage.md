---
icon: lucide/hard-drive
---

# SD Card & Storage

The SD card is the primary offline content store. It holds the audio for
Yoto cards, preloaded content metadata, icons, and playback logging. Access is
via **FatFS** (ESP-IDF `fatfs` VFS component) over either **SDMMC** or **SPI**,
depending on hardware config.

## Interface

| Config | Transport | Pins |
|--------|-----------|------|
| #01 | **SPI** (`sd.type` = `"spi"`; ESP-IDF `sdspi` host) | `cs`=GPIO15, `sclk`=GPIO14, `mosi`=GPIO22, `miso`=GPIO34 |
| #00/#02/#05 | **SDMMC 1-bit** (`"sd1"`) | `clk`=GPIO14, `cmd`=GPIO15, `d0`=GPIO2 |
| #04 | **SDMMC 4-bit** (`"sd4"`) | + `d1`=GPIO4, `d2`=GPIO12, `d3`=GPIO13 |

Mount point: `/sdcard`. The SDMMC host driver is the standard ESP-IDF
`sdmmc_host` (log strings reference `sdmmc_host_start_command`,
`sdmmc_send_cmd (ERASE)`, `sdmmc_decode_scr`, `sdmmc_io_send_op_cond`, etc.).

## On-card layout

```text
/sdcard/
├── .preload.json              # preloaded content manifest (cardID, contentVersion, version, sizeKB)
├── cards/                     # card content (audio per card/chapter/track)
├── icons/                     # icon images (static PNG + animated GIF)
├── play_log                   # playback logging
├── nvs_confg.json             # device configuration overrides
├── wmh.json                   # walkman history
├── boot_cnt                   # boot counter
├── reset                      # factory-reset trigger file
├── prodtest.txt               # production test marker (renamed to prodtest_renamed.txt)
└── System Volume Information  # (ignored, filtered in listings)
```

## Content model: card → chapter → track

The firmware maps a scanned NFC URL to audio content using a three-level key
hierarchy:

```text
card (from NFC URL/UID)
 └─ chapter (group of tracks)
     └─ track (a single audio file)
```

Evidence:

```text
E (%lu) %s: %s() Chapter out of range
E (%lu) %s: %s() cannot skip tracks without inserted card
E (%lu) %s: Card ID and/or chapter and/or track key too long, disabling this slot: %s
E (%lu) %s: --- CB item (%02d): track key '%s' for chapter key '%s' not found for card %s ---
E (%lu) %s: Attempt to start shuffling with invalid chapter index, defaulting to 2
E (%lu) %s: Either both chapter and track are specified, or neither one is (%s/%s)
```

Playback state is stored per card/track: `/sdcard/%s/%s_status` and
`/sdcard/%s/%s`.

## Preloaded content manifest

`/sdcard/.preload.json` describes content that ships on the card. Parsed
fields include `cardID`, a numeric `contentVersion`, and per-entry `version`
and `sizeKB`:

```text
E (%lu) %s: Found non-numeric contentVersion entry in preload config file!
E (%lu) %s: Found non-numeric sizeKB entry in preload config file!
E (%lu) %s: Found non-numeric version entry in preload config file!
E (%lu) %s: unable to open preload config file
E (%lu) %s: %s: unable to check preload state (so we'll try to format the SD)
%llu MB of preloaded content is specified in the preload config file
```

The firmware checks preload state (and free space) at boot and can (re)format
the SD if the manifest is unreadable. `contentListSha` and `contentVersion`
are reported to the cloud to determine whether content needs updating.

## Custom buttons → content

The two rotary-encoder knobs and their push buttons can be bound to specific
content. Each custom-button slot stores a **card**, **chapter**, and **track**
key plus a fallback URL:

```text
E (%lu) %s: _custom_button_content_chapterkey is NULL, did CB init fail?
E (%lu) %s: _custom_button_content_trackkey is NULL, did CB init fail?
E (%lu) %s: _custom_button_content_url is NULL, did CB init fail?
custom_button_get_content_url
```

## Icons

Icons for cards/chapters are rendered on the display and cached under
`/sdcard/icons`. Both static (PNG) and animated (GIF, `icon_anm`) icons are
supported. Icon state changes are logged:

```text
D (%lu) %s: Icon changed for %s. Was %s is %s
D (%lu) %s: IconId set to: '%s'
D (%lu) %s: Rendering icon %d. Overlay:%llu
```

## Read/write summary

- **Read path**: NFC URL → resolve card → open `/sdcard/cards/<card>/…` audio
  (MP3/MP4/AAC/WAV/OPUS/OGG via ESP-ADF decoders) → stream to I2S. If the
  content is not local, the URL is used to **stream over HTTP(S)** (see
  [Audio](audio.md)).
- **Write path**: OTA downloads, icon cache updates, and `play_log` append.
  Files are copied with `Failed to copy file, cannot access sdcard` guarding
  free-space/card-presence failures. Free space is queried via FatFS
  (`Failed to obtain sdcard space info, FRESULT %d`).
- **Mount management**: `_check_for_preloaded_sd_and_unmount` and the SD host
  reset/clock-update paths (`sdmmc_host_set_card_clk`, `sdmmc_host_reset`)
  handle card insertion/removal.
