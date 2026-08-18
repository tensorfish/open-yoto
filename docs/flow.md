---
icon: lucide/git-branch
---

# Flow Charts

High-level data and control flow through the device.

## Boot

```mermaid
flowchart TD
  A[ROM bootloader] --> B[2nd-stage bootloader @ flash 0x1000]
  B --> C[partition table @ 0x8000]
  C --> D{active partition}
  D -->|factory| E[app @ 0x40000]
  D -->|ota_0 / ota_1| F[OTA slot]
  E --> G[call_start_cpu0 @ 0x400813a8]
  G --> H[FreeRTOS init]
  H --> I[app_main]
  I --> J[board init: GPIO, I2C, SPI, SD, NFC, display, audio]
```

## NFC card read → content

```mermaid
flowchart LR
  A[Yoto card<br/>NFC Type 2 tag] -->|13.56 MHz| B[CR95HF reader]
  B -->|UART or SPI| C[NFC HAL task]
  C --> D[REQA → anticol → select]
  D --> E[read NDEF TLV]
  E --> F[UID + URL<br/>e.g. https://yoto.io/id]
  F --> G[content lookup]
```

## Content resolution (SD vs stream)

```mermaid
flowchart TD
  A[NFC URL / UID] --> B{content on SD card?}
  B -->|yes| C["/sdcard/cards/&lt;card&gt;/&lt;chapter&gt;/&lt;track&gt;"]
  B -->|no| D[HTTP / HLS stream]
  C --> E[decode: MP3 / AAC / OPUS / OGG / WAV]
  D --> E
  E --> F[I2S → codec]
  F --> G[amp + speaker / headphone DAC]
```

## Audio pipeline (ESP-ADF)

```mermaid
flowchart LR
  A[source] --> B[ADF pipeline]
  B --> C[decode]
  C --> D[resample + EQ]
  D --> E[I2S]
  E --> F[codec]
  F --> G[output]
```

## Power

```mermaid
flowchart TD
  A[USB-C PD / Qi / battery] --> B[charger]
  B --> C[battery + fuel gauge]
  C --> D[power control IOX]
  D --> E[rails: Vout / Vin-hold / level-shifter]
  E --> F[SoC + peripherals]
```

## Hardware revisions

```mermaid
flowchart LR
  A[#01<br/>SPI SD + SPI NFC<br/>ES8388 + ADC] --> B[#00/#02<br/>SDMMC 1-bit<br/>aw881xx + ES8156]
  B --> C[#04/#05<br/>SDMMC 1/4-bit<br/>CW2215B + Qi + USB-C]
```
