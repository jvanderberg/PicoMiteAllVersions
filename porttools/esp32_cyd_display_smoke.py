#!/usr/bin/env python3
"""Classic-ESP32 CYD local display surface smoke.

The no-PSRAM CYD presents straight to the panel: it supports direct-draw
primitives and MISO PIXEL readback, but has no off-screen buffer, so
FRAMEBUFFER and FASTGFX are rejected rather than silently drawing into a
buffer that never reaches glass. This smoke proves both halves:

  - direct PIXEL draw + readback for red/green/blue, and
  - FRAMEBUFFER CREATE / LAYER and FASTGFX CREATE fail cleanly with a
    "not supported" error (no crash, no silent no-op).

Companion to esp32_display_surface_smoke.py, which covers the PSRAM-backed
S3/Freenove FRAMEBUFFER path that this board intentionally lacks.
"""

from __future__ import annotations

import argparse
import sys
from typing import Iterable

from basic_serial import BasicSerial, default_port
from esp32_fs_vm_smoke import basic_string_expr, join_drive, normalise_drive


def command(basic: BasicSerial, line: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(line, timeout=timeout, check_error=check_error).clean_text


def write_program(basic: BasicSerial, path: str, lines: Iterable[str], *, timeout: float) -> None:
    command(basic, f'ON ERROR SKIP : KILL "{path}"', timeout, check_error=False)
    command(basic, f'OPEN "{path}" FOR OUTPUT AS #1', timeout)
    try:
        for line in lines:
            command(basic, f"PRINT #1,{basic_string_expr(line)}", timeout)
    finally:
        command(basic, "CLOSE #1", timeout, check_error=False)


def cyd_display_program() -> list[str]:
    return [
        "OPTION EXPLICIT",
        "DIM INTEGER p%",
        "CLS RGB(BLACK)",
        # --- direct-draw + MISO readback (supported) ---
        "PIXEL 2, 2, RGB(RED)",
        "p% = PIXEL(2, 2)",
        'IF p% \\ 65536 < 180 THEN ERROR "pixel red r"',
        'IF (p% \\ 256) MOD 256 > 80 THEN ERROR "pixel red g"',
        'IF p% MOD 256 > 80 THEN ERROR "pixel red b"',
        "PIXEL 3, 3, RGB(GREEN)",
        "p% = PIXEL(3, 3)",
        'IF (p% \\ 256) MOD 256 < 180 THEN ERROR "pixel green g"',
        "PIXEL 4, 4, RGB(BLUE)",
        "p% = PIXEL(4, 4)",
        'IF p% MOD 256 < 180 THEN ERROR "pixel blue b"',
        # --- FRAMEBUFFER must be rejected cleanly (no off-screen buffer) ---
        "ON ERROR SKIP",
        "FRAMEBUFFER CREATE",
        'IF MM.ERRNO = 0 THEN ERROR "FRAMEBUFFER CREATE was not rejected"',
        'IF INSTR(MM.ERRMSG$, "not supported") = 0 THEN ERROR "FB create msg: " + MM.ERRMSG$',
        "ON ERROR SKIP",
        "FRAMEBUFFER LAYER RGB(BLACK)",
        'IF MM.ERRNO = 0 THEN ERROR "FRAMEBUFFER LAYER was not rejected"',
        # --- FASTGFX must also be rejected (no shadow buffer) ---
        "ON ERROR SKIP",
        "FASTGFX CREATE",
        'IF MM.ERRNO = 0 THEN ERROR "FASTGFX CREATE was not rejected"',
        "ON ERROR CLEAR",
        'PRINT "ESP32_CYD_DISPLAY_OK"',
    ]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--long-timeout", type=float, default=60.0)
    parser.add_argument("--drive", default="A:")
    parser.add_argument("--program", default="esp32_cyd_display.bas")
    parser.add_argument("--keep-file", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    drive = normalise_drive(args.drive)
    path = join_drive(drive, args.program)
    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.long_timeout, boot_wait=args.boot_wait)
        command(basic, "ON ERROR SKIP : FRAMEBUFFER CLOSE", args.timeout, check_error=False)
        command(basic, "ON ERROR SKIP : FASTGFX CLOSE", args.timeout, check_error=False)
        write_program(basic, path, cyd_display_program(), timeout=args.timeout)
        print(f"PASS program upload - {path}")
        try:
            out = command(basic, f'RUN "{path}"', args.long_timeout)
            if "ESP32_CYD_DISPLAY_OK" not in out:
                print(f"esp32_cyd_display_smoke: FAIL: missing OK marker\n{out}")
                return 1
        finally:
            if not args.keep_file:
                command(basic, f'ON ERROR SKIP : KILL "{path}"', args.timeout, check_error=False)
    print("PASS direct-draw + readback; FRAMEBUFFER/FASTGFX rejected cleanly")
    print("esp32_cyd_display_smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
