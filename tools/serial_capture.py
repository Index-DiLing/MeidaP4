#!/usr/bin/env python3
"""Capture ESP32-P4 UART output for a fixed time and save it.

Used by the smoke-test procedure (docs/probe-plan.md). Run it with the ESP-IDF
python interpreter, which already has pyserial:

    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools\\serial_capture.py \
        --port COM16 --baud 115200 --seconds 12 --out build-main-a/serial-mode-a.log

Opening the port asserts DTR/RTS, which resets a dev board wired the usual way,
so the capture normally starts at the boot banner. Exit code 0 means "we read
something", 1 means "nothing arrived", 2 means "tool/port error".
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    print(
        "pyserial is missing. Use the ESP-IDF python env:\n"
        r"  C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe",
        file=sys.stderr,
    )
    raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture ESP32 serial output")
    ap.add_argument("--port", required=True, help="e.g. COM16")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--out", help="also write the raw bytes here")
    ap.add_argument("--no-reset", action="store_true", help="do not touch DTR/RTS")
    ap.add_argument("--grep", action="append", default=[], help="substring to highlight; repeatable")
    args = ap.parse_args()

    try:
        ser = serial.Serial()
        ser.port = args.port
        ser.baudrate = args.baud
        ser.timeout = 0.2
        # Reset the target into a normal boot: DTR/RTS low then release.
        if not args.no_reset:
            ser.dtr = False
            ser.rts = True
        ser.open()
        if args.no_reset:
            # Best effort only. On boards with the classic two-transistor auto-reset circuit
            # the transition performed by the driver while opening the port can still reset
            # the chip - which silently zeroes any on-device counters. We therefore detect a
            # boot banner afterwards and say so loudly instead of reporting bogus zeros.
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
        else:
            time.sleep(0.1)
            ser.rts = False
            ser.dtr = False
    except Exception as exc:  # serial.SerialException and friends
        print(f"cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    sink = None
    if args.out:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        sink = out_path.open("wb")

    total = 0
    head = bytearray()
    deadline = time.time() + args.seconds
    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            total += len(chunk)
            if len(head) < 4096:
                head.extend(chunk)
            if sink:
                sink.write(chunk)
                sink.flush()
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.close()
        finally:
            if sink:
                sink.close()

    print(f"\n--- captured {total} bytes from {args.port} @ {args.baud} in {args.seconds}s ---")

    if args.no_reset:
        text = head.decode("utf-8", errors="replace")
        if "rst:0x" in text or "boot:0x" in text or "cpu_start" in text:
            print(
                "WARNING: a boot banner was captured even though --no-reset was given.\n"
                "         The chip restarted when the port was opened, so any on-device\n"
                "         counters printed during this capture were RESET and must NOT be\n"
                "         read as absolute values. Diff two captures made the same way, or\n"
                "         read the counters from a log the firmware itself produced.",
                file=sys.stderr,
            )
    if args.grep and args.out:
        text = Path(args.out).read_text(encoding="utf-8", errors="replace")
        for needle in args.grep:
            hits = [ln for ln in text.splitlines() if needle.lower() in ln.lower()]
            print(f"[grep] {needle!r}: {len(hits)} hit(s)")
            for ln in hits[:10]:
                print(f"       {ln}")

    return 0 if total > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
