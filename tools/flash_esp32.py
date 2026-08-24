#!/usr/bin/env python3
"""Build and flash an ESP32 with manual BOOT/RESET control.

The default USB-UART wiring is RTS -> BOOT/GPIO0 and DTR -> RESET/EN.  Both
signals are active-low on the usual adapter circuit.  The workflow is:

1. run ``idf.py build``;
2. run ``tools/bootloader_esp32.py`` to enter ROM download mode;
3. run ``idf.py -p PORT flash`` with ESP-IDF reset control disabled.

The default invocation is intentionally small:

    python tools/flash_esp32.py

Use ``--dry-run`` to print the sequence and command without opening a serial
port or running ESP-IDF.
"""
from __future__ import annotations

import argparse
import os
import selectors
import termios
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any, Callable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FIRMWARE_DIR = ROOT / "firmware"
DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_TIMEOUT = 30.0
DEFAULT_DELAY = 1.0


def _non_negative(value: str) -> float:
    try:
        result = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if result < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return result


def _timeout(value: str) -> float:
    try:
        result = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if result <= 0 or result > DEFAULT_TIMEOUT:
        raise argparse.ArgumentTypeError("must be greater than 0 and no more than 30 seconds")
    return result


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"USB-UART port (default: {DEFAULT_PORT})")
    parser.add_argument(
        "--firmware-dir",
        type=Path,
        default=DEFAULT_FIRMWARE_DIR,
        help=f"directory in which idf.py runs (default: {DEFAULT_FIRMWARE_DIR})",
    )
    parser.add_argument("--idf", default="idf.py", help="idf.py executable or PATH name")
    parser.add_argument(
        "--boot-line",
        choices=("dtr", "rts"),
        default="rts",
        help="USB-UART modem line wired to BOOT/GPIO0 (default: rts)",
    )
    parser.add_argument(
        "--reset-line",
        choices=("dtr", "rts"),
        default="dtr",
        help="USB-UART modem line wired to RESET/EN (default: dtr)",
    )
    parser.add_argument(
        "--boot-asserted",
        choices=("low", "high"),
        default="low",
        help="physical level that asserts BOOT/GPIO0 (default: low)",
    )
    parser.add_argument(
        "--reset-asserted",
        choices=("low", "high"),
        default="low",
        help="physical level that asserts RESET/EN (default: low)",
    )
    parser.add_argument(
        "--delay",
        type=_non_negative,
        default=DEFAULT_DELAY,
        help="seconds for each required interlock wait (default: 1)",
    )
    parser.add_argument(
        "--capture-seconds",
        type=_non_negative,
        default=30.0,
        help="seconds of UART output to capture after flashing (default: 30)",
    )
    parser.add_argument(
        "--log",
        type=Path,
        default=Path("/tmp/yoto_scan.log"),
        help="UART log path (default: /tmp/yoto_scan.log)",
    )
    parser.add_argument(
        "--timeout",
        type=_timeout,
        default=DEFAULT_TIMEOUT,
        help="maximum seconds to wait for idf.py (default: 30; maximum: 30)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the sequence and flash command without hardware or subprocesses",
    )
    args = parser.parse_args(argv)
    if args.boot_line == args.reset_line:
        parser.error("--boot-line and --reset-line must name different modem lines")
    return args


def asserted_value(asserted_level: str) -> bool:
    """Return the pyserial modem boolean for an asserted physical level.

    USB-UART control lines are conventionally active-low: pyserial ``True``
    drives the signal low.  Supporting the opposite polarity keeps the
    sequence independent of adapter/inverter wiring.
    """

    return asserted_level == "low"


def set_line(serial_port: Any, line: str, value: bool) -> None:
    setattr(serial_port, line, value)


def run_boot_sequence(
    serial_port: Any,
    *,
    boot_line: str,
    reset_line: str,
    boot_asserted: str = "low",
    reset_asserted: str = "low",
    delay: float = DEFAULT_DELAY,
    sleep: Callable[[float], None] = time.sleep,
    announce: Callable[[str], None] = print,
) -> None:
    """Perform the required BOOT/RESET sequence in its exact order."""

    boot_on = asserted_value(boot_asserted)
    reset_on = asserted_value(reset_asserted)
    boot_off = not boot_on
    reset_off = not reset_on

    set_line(serial_port, boot_line, boot_on)
    announce(f"assert {boot_line.upper()} (BOOT/GPIO0)")
    set_line(serial_port, reset_line, reset_on)
    announce(f"assert {reset_line.upper()} (RESET/EN)")
    sleep(delay)
    set_line(serial_port, reset_line, reset_off)
    announce(f"release {reset_line.upper()} (RESET/EN)")
    sleep(delay)
    set_line(serial_port, boot_line, boot_off)
    announce(f"release {boot_line.upper()} (BOOT/GPIO0)")


def release_lines(serial_port: Any, *, boot_line: str, reset_line: str,
                  boot_asserted: str, reset_asserted: str) -> None:
    """Release both controls, including when flashing or sequencing fails."""

    set_line(serial_port, boot_line, not asserted_value(boot_asserted))
    set_line(serial_port, reset_line, not asserted_value(reset_asserted))

def reset_application(
    serial_port: Any,
    *,
    boot_line: str,
    reset_line: str,
    boot_asserted: str,
    reset_asserted: str,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Pulse RESET/EN with BOOT released."""

    set_line(serial_port, boot_line, not asserted_value(boot_asserted))
    set_line(serial_port, reset_line, asserted_value(reset_asserted))
    sleep(0.1)
    set_line(serial_port, reset_line, not asserted_value(reset_asserted))
    sleep(0.3)

def reboot_and_capture(
    serial_port: Any,
    *,
    boot_line: str,
    reset_line: str,
    boot_asserted: str,
    reset_asserted: str,
    seconds: float,
    log_path: Path,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Reset into the application and save its UART output."""

    reset_application(
        serial_port,
        boot_line=boot_line,
        reset_line=reset_line,
        boot_asserted=boot_asserted,
        reset_asserted=reset_asserted,
        sleep=sleep,
    )

    raw = bytearray()
    deadline = time.monotonic() + seconds
    print(f"capturing {seconds:g}s to {log_path} ...", flush=True)
    while time.monotonic() < deadline:
        chunk = serial_port.read(4096)
        if chunk:
            raw.extend(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(raw)
    print(f"\n--- saved {len(raw)} bytes to {log_path} ---", flush=True)


def _serial_factory(port: str) -> Any:
    try:
        import serial
    except ImportError as exc:  # pragma: no cover - depends on host environment
        raise RuntimeError("pyserial is required; install it with `python -m pip install pyserial`") from exc

    serial_port = serial.Serial(port=None, baudrate=115200, timeout=0, rtscts=False, dsrdtr=False)
    # Set released values before opening: some drivers apply DTR/RTS as they
    # open the device, and must not accidentally hold the ESP32 in reset.
    serial_port.dtr = False
    serial_port.rts = False
    serial_port.port = port
    try:
        serial_port.open()
        attrs = termios.tcgetattr(serial_port.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(serial_port.fileno(), termios.TCSANOW, attrs)
    except Exception:
        serial_port.close()
        raise
    return serial_port


def flash(args: argparse.Namespace, *, serial_factory: Callable[[str], Any] = _serial_factory,
          run: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
          sleep: Callable[[float], None] = time.sleep) -> int:
    """Build, wait for esptool to connect, then manually enter bootloader."""

    build_command = [args.idf, "build"]
    flash_command = [args.idf, "-p", args.port, "flash"]
    bootloader_command = [
        sys.executable,
        str(ROOT / "tools" / "bootloader_esp32.py"),
        "--port",
        args.port,
        "--hold",
        str(args.delay),
        "--settle",
        str(args.delay),
    ]
    if args.dry_run:
        print("would run:", *build_command)
        print("would start:", *flash_command)
        print("would wait for esptool Connecting...")
        print("would run:", *bootloader_command)
        print(f"would wait up to {args.timeout:g}s for flash")
        print(f"would reboot and capture {args.capture_seconds:g}s to {args.log}")
        return 0

    serial_port = None
    flash_process = None
    try:
        print("running:", *build_command, flush=True)
        try:
            built = run(build_command, cwd=args.firmware_dir, check=False)
        except FileNotFoundError:
            print(f"idf.py executable not found: {args.idf}", file=sys.stderr)
            return 127
        if built.returncode:
            print(f"idf.py build exited with status {built.returncode}", file=sys.stderr)
            return built.returncode

        print("starting:", *flash_command, flush=True)
        flash_env = os.environ.copy()
        # The manual sequence takes two seconds.  Continue esptool sync
        # attempts until it completes rather than letting the seven-attempt
        # default expire while RESET is held low.
        flash_env.setdefault("ESPTOOL_CONNECT_ATTEMPTS", "0")
        flash_env.setdefault("PYTHONUNBUFFERED", "1")
        try:
            flash_process = subprocess.Popen(
                flash_command,
                cwd=args.firmware_dir,
                env=flash_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=0,
            )
        except FileNotFoundError:
            print(f"idf.py executable not found: {args.idf}", file=sys.stderr)
            return 127
        assert flash_process.stdout is not None
        fd = flash_process.stdout.fileno()
        os.set_blocking(fd, False)
        selector = selectors.DefaultSelector()
        selector.register(fd, selectors.EVENT_READ)
        deadline = time.monotonic() + args.timeout
        recent = ""
        connecting = False
        try:
            while not connecting:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                events = selector.select(remaining)
                if not events:
                    break
                for _key, _mask in events:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        continue
                    text = chunk.decode("utf-8", "replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    recent = (recent + text)[-256:]
                    if (
                        "WARNING: Pre-connection option" in recent
                        or "Connecting..." in recent
                    ):
                        connecting = True
                        break
        finally:
            selector.close()

        if not connecting:
            if flash_process.poll() is None:
                flash_process.kill()
            flash_process.wait()
            print("idf.py did not reach esptool Connecting state", file=sys.stderr)
            return flash_process.returncode or 1

        print("\nrunning:", *bootloader_command, flush=True)
        entered = subprocess.run(bootloader_command, cwd=ROOT, check=False)
        if entered.returncode:
            print(
                f"bootloader helper exited with status {entered.returncode}",
                file=sys.stderr,
            )
            flash_process.kill()
            flash_process.wait()
            return entered.returncode

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            flash_process.kill()
            flash_process.wait()
            print(f"idf.py flash timed out after {args.timeout:g}s", file=sys.stderr)
            return 124
        try:
            remainder, _ = flash_process.communicate(timeout=remaining)
        except subprocess.TimeoutExpired:
            flash_process.kill()
            remainder, _ = flash_process.communicate()
            if remainder:
                sys.stdout.write(remainder.decode("utf-8", "replace"))
            print(f"idf.py flash timed out after {args.timeout:g}s", file=sys.stderr)
            return 124
        if remainder:
            sys.stdout.write(remainder.decode("utf-8", "replace"))
            sys.stdout.flush()
        if flash_process.returncode:
            print(f"idf.py flash exited with status {flash_process.returncode}", file=sys.stderr)
            return flash_process.returncode

        serial_port = serial_factory(args.port)
        # The CR95HF consistently synchronizes after a second application
        # reset following esptool's bootloader session.
        reset_application(
            serial_port,
            boot_line=args.boot_line,
            reset_line=args.reset_line,
            boot_asserted=args.boot_asserted,
            reset_asserted=args.reset_asserted,
            sleep=sleep,
        )
        reboot_and_capture(
            serial_port,
            boot_line=args.boot_line,
            reset_line=args.reset_line,
            boot_asserted=args.boot_asserted,
            reset_asserted=args.reset_asserted,
            seconds=args.capture_seconds,
            log_path=args.log,
            sleep=sleep,
        )
        return 0
    except Exception as exc:
        if flash_process is not None and flash_process.poll() is None:
            flash_process.kill()
            flash_process.wait()
        print(f"flash setup failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if serial_port is not None:
            try:
                release_lines(
                    serial_port,
                    boot_line=args.boot_line,
                    reset_line=args.reset_line,
                    boot_asserted=args.boot_asserted,
                    reset_asserted=args.reset_asserted,
                )
            except Exception as exc:
                print(f"warning: could not release BOOT/RESET lines: {exc}", file=sys.stderr)
            try:
                serial_port.close()
            except Exception as exc:
                print(f"warning: could not close serial port: {exc}", file=sys.stderr)


def main(argv: Sequence[str] | None = None) -> int:
    return flash(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
