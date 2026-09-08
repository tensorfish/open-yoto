---
icon: lucide/wifi
---

# Post-flash setup

Open Yoto's administration page runs on the player. It does not use a Yoto
account, Yoto's servers, or an internet connection. You open it with a dedicated
NFC admin card, join the player's temporary Wi-Fi network, and manage the SD
card from your phone or computer. This guide assumes the source-built firmware
has passed the [initial boot checks](flash-custom-firmware.md#5-first-boot-acceptance-checklist);
the project is still in early development.

!!! warning "Local does not mean encrypted"
    The `openyoto` hotspot has **no Wi-Fi password**, and the web interface uses
    **HTTP, not HTTPS**. The displayed code gates API access; it does not encrypt
    traffic. Nearby attackers may observe the login code, session cookie, and
    uploaded content, or interfere with traffic. Use admin mode briefly in a
    trusted physical environment, avoid sensitive files, and stop it when done.
    Do not bridge or forward this service onto another network. See the
    [security limitations](../project.md#security).

## What you need

- A Yoto Mini running Open Yoto.
- One writable NFC Type 2 card reserved as the admin card.
- A phone or other NFC writer that can write an NDEF URI record.
- A phone, tablet, or computer with Wi-Fi.
- `firmware/components/admin/html/index.html` from the **same source revision**
  you built and flashed.

Copy that `index.html` from your checkout before joining the hotspot, which has
no internet access. The [repository copy](https://github.com/tensorfish/open-yoto/blob/main/firmware/components/admin/html/index.html)
is useful for browsing, but `main` can change: do not use its latest file with
an older firmware build. Keep the matching firmware, source revision, and UI
together so you can replace a broken page.

## 1. Make the admin card

Use an NFC writing app or writer to create one **URI/URL NDEF record** containing
exactly:

```text
https://openyoto.com/admin
```

The firmware compares the complete URI. Do not add a trailing slash, query
string, or other text. Label this card and keep it: it is how you enter and leave
admin mode.

For the byte-level NDEF representation, see
[NFC: verified admin card record](../subsystems/nfc.md#required-type-2-layout).

## 2. Enter admin mode

1. Turn on the Yoto Mini and wait for it to finish starting.
2. Place the admin card on the NFC reader.
3. Wait for the player to display a new **six-character code** made from capital
   letters and numbers.
4. On your phone, tablet, or computer, join the Wi-Fi network named
   **`openyoto`**. It is an open network and has no Wi-Fi password.
5. If your device warns that this network has no internet, choose to stay
   connected.
6. Open a browser and visit **[http://192.168.4.1](http://192.168.4.1)**. Use
   `http`, not `https`.
7. Enter the six-character code shown on the player.

A successful login creates a session cookie in that browser. A fresh code is
created each time admin mode starts. Neither the code nor the cookie provides
transport encryption, and the code also appears in the UART log.

## 3. Install the web interface

On a new SD card, `http://192.168.4.1` redirects to a small installer built into
the firmware:

1. Enter the six-character code and select **Continue**.
2. Choose the `index.html` downloaded before joining the hotspot.
3. Select **Upload web UI**.
4. Return to `http://192.168.4.1` after the upload completes.

The player stores the uploaded file as `/sdcard/webui/index.html` and serves it
directly from the SD card on later visits. The installer remains available at
`http://192.168.4.1/upload` when you want to replace it.

### Customize the page

The web UI is deliberately stored on the SD card rather than compiled into the
firmware. You can edit `index.html` before uploading it or replace it later.
Keep an untouched copy of the provided file so you can recover from a broken
custom page.

A replacement interface must still use the player's local HTTP API if it is to
manage files, playback, and cards. The provided page from the matching source
revision is the reference implementation; compatibility across revisions is
not guaranteed.

## 4. Add media

After login, use the local page to manage content under `/sdcard/media`:

- Upload individual MP3, AAC, or M4A files.
- Upload a complete folder from your phone or computer.
- Browse, rename, download, or delete local files and folders.
- Add PNG or JPEG artwork; the page converts it for the player's display.
- Play local files and control the player from the browser.

Uploads travel directly from your device to the Yoto Mini over the `openyoto`
hotspot. They are not sent to Yoto or another cloud service.

## 5. Create a playable card

Keep admin mode active while creating a card:

1. Open **Cards**, choose **New card**, and keep the default automatic creation mode.
2. Put a writable blank card on the player's NFC reader. During admin mode, the
   firmware captures the scanned card for the page instead of starting normal
   playback.
3. Choose the media folder or tracks the card should play.
4. Choose its artwork and track order.
5. Select **Create & write card** while that same card remains on the reader.
   This saves the playlist and writes its generated URI to the card. Wait for
   success before removing it; review any warning about replacing an existing link.
6. Leave admin mode, remove and reinsert the new card, and test playback.

Manual creation saves a supplied URI without writing a physical card. To write
a saved playlist later, insert the intended card and use **Write to card** in
**Library**. Do not confuse saving a manual entry with a successful NFC write.

The library and card mappings stay on the player's SD card. Normal playback
reads them locally and does not sync with Yoto's services. If those services
become unavailable, Open Yoto's local media and cards continue to work.

There is no stock Yoto content-import workflow here. Open Yoto does not copy
paid or protected Yoto content. Only upload files you own or have permission
to use.

## 6. Leave admin mode

Place the admin card on the reader again. The player stops the hotspot and
returns to normal card playback. Do this when administration is finished; the
`openyoto` Wi-Fi network is intentionally temporary.

## Optional: use it as a Bluetooth speaker

Bluetooth is off at boot. To receive audio from a phone or computer:

1. Stop local playback (remove the media card) and leave admin mode.
2. Hold the **right knob for three seconds** to enable Classic Bluetooth.
3. In your phone or computer's Bluetooth settings, select **Open Yoto** as the
   audio device and start playback there.
4. Use the player's left knob for volume.
5. Hold the right knob for three seconds again to disable Bluetooth.

The top-right indicator shows Bluetooth enabled: a blue dot on #04, an on/off
LED on #05. This is an **A2DP speaker sink**, not output to Bluetooth headphones,
and does not copy the streamed audio to the SD card or create an NFC playlist.
It is discoverable/connectable while enabled; leave it off when not in use.

## Check the finished setup

- Upload a short known-good MP3 or AAC-LC/M4A file and play it at low volume.
- Write a media card, stop admin mode, remove/reinsert the card, and check
  playback, volume, pause/resume, and stopping on card removal.
- Confirm the `openyoto` hotspot disappears after leaving admin mode.
- Back up the SD contents after creating your library. Preserve
  `/sdcard/media/library.json`, the referenced media/artwork, and
  `/sdcard/webui/index.html` together. Keep the untouched stock backup separate.

For firmware changes, follow [updates and recovery](flash-custom-firmware.md#updates-and-recovery).
The `/upload` installer only replaces HTML on the SD card; it cannot update
firmware or provide an OTA rollback.

## Troubleshooting

### The `openyoto` hotspot does not appear

- Confirm the admin card contains the exact URI `https://openyoto.com/admin`.
- Confirm it is an NDEF URI record rather than plain text.
- Remove the card, wait briefly, and scan it again.
- Restart the player and wait for startup to finish before scanning.

### `192.168.4.1` does not open

- Confirm your device is still connected to `openyoto` despite its no-internet
  warning.
- Enter `http://192.168.4.1` explicitly; HTTPS will not work.
- Temporarily disable mobile-data auto-switching or any VPN that takes traffic
  away from the local Wi-Fi network.

### The access code is rejected

- Enter all six displayed characters; letters are uppercase.
- Scan the admin card again to stop admin mode, then scan it once more to start
  a new session with a new code.

### The normal admin page is missing

Open `http://192.168.4.1/upload`, enter the current code, and upload a known-good
`index.html` from the same source revision as your firmware. Reload the page
after replacement; a stale browser copy or mismatched UI can produce API errors.

### Uploads fail or saved media disappears

- Check the UART log for `SD/content initialization failed`; the welcome sound
  can play even when the SD card does not mount.
- Check free space and try one small supported audio file before a large folder.
- Power down before inspecting or reseating the SD card. Back it up before
  filesystem repair or formatting.

### A new card does not play

- Leave admin mode: scans there are captured for the page, not played.
- Confirm the write completed on the same writable Type 2 card that was scanned.
- Check the saved card entry and that its referenced files still exist. A file
  extension alone does not guarantee a supported codec; start with a known-good
  MP3 or AAC-LC file.

### Bluetooth does not appear

- Hold the right knob for the full three seconds and check its indicator.
- Look for **Open Yoto** in audio-device pairing settings, not a BLE scanner.
- Stop local playback and any other Bluetooth source connection before retrying.
- Check UART for a Bluetooth initialization error; a successful firmware build
  is not evidence that Bluetooth works on every board.
