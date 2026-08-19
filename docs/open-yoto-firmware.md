---
icon: lucide/file-cog
---

# open-yoto-firmware

A from-scratch firmware for the Yoto Player (ESP32). It reads NFC cards and
plays the matching audio + picture, with an offline admin mode for loading
your own content — **no Wi-Fi, Bluetooth, or cloud unless you explicitly turn
on admin mode**.

This page explains the logic in plain terms. The decompilation-based
understanding of the *original* firmware is elsewhere (see
[Hardware & Ports](hardware.md) and the [AI reference](ai/index.md)).

## The big picture

```mermaid
flowchart TD
  A[boot] --> B[init hardware + services]
  B --> C{main loop}
  C -->|powered off| C
  C -->|admin mode| D[web server runs in background]
  C -->|normal| E[check battery ~30s]
  C -->|normal| F[poll NFC]
  F -->|magic URL| G[toggle admin mode]
  F -->|content card| H[look up content]
  H --> I[show image + play track]
  E -->|low| J[show low-battery art]
```

## What it does

1. **Scans an NFC card** → reads its URL.
2. **Looks up that URL** in a file on the SD card (`mapping.json`).
3. **Plays the matching audio** (MP3) and **shows the matching picture** (a
   16×16 pixel image).
4. The two knobs control **volume** (left) and **track skip** (right); pressing
   them **pauses**. Long-pressing the right knob or the power button turns the
   device **off/on**.
5. Scanning a special "magic" card toggles **admin mode** — a temporary Wi-Fi
   hotspot + web page where you add, browse, preview, play, and delete content.

## The components

| Component | Job |
|-----------|-----|
| `board` | the pin map (which wire goes where) |
| `iox` | the two I/O expander chips (buttons, display, power control) |
| `battery` | fuel-gauge + battery level / low-battery detection |
| `ht16d35x` | the 16×16 LED display |
| `cr95hf` | the NFC card reader |
| `encoder` | the two rotary knobs + push buttons + power button |
| `content` | the SD card + `mapping.json` catalog |
| `audio` + `codec_es8156` + `libhelix-mp3` | MP3 playback → headphone DAC |
| `admin` | the Wi-Fi hotspot + web page |
| `main` | ties it all together (the state machine) |

## Boot sequence

On power-up, in order:

1. Initialize settings storage (erase it if it's corrupt).
2. Start the I²C bus + both I/O expanders.
3. Battery monitor.
4. 16×16 display.
5. NFC reader.
6. Rotary encoders + buttons.
7. SD card + content catalog.
8. Audio.

Then it logs the battery level, checks it once, and enters the main loop.

## The main loop (the state machine)

It's one loop with three gates, then the real work:

1. **Powered off?** → sleep 200 ms, do nothing (the power button/knob still
   works because buttons are event-driven).
2. **Admin mode?** → sleep 200 ms; the web server runs on its own in the
   background.
3. **Normal mode** → every ~30 s check the battery (show the low-battery art if
   depleted), then poll for an NFC card.

When a card is detected:

- **Magic URL** (`openyoto.local/admin`) → toggle admin mode on/off.
- **Anything else** → look it up in the catalog; if found, show its image and
  start playing its first track.

The knobs/buttons are handled as events (not polled in the loop):

| Input | Action |
|-------|--------|
| Left knob turn | volume (5 per click, 0–100) |
| Right knob turn | skip track (wraps around) |
| Either knob press (short) | play / pause |
| Right knob press (long, ~0.8 s) | power off/on |
| Power button | power off/on |

## Card → playback flow

```mermaid
flowchart LR
  A[NFC card] --> B[CR95HF reads UID + URL]
  B --> C[look up URL in mapping.json]
  C --> D[image + tracks list]
  D --> E[show image]
  D --> F[play track 0]
  F --> G[skip knob cycles tracks]
```

- The NFC reader talks over UART (57,600 baud). It reads the card's UID (4 or
  7 bytes) and its **NDEF URL**.
- The **URL is the key** (not the UID). `mapping.json` maps each URL to an
  image and an ordered list of audio tracks:

  ```json
  {"cards": [{"url": "https://example.com/card",
              "image": "media/cover.img",
              "tracks": ["media/track1.mp3", "media/track2.mp3"]}]}
  ```

- Images are **16×16, one-bit (32 raw bytes)** — pre-scaled in the browser, so
  the device just displays the bytes. Audio is MP3, streamed from the SD card.

## Audio pipeline

```
SD card → libhelix-mp3 (decode) → I2S → ES8156 headphone DAC
```

- A background task reads + decodes the MP3 and pushes 16-bit stereo samples
  (44.1 kHz) out the I²S port.
- Play/stop/pause/volume are simple flags + volume writes to the DAC.
- The speaker amp (`aw881xx`) is intentionally left off — its init sequence was
  never cracked; the **headphone path (ES8156) is the working output**.

## Admin mode

Scanning the magic card starts a temporary **open Wi-Fi hotspot** (`openyoto`,
at `192.168.4.1`) and a web server. The 4-digit access code appears on the
16×16 display and gates any *changes* (viewing is open).

The web page lets you, from a phone or laptop:

- **Add** content (URL + sound + optional image) — the image is resized to
  16×16 in the browser before uploading, with a live preview.
- **Browse** content — each item shows its picture and a play button (audio
  streams in the browser).
- **Delete** content.

Routes: `GET /` (the page), `GET /api/list`, `GET /media/*` (serve files),
`POST /api/add`, `POST /api/delete`.

## Power & battery

- Battery is read from a **CW2215B fuel gauge** (I²C), falling back to a raw
  ADC reading if the gauge is absent.
- **Low battery** = charge < 15% **or** voltage < 3.3 V — checked at boot and
  every ~30 s, showing the low-battery art (but not shutting down).
- "Off" is a software standby (stops audio + blanks the display); true
  deep-sleep power-off needs the I/O-expander interrupt pin as a wake source
  and isn't implemented yet.
