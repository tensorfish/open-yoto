#!/usr/bin/env python3
"""Capture Yoto UART0 output without toggling BOOT or RESET.

Usage: python3 /tmp/yoto_capture.py [seconds]
"""
import sys
import termios
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200
LOG = "/tmp/yoto_scan.log"
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 20

# Do not pass PORT to Serial(): opening with pyserial's default DTR/RTS states
# can momentarily assert the ESP32's wired BOOT/RESET controls. Set both
# released before opening, disable modem flow control, and suppress HUPCL so
# close() cannot pulse RESET/EN.
ser = serial.Serial(port=None, baudrate=BAUD, timeout=0.2, rtscts=False, dsrdtr=False)
ser.dtr = False
ser.rts = False
ser.port = PORT
ser.open()
attrs = termios.tcgetattr(ser.fileno())
attrs[2] &= ~termios.HUPCL
termios.tcsetattr(ser.fileno(), termios.TCSANOW, attrs)

buf = bytearray()
try:
    end = time.monotonic() + SECS
    print(f"capturing {SECS}s to {LOG} ...", flush=True)
    while time.monotonic() < end:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
finally:
    ser.close()

with open(LOG, "wb") as log:
    log.write(buf)
print(f"\n--- saved {len(buf)} bytes to {LOG} ---", flush=True)
