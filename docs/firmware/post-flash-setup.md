---
icon: lucide/wifi
---

# Post-flash setup

Open Yoto's administration page runs on the player. It does not use a Yoto
account, Yoto's servers, or an internet connection. You open it with a dedicated
NFC admin card, join the player's temporary Wi-Fi network, and manage the SD
card from your phone or computer.

## What you need

- A Yoto Mini running Open Yoto.
- One writable NFC Type 2 card reserved as the admin card.
- A phone or other NFC writer that can write an NDEF URI record.
- A phone, tablet, or computer with Wi-Fi.
- The provided [`index.html`](https://raw.githubusercontent.com/tensorfish/open-yoto/main/firmware/components/admin/html/index.html).

Save `index.html` as a file before joining the player hotspot. The hotspot has no
internet access, so download it first.

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

The code protects the local page even though the hotspot itself is open. A
successful login creates a session cookie in that browser. A fresh code is
created each time admin mode starts.

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
manage files, playback, and cards. The provided page is the supported reference
implementation.

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

1. Open the **Cards** area in the web UI.
2. Put a writable blank card on the player's NFC reader. During admin mode, the
   firmware captures the scanned card for the page instead of starting normal
   playback.
3. Choose the media folder or tracks the card should play.
4. Choose its artwork and track order.
5. Save the card entry, then use the page's write action while that same card is
   still on the reader.
6. Leave admin mode and scan the new card to test normal playback.

The library and card mappings stay on the player's SD card. Normal playback
reads them locally and does not sync with Yoto's services. If those services
become unavailable, Open Yoto's local media and cards continue to work.

Open Yoto does not copy paid or protected Yoto content. Only upload files you
own or have permission to use.

## 6. Leave admin mode

Place the admin card on the reader again. The player stops the hotspot and
returns to normal card playback. Do this when administration is finished; the
`openyoto` Wi-Fi network is intentionally temporary.

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
copy of the provided `index.html`.
