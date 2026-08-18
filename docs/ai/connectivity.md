---
icon: lucide/wifi
---

# Wi-Fi & Bluetooth — AI Detail Reference

Dense, evidence-backed reference for AI agents working the **connectivity**
subsystem of the Yoto ESP32 firmware. Every string is cited by `strings.txt`
line number; every code address is a real decompiled function with its size and
the string that pins it to this subsystem. All roles are `[INFERENCE]` unless
the function name is a resolved ESP-IDF symbol (it never is here — see caveat).

## 1. Scope & ground truth caveat

- **Radio**: the ESP32 **integrated 2.4 GHz transceiver** provides both Wi-Fi
  (802.11 b/g/n, STA + AP) and **classic Bluetooth** (Bluedroid). There are **no
  external GPIO/ADC/IOX pins** for Wi-Fi or BT in any `hwconfig_*.json` — the
  antenna, RF switch, PA and coex arbiter are all on-die. Do not look for
  Wi-Fi/BT pins in `output/pinmap.json`; they do not exist.
- **No BLE**: the image contains **no** `esp_gatt`/`nimble`/`esp_gap_ble`/`ble_`
  stack. Classic BT only (A2DP source + AVRC controller/target). The only GAP
  strings are classic-BT (`ESP_BT_GAP_PIN_REQ_EVT`, `bt_app_gap_cb`).
- **String→function xrefs are sparse**: Ghidra's generic Xtensa backend does not
  resolve `L32R` literal-pool references. Function names are auto-generated
  (`FUN_segN__ADDR`). The mapping below was recovered by locating each string's
  DROM address, finding the little-endian pointer to it in the seg4 literal-pool
  region, then grepping `output/decompiled/*.c` for that `DAT_seg4__XXXXXXXX`
  slot. This is **partial**: some strings are reached via two-level pointers or
  seg0 data and resolve to no function.

## 2. Component / pin table

| Component | Location | Pins | Evidence |
|---|---|---|---|
| Wi-Fi radio | ESP32 on-die | none (internal) | `ESP_ERR_WIFI_*` (strings.txt 5679–5707) |
| BT radio (classic) | ESP32 on-die | none (internal) | `BTDM_INIT` (6043), `Bluetooth MAC:` (6044) |
| Wi-Fi/BT coex arbiter | ESP32 on-die | none | `coexist_core.c` (4064), `coex_wifi_channel_get` (4065) |
| RF calibration | NVS `net80211` + `phy_init` partition | — | `phy_init`@`0x3b000` (layout.json) |
| Hostname (STA) | firmware string | — | `yoto-player` (2027), `yoto-mini` (2028) |
| Console/UART (diag) | UART0 (ROM) | — | `mqttuart` (3137), `MQTT over serial` (3121–3129) |

**Memory co-location (not pins):** `WiFi/LWIP prefer SPIRAM` (6027) and
`Release Bluetooth stack from internal ram?` (234) show the stacks can be
placed in PSRAM. `WiFi needs 80MHz APB frequency` (6029) / `wifi_apb80m_request`
(6030) — the Wi-Fi driver forces an 80 MHz APB clock.

**NVS keys** (strings 7828–7868): `APP_BT_ENABLED`, `BT_MODE`, `BT_POST_BOOT`,
`BT_TIMESTAMP`, `APP_BTHP_ON`, `APP_MQTTLOG_OFF`, `WIFI_DETAILS`. Default JSON
blobs at 7643–7736.

## 3. Segment / address map (for pointer chasing)

From `output/layout.json:66-101`:

| Seg | Load addr | Size | Role |
|---|---|---|---|
| 0 | `0x3F400020` | 680880 | DROM (rodata: all strings) |
| 3 | `0x40080000` | 9328 | IRAM startup (`call_start_cpu0`@`0x400813a8`) |
| 4 | `0x400D0020` | 1634508 | code + literal pools (`DAT_seg4__*`) |
| 5 | `0x40082470` | 98888 | code |
| 6 | `0x400C0000` | 100 | code |

Entry: `0x400813a8` (init_flow.json). Decompiled funcs are named
`FUN_segN__<ADDR>` and live in `output/decompiled/FUN_segN__<ADDR>_0x<ADDR>.c`;
their `addr/name/size/file` are in `output/decompiled_manifest.json`
(5915 entries).

## 4. Exact strings (ground truth)

Line numbers refer to `output/strings.txt`. Starred rows are the highest-value
identifiers. VAs are the string's DROM address (seg0); `slot` is the seg4
literal-pool word that points to it.

### 4.1 Wi-Fi core (`YOTO_WIFI` tag, `wifi_init`)

| Line | String |
|---|---|
| 2020 | `YOTO_WIFI` |
| 2084 | `wifi_init` |
| 2082 | `wifi_set_esp_default` |
| 2083 | `wifi_has_esp_default` |
| 2085 | `_get_ap_scan_retries` |
| 2143 | `WIFI_STA_DEF` |
| 1942 | `E (%lu) %s: SSID in NVS %s` |
| 1943 | `E (%lu) %s: Wi-Fi is not currently initialised` |
| 1944 | `E (%lu) %s: No esp wifi config is stored` |
| 1950 | `I (%lu) %s: WiFi is on` |
| 2024 | `I (%lu) %s: Wifi STA obtained an IP address:%d.%d.%d.%d` |
| 2031 | `I (%lu) %s: Yoto Player Wifi Station Started` |
| 2034 | `I (%lu) %s: Combined Soft Access Point and Wifi STA connected` |
| 2038 | `E (%lu) %s: Disconnect reason code: %d. rssi: %d` |
| 2041 | `I (%lu) %s: wifi_add_to_nvs() : SSID = %s` |
| 2045 | `I (%lu) %s: %s scan found %d networks` |
| 2050 | `E (%lu) %s: %s: esp_wifi_init() failed` |
| 2051 | `W (%lu) %s: esp_wifi_set_mode() failed` |
| 2052 | `W (%lu) %s: esp_wifi_start() failed` |
| 2054 | `I (%lu) %s: BSSID selection : %02X:…:%02X` |
| 2061/2070 | `W (%lu) %s: ---- WIFI Scan ----` / `[Done]` |
| 2066 | `I (%lu) %s: Do not init wifi. No wifi credentials` |
| 2067 | `E (%lu) %s: Could not read WiFi credentials from NVS` |
| 2069 | `I (%lu) %s: SSID %s` |
| 2073 | `I (%lu) %s: Removing WIFI settings from NVS` |
| 2029/2030 | `Soft Access Point Started` / `Stopped` |
| 2027/2028 | `yoto-player` / `yoto-mini` (hostnames) |
| 2032/2033 | `Managed to set YotoPlayer hostname` / `Could not set hostname` |
| 1383 | `_proc_wifi` |
| 1590/1595/1596 | `Wifi not connected` / `Currenty connected to wifi, disconnect.` / `Setting wifi details failed` |

### 4.2 SoftAP provisioning (`wifi_softap_prov`, `task_softap.c`)

| Line | String |
|---|---|
| 2138 | `wifi_softap` |
| 2155 | `wifi_softap_prov` |
| 2154 | `stop_prov` |
| 2219 | `task_softap` |
| 2231 | `start_softap_task` |
| 2233 | `…/components/yoto_wifi/task_softap.c` |
| 2206 | `WIFI_SOFTAP_HTTPD` |
| 2152 | `Yoto_%c%c%c` (SSID prefix template) |
| 2201 | `I (%lu) %s: SoftAP provisioning started with SSID '%s', Password '%s'` |
| 2200 | `I (%lu) %s: PoP is set to: %s` |
| 2218 | `/ssids` |
| 2208/2209 | `devicekeyid` / `deviceid` |
| 2216 | `{"fwVersion": "%s"}` |
| 2203/2204/2205 | `500 Server Error` / `200 OK` / `{"error": "Status get endpoint error"}` |
| 2157/2158 | `prov-config` / `prov-session` |
| 2166 | `saperr/popcode/security` |
| 2177 | `saperr/ap/disconnected/withWiFiIp` |
| 2188 | `saperr/sta/conn` |
| 2237 | `saperr/scan/initial` |
| 2238/2239/2240 | `Waiting credentials` / `softap/state` / `Applying credentials` |
| 2245 | `softap/complete` |
| 2243/2244 | `Starting MQTT + popcode registration` / `Completed popcode registration` |
| 2247 | `E (%lu) %s: POP CODE REGISTRATION FAILED` |
| 2248 | `MQTT or popcode failed` |
| 2249 | `Provided credentials cannot be used` |
| 2195 | `E (%lu) %s: Failed to create new protocomm instance` |
| 2196 | `E (%lu) %s: Failed to start protocomm HTTP server` |
| 2198/2199 | `Failed to set provisioning endpoint` / `Failed to set http endpoint` |
| 2184/2185 | `single phone softap flow detected` / `standard phone softap flow detected` |
| 2167/2168 | `STA Start` / `STA connected` |
| 2176 | `App device disconnected from our SoftAP network` |
| 2227–2230 | `softap/system/inter` / `softap/system/b8` / `softap/system/b32` / `softap/system/dma` |

Provisioning protobuf (ESP-IDF `wifi_provisioning`): `WiFiProvConfig` (6603),
`WiFiConfigPayload` (6619), `@WiFiConfigMsgType` (6612), `passphrase` (6626),
`sta_state` (6627), `fail_reason` (6628), `cmd_get_status`/`resp_get_status`/
`cmd_set_config`/`resp_apply_config` (6620–6625).

Protocomm security (Sec0/Sec1): `mbedtls_aes_crypt_ctr` (6412),
`mbedtls_ecp_group_load` (6424), `mbedtls_ecdh_gen_public` (6425),
`mbedtls_ecdh_compute_shared` (6429), `mbedtls_sha256_ret` (6430),
`mbedtls_gcm_setkey_enc` (6454), `Sec0MsgType`/`SEC0_MSG_TYPE__S0_*` (6455–6457).

### 4.3 MQTT (`MQTTS`, `mqtt_client`)

| Line | String |
|---|---|
| 2473 | `mqtts://data.iot.eu-west-2.amazonaws.com` |
| 2472 | `x-amzn-mqtt-ca` |
| 2464/2466 | `MQTTS` / `Initialising MQTTS` |
| 2465 | `mqtt_init() - Cannot initialise MQTT lock` |
| 2025/2026 | `(Re)initialising MQTT` / `Disconnecting MQTT` |
| 2221/2222 | `Could not connect to MQTT to mark device as online` / `Successfully connected to MQTT…` |
| 2486 | `command/#` (command subscription topic) |
| 2487 | `device/%s/%s` |
| 2490 | `status/full` |
| 2491 | `reportplay` |
| 2492 | `preloadedcontent` |
| 2523 | `log/device/%s/debug` |
| 2524 | `{ "level": "%s", "fwVersion":"%s", "message": "%s" }` |
| 1726 | `device/%s/events` |
| 1758 | `device/%s/progress` |
| 2493/2496/2497/2499/2500/2507 | `MQTT_EVENT_CONNECTED`/`DISCONNECTED`/`SUBSCRIBED`/`PUBLISHED`/`DATA`/`ERROR` |
| 2511/2517 | `MQTT_ERROR_TYPE_TCP_TRANSPORT` / `MQTT_ERROR_TYPE_CONNECTION_REFUSED` |
| 2525 | `_mqtt_cmd_unsubscribe_from_command_topic` |
| 2526 | `_mqtt_event_processor` |
| 2527 | `MQTTS_CMD` |
| 2551 | `_command_download() -> Received download MQTT cmd %s` |
| 2660 | `_command_refreshconfig() -> Received refreshconfig MQTT cmd` |
| 2747 | `mqtt_log_listener` |
| 2608 | `mqtt_shutdown` |
| 2456 | `mqttShutdown` (shutdown reason) |
| 3136/3137 | `mqttcmd` / `mqttuart` (serial MQTT transport) |
| 6513 | `mqtt_client` |
| 6526/6527 | `mqtt` / `mqtts` (schemes) |
| 6572 | `mqtt_task` |
| 6583–6594 | `deliver_publish` / `mqtt_process_receive` / `esp_mqtt_connect` / `esp_mqtt_client_set_uri` / `esp_mqtt_client_init` / `esp_mqtt_set_config` |
| 6595/6596 | `MQIsdp` / `MQTT` |

MQTT command endpoints (topic payloads): `/download` (2661), `/sd-format`
(2662), `/refresh-config` (2659), `/wifi-restart` (2679), `/rev-check` (2700).

### 4.4 HTTP client / cloud endpoints

| Line | String |
|---|---|
| 2120 | `https://api.yotoplay.com/device-v2/provision` |
| 2130 | `https://api.yotoplay.com/device-v2/provision/verify` |
| 2296 | `https://api.yotoplay.com/device-v2/popCode` |
| 2005 | `https://api.yotoplay.com/device-v2/version` |
| 2128 | `https://api.yotoplay.com/device-v2/status` |
| 2427 | `https://api.yotoplay.com/device-v2/config?shortcuts=true` |
| 1746 | `https://api.yotoplay.com/device-v2/recondition` |
| 1781 | `https://api.yotoplay.com/data/log/play` |
| 2313 | `https://sync.api.yotoplay.com/next?accel=true` |
| 2376 | `https://sync.api.yotoplay.com/recache/` |
| 2370 | `https://api.yotoplay.com/card/%s/media/%s?accel=true&nossl=true` |
| 8446 | `https://api.yotoplay.com/card/%s/linkUrl?uid=%02X…&originalUrl=%s` |
| 1388 | `https://card-content.yotoplay.com/yoto/` |
| 1604 | `https://listen-funkids.sharp-stream.com/funkids.aac?device=yoto` |
| 1447/1677 | `https://yoto.io` / `https://yoto.io/` |
| 8530 | `https://yoto.io/%s` |
| 2431–2433 | `.s3-accelerate.amazonaws.com` / `yotoplay.com` / `yoto.io` |
| 2086 | `Yoto v2 FW; version %s` (User-Agent payload) |
| 2087/2091 | `User-Agent` / `Content-type` |
| 1745/1749 | `Authorization` / `Bearer` (JWT auth) |
| 2388/2390/2392 | `application/vnd.yoto.card.raw+json` / `x-api-key` / `X-Yoto-Card-UID` |
| 3950 | `chunks.memfault.com` (Memfault telemetry) |

### 4.5 Bluetooth (classic / Bluedroid / A2DP sink)

| Line | String |
|---|---|
| 442 | `…/components/ybluetooth/blue_service.c` |
| 440 | `I (%lu) %s: Bluetooth service have been initialized` |
| 447/448 | `initialize bluedroid failed (0x%04x)` / `enable bluedroid failed` |
| 445/446 | `initialize controller failed` / `enable controller failed` |
| 3305 | `BLUETOOTH_INIT` |
| 3306 | `Bluetooth is not enabled for this Yoto Player` |
| 3307 | `Bluetooth is enabled for this Yoto Player - Restarting in Bluetooth sink Mode` |
| 3312 | `Starting up in Bluetooth Sink Mode` |
| 3321 | `Bluetooth Sink Mode` |
| 6043/6044 | `BTDM_INIT` / `Bluetooth MAC: %02x:…` |
| 6085 | `bt_config.conf` |
| 317/318 | `btHndl_create_audio_periph` / `[5.2] Start Bluetooth peripheral` |
| 354 | `btHndl_setvolume` |
| 465–475 | `blue_service_check_update_sbc` / `create_stream` / `create_alc` / `destroy` / `start` / `bt_avrc_ct_cb` / `bt_avrc_tg_cb` / `bt_a2d_source_cb` / `bt_app_gap_cb` |
| 392 | `A2DP connection state: %s, [%02x:…]` |
| 393/394 | `A2DP connection state = CONNECTED` / `DISCONNECTED` |
| 397 | `Bluetooth configured, sample rate=%d` |
| 404–414 | `a2dp connecting to us` / `connected to us` / `media ready` / `START`/`SUSPEND`/`STOP` |
| 429 | `a2dp connecting to peer: %s` |
| 435 | `ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d` |
| 783/784 | `PLAY_BLUETOOTH` / `bluetoothtask` |
| 789 | `= ^_^ Welcome to Yoto Sink Mode ^_^ =` |
| 792 | `[Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->[codec_chip]` |
| 801–804 | `PERIPH_BLUETOOTH_CONNECTED` / `DISCONNECTED` / `AUDIO_STARTED` / `AUDIO_SUSPENDED/STOPPED` |
| 1363/1364 | `BT service is not starting because no devices were paired` / `Timeout for bluetooth activation…` |
| 1914 | `BT_AV_TAG` |
| 1932/1933 | `The bluetooth is disabled` / `bluetooth a2d source feature is disabled` |
| 2598/2720 | `bluetoothHp` / `bt_speaker` |

### 4.6 WPA3 / auth modes

| Line | String |
|---|---|
| 4211 | `Open Auth` |
| 4212 | `WPA-PSK` |
| 4213 | `WPA2-ENT` |
| 4214 | `WPA2-PSK` |
| 4215 | `WPA2-CCKM` |
| 4216 | `WPA3-SAE` |
| 4217 | `WAPI-PSK` |
| 4219 | `WPA3-ENT-192` |
| 4220 | `WPA2-PSK-FT` |
| 4221 | `WPA3-OWE` |
| 6631–6639 | `WifiAuthMode` enum: `WIFI_AUTH_MODE__Open/WEP/WPA_PSK/WPA2_PSK/WPA_WPA2_PSK/WPA2_ENTERPRISE/**WPA3_PSK**/**WPA2_WPA3_PSK**` |
| 4115–4122 | `sta.ssid` / `sta.authmode` / `sta.pswd` / `sta.pmk` / `sta.bssid` |
| 4137–4140 | `ap.ssid` / `ap.passwd` / `ap.pmk` / `ap.chan` |
| 6640–6642 | `WifiConnectFailedReason`: `AuthError` / `NetworkNotFound` |

### 4.7 Wi-Fi/BT coexistence

| Line | String |
|---|---|
| 4064/4066 | `coexist_core.c` / `coexist_arbit.c` |
| 4065 | `coex_wifi_channel_get` |
| 4070–4075 | `Invalid WiFi esp_wifi_*.h md5, internal: %s, idf: %s` (adapter ABI checks) |
| 11071 | `…default passive scan time parameter for WiFi scan when Bluetooth is enabled!!!!!!` |
| 11072 | `…default active scan time parameter for WiFi scan when Bluetooth is enabled!!!!!!` |

### 4.8 lwIP / mbedTLS / HLS

| Line | String |
|---|---|
| 5982/5983 | `lwip_arch` / `sys_init: failed to init lwip protect mutex` |
| 5988/5991/6005 | `DHCP server assigned IP…` / `DHCP server cannot be started` / `DHCP server started on interface %s…` |
| 5995 | `dhcp client start failed` |
| 6010 | `esp_netif_new_api` |
| 6027 | `WiFi/LWIP prefer SPIRAM` |
| 6038 | `wifi_netif` |
| 6021–6026 | `tcpip mbox` / `udp mbox` / `tcp mbox` / `tcp mss` |
| 6109/6134 | `esp-tls` / `esp-tls-mbedtls` |
| 6138/6139 | `Failed to verify peer certificate!` / `Certificate verified.` |
| 6140 | `mbedtls_ssl_handshake returned -0x%04X` |
| 3806/3832 | `-----BEGIN CERTIFICATE-----` / `-----END CERTIFICATE-----` (embedded CA cert) |
| 5797–5799 | `ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED` / `CONF_PSK_FAILED` / `TICKET_SETUP_FAILED` |
| 8283 | `.m3u` |
| 8288/8289 | `Hls do not have key url` / `No memory for hls key` |
| 8299 | `Fail to decrypt aes ret %d` |
| 8306 | `HLS_PLAYLIST` |
| 8308 | `…/lib/hls/hls_playlist.c` |
| 8311 | `hls_main_tag_cb` |
| 8335 | `#EXTINF:` |
| 8336 | `HLS_PARSER` |
| 8338–8340 | `AUTOSELECT` / `BANDWIDTH` / `CODECS` |

## 5. Function-address list

`FUN_seg4__<hex>` names; address column is `0x<hex>`. `size` in bytes from
`output/decompiled_manifest.json`. "Pinned by" = the string (rodata VA → seg4
literal slot) that the function references; this is the objective link. Role is
`[INFERENCE]`.

### 5.1 Wi-Fi core — `YOTO_WIFI` (slot `0x400D34E8`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400f8734` | 114 | YOTO_WIFI | `wifi_init` entry (see `wifi_init` str 2084) |
| `0x400f87c4` | 93 | YOTO_WIFI | wifi init helper |
| `0x400f8ab4` | 180 | YOTO_WIFI | wifi deinit/stop |
| `0x400f8b9c` | 235 | YOTO_WIFI | wifi scan trigger (called from softap httpd) |
| `0x400f8d24` | 398 | YOTO_WIFI | wifi event handler |
| `0x400f8ebc` | 351 | YOTO_WIFI | STA connect (`wifi_join`) |
| `0x400f901c` | 374 | YOTO_WIFI | NVS credential load/save |
| `0x400f9194` | 104 | YOTO_WIFI | esp_default cred helper |
| `0x400f91fc` | 549 | YOTO_WIFI | STA/IP event processing |
| `0x400f9424` | 680 | YOTO_WIFI | wifi mode/config applier |
| `0x400f96d8` | 102 | YOTO_WIFI | RSSI/scan compare |
| `0x400f97e0` | 457 | YOTO_WIFI | scan result → connect selection |
| `0x400f99e0` | 127 | YOTO_WIFI | AP scan retries |

### 5.2 SoftAP provisioning — `wifi_softap_prov` (slot `0x400D3888`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400fa9cc` | 292 | `Yoto_%c%c%c` (0x400D3870) | build SoftAP SSID from MAC |
| `0x400fab14` | 194 | wifi_softap_prov | prov state enter |
| `0x400fac08` | 398 | wifi_softap_prov | prov state machine step |
| `0x400faddc` | 702 | wifi_softap_prov | provisioning main state machine |
| `0x400fb0f8` | 213 | wifi_softap_prov | apply received creds |
| `0x400fb1d0` | 241 | wifi_softap_prov | prov stop/cleanup |
| `0x400fb2c4` | 481 | wifi_softap_prov | protocomm endpoint setup |

### 5.3 SoftAP HTTP server — `WIFI_SOFTAP_HTTPD` (slot `0x400D39D4`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400fba00` | 48 | WIFI_SOFTAP_HTTPD | httpd task trampoline |
| `0x400fb550` | 911 | WIFI_SOFTAP_HTTPD | `/ssids` handler (iterates 0x54-byte scan records) |

### 5.4 HTTP server / control endpoints (`/wifi-creds`, `/wifi-restart`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x40101638` | 7408 | `/wifi-creds` (0x400D4710), `/wifi-restart` (0x400D4684) | local HTTP server + URI dispatch (console/control) |

### 5.5 Wi-Fi details / netif

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x401ccf04` | 2077 | `WIFI_DETAILS` (0x401C6DB0) | build wifi-detail JSON (SSID/RSSI/MAC) |
| `0x401914cc` | 56 | `wifi_netif` (0x401884E4) | netif config |
| `0x4019151c` | 162 | `wifi_netif` | netif create/attach |

### 5.6 MQTT — `mqtts://…amazonaws.com` (slot `0x400D40C8`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400ff674` | 571 | `mqtts://…` + `x-amzn-mqtt-ca` | MQTT credential/config init (AWS IoT endpoint) |
| `0x400f3b68` | 48 | `device/%s/events` (0x400D2CAC) | event publisher |
| `0x400ffa58` | 312 | `command/#` (0x400D4138) | command-topic subscribe |
| `0x400ffc98` | 1160 | `command/#` | command JSON processor/dispatch |
| `0x4010012c` | 96 | `log/device/%s/debug` (0x400D4200) | debug-log publisher |

### 5.7 HTTP client — yotoplay endpoints

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400f8424` | 371 | `device-v2/version` (0x400D34A4) | OTA version check |
| `0x400fc438` | 288 | `device-v2/popCode` (0x400D3B74) | popCode register |
| `0x400fc658` | 227 | `device-v2/popCode` | popCode register (helper) |
| `0x400f3c78` | 277 | `device-v2/recondition` (0x400D2CF4) | recondition POST |
| `0x400fa450` | 201 | `device-v2/provision/verify` (0x400D37E4) | provision verify |
| `0x400feb14` | 714 | `device-v2/config?shortcuts=true` (0x400D3F34) | app-config refresh |
| `0x400fdc34` | 83 | `sync…/recache/` (0x400D3DE0) | recache trigger |
| `0x400fda9c` | 180 | `card/…/media/…` (0x400D3DC4) | media download URL |
| `0x401dc508` | 216 | `card/…/linkUrl` (0x401C7E48) | card link resolution |
| `0x400f4524` | 235 | `data/log/play` (0x400D2E08) | play-log POST |
| `0x400ecea4` | 184 | `card-content.yotoplay.com` (0x400D23F0) | card content fetch |
| `0x400ecf60` | 218 | `card-content.yotoplay.com` | card content fetch (helper) |
| `0x400eeab8` | 836 | `yoto.io` (0x400D262C) | yoto.io link/redirect |
| `0x400efc58` | 187 | `yoto.io` | yoto.io helper |
| `0x400f1df8` | 1987 | `listen-funkids…funkids.aac` (0x400D29C8) | internet-radio live stream |

### 5.8 Bluetooth — `blue_service.c` (slot `0x400D0CB4`)

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x400dd978` | 981 | `blue_service.c` | `blue_service_start` (controller/Bluedroid enable) |
| `0x400dde88` | 179 | `blue_service.c` | blue_service helper |
| `0x400ddf3c` | 196 | `blue_service.c` | A2DP source setup |
| `0x400ddd58` | 194 | `blue_service.c` | AVRC/A2DP event cb dispatch |
| `0x400dbe0c` | 197 | `btHndl_create_audio_periph` (0x400D0998) | create BT audio peripheral |
| `0x40112098` | 100 | `BLUETOOTH_INIT` (0x4010E914) | gate + launch BT sink-mode task |

### 5.9 esp-tls / mbedTLS — `esp-tls-mbedtls` (slot `0x401893E4`)

| Address | Size | Likely role [INFERENCE] |
|---|---|---|
| `0x401bee80` | 47 | tls session open |
| `0x401beda0` | 88 | tls session close |
| `0x401bedfc` | 128 | tls read/write |
| `0x401becbc` | 95 | tls hostname/config |
| `0x401bef38` | 956 | tls handshake (mbedtls) |
| `0x401bf304` | 324 | cert verify / drbg seed |

### 5.10 coexist — `coexist_core.c` (slot `0x4011DFE8`)

| Address | Size | Likely role [INFERENCE] |
|---|---|---|
| `0x4011e00c` | 227 | coexist arbitration init |
| `0x4011e104` | 156 | coex channel/priority helper |

### 5.11 HLS — `HLS_PLAYLIST`/`.m3u`/`HLS_PARSER`

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x401d67c0` | 192 | `HLS_PLAYLIST` (0x401C786C) | playlist element |
| `0x401d5bec` | 1238 | `.m3u` (0x401C77C0) | m3u8 tag parser |
| `0x401d7b68` | 1279 | `HLS_PARSER` (0x401C7938) | HLS attribute parser |

### 5.12 lwIP / esp_netif — `esp_netif_lwip`/`lwip_arch`

| Address | Size | Pinned by | Likely role [INFERENCE] |
|---|---|---|---|
| `0x4018a8b0` | 61 | `lwip_arch` (0x40188070) | lwip protect mutex |
| `0x4018a92c` | 87 | `lwip_arch` | lwip arch init |
| `0x4018e7ac` | 126 | `esp_netif_lwip` (0x40188230) | netif lwip init |
| `0x4018ebc0` | 143 | `esp_netif_lwip` | netif input |
| `0x4018ed00` | 647 | `esp_netif_lwip` | netif DHCP client/server |
| `0x4018e994` | 392 | `esp_netif_lwip` | netif attach driver |
| `0x4018f22c` | 208 | `esp_netif_lwip` | netif transmit |
| `0x4018f344` | 61 | `esp_netif_lwip` | netif lwip input fn |

## 6. Protocol & flow detail

### 6.1 Boot → Wi-Fi STA

1. `wifi_init` (strings 2084) — `esp_wifi_init()`; failures logged at 2050–2052.
2. Load credentials from NVS; on absence: `Do not init wifi. No wifi
   credentials` (2066) → `Trying esp default config` (2068).
3. `esp_wifi_set_mode()` / `esp_wifi_start()`; STA gets IP (`Wifi STA obtained
   an IP address`, 2024). Hostname set to `yoto-player`/`yoto-mini` (2032).
4. DHCP client via lwIP (`dhcp client start failed`, 5995); AP+STA dual mode
   reported at 2034.
5. Disconnect reasons/background RSSI scan (2038, 2045–2049).

### 6.2 SoftAP provisioning flow (no BLE)

1. Enter SoftAP mode (`SoftAp Triggered`, 2234; `start_softap_task`, 2231).
2. SSID = `Yoto_%c%c%c` (2152) — last 3 MAC octets; password generated
   (`SoftAP provisioning started with SSID '%s', Password '%s'`, 2201).
3. ESP-IDF `protocomm` HTTP server (2195–2196) with `WIFI_SOFTAP_HTTPD` tag
   (2206); endpoints `/ssids` (2218), `prov-config` (2157), `prov-session`
   (2158). Security = protocomm Sec1 (ECDH + AES-CTR, 6424/6425/6412).
4. Phone app joins SoftAP, posts Wi-Fi creds (protobuf `WiFiConfigPayload`,
   6619; `passphrase` 6626). `WiFi Credentials Received for SSID %s` (2140).
5. Creds applied (`WiFi Credentials Applied`, 2146), saved to NVS
   (`wifi_add_to_nvs`, 2041; `Saved in the NVS`, 2241).
6. STA connect (`STA Start`/`STA connected`, 2167–2168), then
   `Starting MQTT + popcode registration` (2243) → PoP code register
   (`device-v2/popCode`, 2296). `softap/complete` (2245).
7. Single-phone vs standard flow distinction (2184/2185); PoP ("Proof of
   Presence"/popcode) gate: `softap_popcode_required` (303).

### 6.3 MQTT (AWS IoT, TLS)

- Broker: `mqtts://data.iot.eu-west-2.amazonaws.com` (2473), custom CA
  `x-amzn-mqtt-ca` (2472), MQTT-over-TLS via `mqtt_client`/`mqtt_task`.
- Subscribe: `command/#` (2486). Publish: `device/%s/events` (1726),
  `device/%s/progress` (1758), `log/device/%s/debug` (2523), `device/%s/%s`
  (2487).
- Command dispatch: `_command_download` (2551), `_command_refreshconfig` (2660);
  topics `/download`, `/sd-format`, `/refresh-config`, `/wifi-restart`,
  `/rev-check`.
- Serial fallback: `mqttuart` (3137) — "MQTT over serial".

### 6.4 HTTP client (esp-tls + mbedTLS)

- Auth: JWT `Bearer` (1749) + `Authorization` (1745); card API uses `x-api-key`
  (2390) + `X-Yoto-Card-UID` (2392).
- TLS: `esp-tls-mbedtls` (6134); embedded CA cert (3806–3832); cert-bundle
  option referenced (6532, 6145). Errors `ESP_ERR_MBEDTLS_*` (5797–5799).
- Media/HLS: `http_stream.c` (8282) + `hls_playlist.c` (8308); AES-128 segment
  decryption (`Fail to decrypt aes`, 8299; `AES-128-*` cipher list 514–538).

### 6.5 Bluetooth sink (A2DP source)

1. `BLUETOOTH_INIT` gate (3305) checks `APP_BT_ENABLED`; if enabled restart into
   `Bluetooth Sink Mode` (3307).
2. `blue_service_start` enables Bluedroid (`initialize/enable bluedroid`, 447/448)
   and controller (445/446).
3. A2DP **source** (`bt_a2d_source_cb`, 473) + AVRC controller/target
   (`bt_avrc_ct_cb`/`bt_avrc_tg_cb`, 469/472). GAP callback `bt_app_gap_cb` (474).
4. Audio pipeline: `[Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->codec`
   (792); `PERIPH_BLUETOOTH_*` events (801–804). SBC config via
   `blue_service_check_update_sbc` (465).

### 6.6 Coexistence

`coexist_core.c`/`coexist_arbit.c` arbitrate the shared 2.4 GHz radio; Wi-Fi
scan params are relaxed when BT is enabled (11071–11072). ABI integrity checks
against `esp_wifi_*.h` MD5s (4070–4075).

## 7. Raw file pointers

| Artifact | Path | What to grep |
|---|---|---|
| All strings | `output/strings.txt` | line numbers in §4 |
| Categorized strings | `output/strings_categorized.json` | `wifi_bt` (870), `crypto_ota` (102) |
| Decompiled funcs | `output/decompiled/FUN_seg4__<ADDR>_0x<ADDR>.c` | §5 addresses |
| Func manifest | `output/decompiled_manifest.json` | `addr`/`size`/`file` (5915) |
| Func list (raw) | `output/ghidra_functions.json` | 9185 `addr`/`name`/`size` |
| Partition/segment map | `output/layout.json` | §3 |
| Pin map | `output/pinmap.json` | confirms **no** Wi-Fi/BT pins |
| Hardware configs | `output/hwconfig_00..05_*.json` | no Wi-Fi/BT fields |
| Boot entry | `output/decompiled/init_flow.json` | `0x400813a8` |
| Sibling summary | `docs/docs/subsystems/connectivity.md` | human-facing overview |

## 8. Open gaps

- String xrefs for `device-v2/provision` (`0x3F413BD2`), `device-v2/status`
  (`0x3F413CF9`), `sync…/next` (`0x3F41540F`), `hostname` strings, and
  `coex_wifi_channel_get` did not resolve to a decompiled function via the
  literal-pool scan — these are likely reached through two-level pointers or
  seg0 data. Re-derive with a full `L32R` resolver if exact callers are needed.
- All roles in §5 are inference from string proximity; confirm by reading the
  referenced `FUN_seg4__*.c` bodies.
