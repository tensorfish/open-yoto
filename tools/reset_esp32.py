#!/usr/bin/env python3
"""Reset a running ESP32 through the USB-UART modem-control lines.

Wiring used by this board:
  RTS -> BOOT/GPIO0 (released/high)
  DTR -> RESET/EN   (asserted/low during the reset pulse)
"""
from __future__ import annotations

import argparse
import time

DEFAULT_PORT = "/dev/ttyUSB0"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--hold", type=float, default=0.1, help="reset hold time in seconds (default: 0.1)")
    parser.add_argument("--settle", type=float, default=0.3, help="wait after reset in seconds (default: 0.3)")
    args = parser.parse_args()
    if args.hold < 0 or args.settle < 0:
        parser.error("--hold and --settle must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required; install it with `uv sync`") from exc

    port = serial.Serial(port=None, baudrate=115200, timeout=0)
    # Set released levels before opening so opening the device cannot hold the
    # target in reset or select the ROM bootloader.
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        port.rts = False  # BOOT released
        port.dtr = True   # RESET asserted
        time.sleep(args.hold)
        port.dtr = False  # RESET released; application boots
        time.sleep(args.settle)
        print(f"reset {args.port}")
    finally:
        port.dtr = False
        port.rts = False
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
