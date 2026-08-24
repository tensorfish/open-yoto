#!/usr/bin/env python3
"""Put an ESP32 into ROM bootloader mode without changing UART settings.

Wiring used by this board:
  RTS -> BOOT/GPIO0 (asserted/low to select bootloader)
  DTR -> RESET/EN   (asserted/low to reset the chip)
"""
from __future__ import annotations

import argparse
import fcntl
import os
import struct
import termios
import time

DEFAULT_PORT = "/dev/ttyUSB0"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--hold", type=float, default=1.0, help="hold BOOT and RESET in seconds (default: 1)")
    parser.add_argument("--settle", type=float, default=1.0, help="wait after releasing RESET (default: 1)")
    args = parser.parse_args()
    if args.hold < 0 or args.settle < 0:
        parser.error("--hold and --settle must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    # Do not use pyserial here.  Opening a Serial object applies its default
    # 9600 8N1 termios configuration and DTR/RTS states to this shared TTY,
    # breaking esptool's active 460800 connection.  These ioctls only change
    # the requested modem-control bits.
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    set_bits = getattr(termios, "TIOCMBIS", 0x5416)
    clear_bits = getattr(termios, "TIOCMBIC", 0x5417)
    dtr = getattr(termios, "TIOCM_DTR", 0x002)
    rts = getattr(termios, "TIOCM_RTS", 0x004)

    def set_modem_bits(request: int, bits: int) -> None:
        fcntl.ioctl(fd, request, struct.pack("I", bits))

    try:
        set_modem_bits(clear_bits, dtr | rts)
        time.sleep(0.05)
        set_modem_bits(set_bits, rts)       # BOOT asserted
        set_modem_bits(set_bits, dtr)       # RESET asserted
        time.sleep(args.hold)
        set_modem_bits(clear_bits, dtr)     # RESET released while BOOT remains asserted
        time.sleep(args.settle)
        set_modem_bits(clear_bits, rts)     # BOOT released; ROM bootloader remains selected
        print(f"bootloader mode requested on {args.port}", flush=True)
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
