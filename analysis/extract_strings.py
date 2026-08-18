"""Extract and categorize strings from the factory app image.

Identifies which ESP-IDF peripherals/drivers are present (RFID, SD, display,
audio, power, etc.) — the backbone for port/component mapping.

Usage: uv run python analysis/extract_strings.py
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "output" / "factory.bin"

# Component fingerprint keywords -> category
CATEGORIES = {
    "rfid_nfc": [
        "rc522", "pn532", "pn5180", "mfrc", "mfrc522", "nfc", "rfid",
        "iso14443", "iso15693", "mifare", "ntag", "st25", "st25r", "cr95hf",
        "trf7970", "pcd", "picc", "felica", "iso18092", "ndef", "apdu", "p2p",
    ],
    "sd_card": [
        "sdmmc", "sdspi", "sdcard", "sd_card", "sdhost", "fatfs", "vfs_fat",
        "ff_", "diskio", "fat16", "fat32", "exfat", "msc", "sdc", "sd2",
    ],
    "display": [
        "ili9341", "ili9488", "st7735", "st7789", "gc9a01", "gc9107", "ssd1306",
        "ssd1351", "sh1106", "lcd", "tft", "display", "oled", "frame_buffer",
        "framebuffer", "lcd_panel", "backlight", "spi_master", "lvgl", "touch",
        "ft5x", "gt911", "cst816", "xpt2046", "touch_panel",
    ],
    "audio": [
        "i2s", "dac", "codec", "es8311", "es8388", "wm8960", "tlv320", "pcm",
        "audio", "amp", "amplifier", "tas", "max98357", "pdm", "volume", "eq",
        "playback", "sample_rate", "bit_depth",
    ],
    "power": [
        "battery", "bat_level", "adc", "charg", "bq2", "axp192", "axp20", "fuel",
        "power", "pmu", "voltage", "current", "vbus", "usb_charg", "deep_sleep",
        "light_sleep", "sleep", "wake",
    ],
    "wifi_bt": [
        "wifi", "net80211", "esp_wifi", "wlan", "ble", "bluetooth", "bt_", "btc_",
        "gap", "gatt", "esp_gatt", "esp_bt", "nimble", "bluedroid", "scan", "ssid",
        "wpa", "tcpip", "lwip", "mqtt", "http", "tls", "mbedtls", "socket", "mdns",
    ],
    "gpio": ["gpio", "gpio_config", "pin", "io_mux", "rtc_gpio"],
    "i2c": ["i2c", "i2c_master", "i2c_slave", "twai", "sda", "scl"],
    "spi": ["spi_bus", "spi_master", "spi_slave", "hspi", "vspi", "spi2", "spi3"],
    "uart": ["uart", "uart0", "uart1", "uart2", "rs232", "rs485", "console"],
    "led": ["led", "ledc", "pwm", "ws2812", "neopixel", "rgb", "led_strip"],
    "button": ["button", "button_", "key_", "encoder", "gpio_isr", "isr", "intr"],
    "crypto_ota": ["ota", "esp_ota", "esp_image", "sha256", "aes", "flash_enc", "secure_boot", "bootloader"],
}


def extract_strings(data: bytes, min_len: int = 4) -> list[str]:
    out = []
    cur = bytearray()
    for b in data:
        if 0x20 <= b < 0x7F or b in (0x09,):
            cur.append(b)
        else:
            if len(cur) >= min_len:
                out.append(cur.decode("ascii", errors="replace"))
            cur.clear()
    if len(cur) >= min_len:
        out.append(cur.decode("ascii", errors="replace"))
    return out


def main() -> None:
    data = APP.read_bytes()
    strings = extract_strings(data)
    seen = set()
    uniq = []
    for s in strings:
        if s not in seen:
            seen.add(s)
            uniq.append(s)

    print(f"total strings: {len(strings)}  unique: {len(uniq)}")

    # Save full unique list for later fan-out
    (ROOT / "output" / "strings.txt").write_text("\n".join(uniq))

    # Categorize
    hits: dict[str, list[str]] = {k: [] for k in CATEGORIES}
    for s in uniq:
        sl = s.lower()
        for cat, kws in CATEGORIES.items():
            if any(kw in sl for kw in kws):
                hits[cat].append(s)
                break  # first-match category only

    summary = {k: {"count": len(v), "sample": sorted(set(v))[:40]} for k, v in hits.items()}
    (ROOT / "output" / "strings_categorized.json").write_text(json.dumps(summary, indent=2))

    for cat, kws in CATEGORIES.items():
        found = hits[cat]
        print(f"\n=== {cat} ({len(found)}) ===")
        for s in sorted(set(found))[:30]:
            print(f"  {s!r}")

    print(f"\nWrote output/strings.txt ({len(uniq)} lines) and output/strings_categorized.json")


if __name__ == "__main__":
    main()
