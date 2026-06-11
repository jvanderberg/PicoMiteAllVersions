#!/usr/bin/env python3
"""Smoke-test PicoCalc boot-reserved GP0/GP1 pin handling."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from basic_serial import BasicSerial, default_port  # noqa: E402


def command(basic: BasicSerial, cmd: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(cmd, timeout=timeout, check_error=check_error).clean_text


def marker_value(text: str, marker: str) -> str:
    for line in text.splitlines():
        if line.startswith(marker):
            return line[len(marker) :].strip()
    raise RuntimeError(f"missing {marker!r} in transcript:\n{text}")


def marker_int(text: str, marker: str) -> int:
    value = marker_value(text, marker)
    match = re.search(r"-?\d+", value)
    if not match:
        raise RuntimeError(f"missing integer after {marker!r}: {value!r}")
    return int(match.group(0))


def expect_error(text: str, label: str) -> None:
    if "Error :" not in text and "Error:" not in text:
        raise RuntimeError(f"{label} unexpectedly succeeded:\n{text}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port(), help="serial device path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--long-timeout", type=float, default=20.0)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument(
        "--expect-psram-gp0",
        action="store_true",
        help="require OPTION PSRAM PIN GP0 and a nonzero PSRAM size",
    )
    args = parser.parse_args()

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.timeout, boot_wait=args.boot_wait)
        command(basic, "NEW", args.timeout)

        device = marker_value(command(basic, 'PRINT "PINS_DEVICE="+MM.DEVICE$', args.timeout), "PINS_DEVICE=")
        keyboard = marker_value(
            command(basic, 'PRINT "PINS_KEYBOARD="+MM.INFO$(OPTION KEYBOARD)', args.timeout),
            "PINS_KEYBOARD=",
        )
        gp0 = marker_value(command(basic, 'PRINT "PINS_GP0="+MM.INFO$(PIN GP0)', args.timeout), "PINS_GP0=")
        gp1 = marker_value(command(basic, 'PRINT "PINS_GP1="+MM.INFO$(PIN GP1)', args.timeout), "PINS_GP1=")
        psram_size = marker_int(
            command(basic, 'PRINT "PINS_PSRAM="+STR$(MM.INFO(PSRAM SIZE))', args.timeout),
            "PINS_PSRAM=",
        )
        options = command(basic, "OPTION LIST", args.long_timeout)

        if "PicoCalc" not in device and "PicoCalc" not in keyboard:
            raise RuntimeError(f"not a PicoCalc target: device={device!r} keyboard={keyboard!r}")

        gp0_uses_psram = "OPTION PSRAM PIN GP0" in options or psram_size > 0 or args.expect_psram_gp0
        expected_gp0 = "PSRAM CS" if gp0_uses_psram else "KEYPAD MCU UART"
        if expected_gp0 not in gp0:
            raise RuntimeError(f"GP0 label {gp0!r} did not contain {expected_gp0!r}")
        if "KEYPAD MCU UART" not in gp1:
            raise RuntimeError(f"GP1 label {gp1!r} did not contain 'KEYPAD MCU UART'")

        if args.expect_psram_gp0:
            if "OPTION PSRAM PIN GP0" not in options:
                raise RuntimeError("expected OPTION PSRAM PIN GP0 in OPTION LIST")
            if psram_size <= 0:
                raise RuntimeError(f"expected nonzero PSRAM size, got {psram_size}")

        expect_error(command(basic, "SETPIN GP0,DOUT", args.timeout, check_error=False), "SETPIN GP0,DOUT")
        expect_error(command(basic, "SETPIN GP1,DOUT", args.timeout, check_error=False), "SETPIN GP1,DOUT")

    print(f"ok   PicoCalc GP0 label - {gp0}", flush=True)
    print(f"ok   PicoCalc GP1 label - {gp1}", flush=True)
    print("ok   PicoCalc GP0/GP1 SETPIN rejection", flush=True)
    if gp0_uses_psram:
        print(f"ok   PicoCalc PSRAM on GP0 - {psram_size} bytes", flush=True)
    print("3/3 passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
