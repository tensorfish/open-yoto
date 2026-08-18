---
icon: lucide/wifi
---

# Wi-Fi & Bluetooth

The ESP32's integrated 2.4 GHz radio provides both Wi-Fi and Bluetooth
(Bluedroid), giving the device cloud connectivity and a BT-audio speaker role.

## Wi-Fi

Standard ESP-IDF networking stack:

- **lwIP** TCP/IP (`esp_netif_lwip`, `LWIP_MAX_SOCKETS`)
- **mbedTLS** (`AES-128/256-*`, `tls`, `ssl`)
- Application protocols: **HTTP/HTTPS**, **HLS**, **MQTT**

Configuration and control endpoints expose the Wi-Fi lifecycle:

```text
/wifi-creds      # set/clear saved credentials
/ssids           # scan for nearby networks
/wifi-restart    # restart Wi-Fi
```

Provisioning is **SoftAP-based** (not BLE):

```text
SoftAP provisioning started with SSID '%s', Password '%s'
```

`nvs.net80211` holds the RF calibration (NVS key `cal_data`); the `phy_init`
partition stores initial calibration. App Wi-Fi credentials are stored
separately (`wifi_add_to_nvs`). The `wifi_init` log tag and `WiFiConfigMsgType`
confirm the ESP-IDF Wi-Fi task/IPC model.

## Cloud / MQTT

The device reports telemetry and receives commands over **MQTT**
(`APP_MQTTLOG_OFF` disables MQTT logging; `MQTT over serial enabled/disabled`
exposes a serial transport for diagnostics). Content metadata exchange uses
`contentListSha` and `contentVersion` (see [SD Card & Storage](storage.md)) to
decide what to download. Media is fetched over HTTPS
(`Media download URL %s`).

Concrete cloud endpoints reference `api.yotoplay.com` (e.g.
`https://api.yotoplay.com/device-v2/provision`).

## Bluetooth

The firmware is **classic-Bluetooth-only** (Bluedroid); there is no BLE/NimBLE
stack in the image (no `esp_gatt`/`nimble` strings). It acts as an AV remote
control **target** and **controller**:

```text
//opt/atlassian/pipelines/agent/build/components/ybluetooth/blue_service.c
BT_AV_TAG
AV Remote Control Target
AV Remote Control Controller
AVRC passthrough cmd: key_code 0x%x, key_state %d
BLUETOOTH_INIT
APP_BT_ENABLED
BT_MODE
BT_POST_BOOT
```

AVRC passthrough lets the device's rotary knobs send play/pause/skip/volume to
a paired phone (BT audio source), and the device can play BT-audio through the
same ESP-ADF pipeline.

## Security & coexistence

- **WPA3** support: `WIFI_AUTH_MODE__WPA3_PSK`, `WIFI_AUTH_MODE__WPA2_WPA3_PSK`.
- **SoftAP + STA** simultaneously: `Combined Soft Access Point and Wifi STA
  connected`.
- Wi-Fi/BT **coexistence** is managed by the coexist core
  (`coexist_core.c`, `coexist_arbit.c`,
  `Coexist!!! Wi-Fi station would only keep waked when available`).
