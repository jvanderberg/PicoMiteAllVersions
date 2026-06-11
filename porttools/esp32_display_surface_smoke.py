#!/usr/bin/env python3
"""ESP32-S3 local display surface smoke.

Exercises the BASIC-visible local LCD display surfaces that are not covered by
the scalar display performance benchmark: PIXEL readback, FRAMEBUFFER
create/copy/layer/merge/fast shadow, FASTGFX create/swap/sync/close, RUN, and
FRUN.
"""

from __future__ import annotations

import argparse
import sys
from typing import Iterable

from basic_serial import BasicSerial, default_port
from esp32_fs_vm_smoke import basic_string_expr, join_drive, normalise_drive


def command(basic: BasicSerial, line: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(line, timeout=timeout, check_error=check_error).clean_text


def write_program(
    basic: BasicSerial,
    path: str,
    lines: Iterable[str],
    *,
    timeout: float,
) -> None:
    command(basic, f'ON ERROR SKIP : KILL "{path}"', timeout, check_error=False)
    command(basic, f'OPEN "{path}" FOR OUTPUT AS #1', timeout)
    try:
        for line in lines:
            command(basic, f"PRINT #1,{basic_string_expr(line)}", timeout)
    finally:
        command(basic, "CLOSE #1", timeout, check_error=False)


def run_and_expect(
    basic: BasicSerial,
    run_command: str,
    marker: str,
    *,
    timeout: float,
) -> str:
    output = command(basic, run_command, timeout)
    if marker not in output:
        raise RuntimeError(f"missing marker {marker!r} after {run_command!r}\n{output}")
    return output


def display_surface_program() -> list[str]:
    return [
        "OPTION EXPLICIT",
        "ON ERROR SKIP : FASTGFX CLOSE",
        "ON ERROR SKIP : FRAMEBUFFER CLOSE",
        "OPTION AUTOREFRESH OFF",
        "OPTION CONSOLE BOTH",
        "DIM INTEGER p%",
        'IF MM.HRES <= 0 OR MM.VRES <= 0 THEN ERROR "display geometry"',
        "CLS RGB(BLACK)",
        "PIXEL 2, 2, RGB(RED)",
        "p% = PIXEL(2, 2)",
        'IF p% \\ 65536 < 180 THEN ERROR "pixel red r"',
        'IF (p% \\ 256) MOD 256 > 80 THEN ERROR "pixel red g"',
        'IF p% MOD 256 > 80 THEN ERROR "pixel red b"',
        "LINE 0, 0, 10, 10, 1, RGB(GREEN)",
        "BOX 12, 12, 8, 8, 1, RGB(BLUE), RGB(BLUE)",
        "FRAMEBUFFER CREATE",
        "FRAMEBUFFER WRITE F",
        "CLS RGB(BLACK)",
        "PIXEL 3, 3, RGB(GREEN)",
        "FRAMEBUFFER COPY F, N",
        "FRAMEBUFFER WAIT",
        "FRAMEBUFFER WRITE N",
        "p% = PIXEL(3, 3)",
        'IF p% \\ 65536 > 80 THEN ERROR "framebuffer copy r"',
        'IF (p% \\ 256) MOD 256 < 180 THEN ERROR "framebuffer copy g"',
        'IF p% MOD 256 > 80 THEN ERROR "framebuffer copy b"',
        "FRAMEBUFFER LAYER RGB(BLACK)",
        "FRAMEBUFFER WRITE L",
        "PIXEL 4, 4, RGB(BLUE)",
        "FRAMEBUFFER MERGE RGB(BLACK)",
        "FRAMEBUFFER WAIT",
        "FRAMEBUFFER CLOSE",
        "p% = PIXEL(4, 4)",
        'IF p% \\ 65536 > 80 THEN ERROR "framebuffer merge r"',
        'IF (p% \\ 256) MOD 256 > 80 THEN ERROR "framebuffer merge g"',
        'IF p% MOD 256 < 180 THEN ERROR "framebuffer merge b"',
        "FRAMEBUFFER CREATE FAST",
        "FRAMEBUFFER WRITE F",
        "CLS RGB(BLACK)",
        "PIXEL 6, 6, RGB(WHITE)",
        "FRAMEBUFFER LAYER RGB(BLACK)",
        "FRAMEBUFFER WRITE L",
        "PIXEL 7, 7, RGB(GREEN)",
        "FRAMEBUFFER MERGE RGB(BLACK)",
        "FRAMEBUFFER WAIT",
        "FRAMEBUFFER CLOSE",
        "p% = PIXEL(7, 7)",
        'IF p% \\ 65536 > 80 THEN ERROR "framebuffer fast merge r"',
        'IF (p% \\ 256) MOD 256 < 180 THEN ERROR "framebuffer fast merge g"',
        'IF p% MOD 256 > 80 THEN ERROR "framebuffer fast merge b"',
        "FASTGFX CREATE",
        "FASTGFX FPS 1000",
        "CLS RGB(BLACK)",
        "PIXEL 5, 5, RGB(RED)",
        "p% = PIXEL(5, 5)",
        'IF p% \\ 65536 < 180 THEN ERROR "fastgfx red r"',
        'IF (p% \\ 256) MOD 256 > 80 THEN ERROR "fastgfx red g"',
        'IF p% MOD 256 > 80 THEN ERROR "fastgfx red b"',
        "FASTGFX SWAP",
        "FASTGFX SYNC",
        "FASTGFX CLOSE",
        "OPTION AUTOREFRESH ON",
        "REFRESH",
        'PRINT "ESP32_DISPLAY_SURFACE_OK"',
    ]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--long-timeout", type=float, default=120.0)
    parser.add_argument("--drive", default="A:")
    parser.add_argument("--program", default="esp32_display_surface.bas")
    parser.add_argument("--keep-file", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    drive = normalise_drive(args.drive)
    path = join_drive(drive, args.program)
    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.long_timeout, boot_wait=args.boot_wait)
        write_program(basic, path, display_surface_program(), timeout=args.timeout)
        print(f"PASS program upload - {path}", flush=True)
        run_and_expect(basic, f'RUN "{path}"', "ESP32_DISPLAY_SURFACE_OK", timeout=args.long_timeout)
        print("PASS RUN display surface", flush=True)
        run_and_expect(basic, f'FRUN "{path}"', "ESP32_DISPLAY_SURFACE_OK", timeout=args.long_timeout)
        print("PASS FRUN display surface", flush=True)
        command(basic, "ON ERROR SKIP : FASTGFX CLOSE", args.timeout, check_error=False)
        command(basic, "ON ERROR SKIP : FRAMEBUFFER CLOSE", args.timeout, check_error=False)
        command(basic, "ON ERROR SKIP : OPTION AUTOREFRESH ON", args.timeout, check_error=False)
        command(basic, "ON ERROR SKIP : REFRESH", args.timeout, check_error=False)
        if not args.keep_file:
            command(basic, f'ON ERROR SKIP : KILL "{path}"', args.timeout, check_error=False)
            print("Generated display surface program removed", flush=True)
    print("esp32_display_surface_smoke: PASS", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"esp32_display_surface_smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
