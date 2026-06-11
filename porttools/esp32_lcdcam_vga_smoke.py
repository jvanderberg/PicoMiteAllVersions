#!/usr/bin/env python3
"""ESP32-S3 LCD_CAM/VGA smoke for a VGA-wired board.

This script is intentionally not part of the Freenove suite. It changes saved
VGA options and can reboot the device, so run it only on an ESP32-S3 board
wired for LCD_CAM VGA output.
"""

from __future__ import annotations

import argparse
import sys
import time

from basic_serial import BasicSerial, default_port


METRO_3BIT_COMMANDS = [
    "OPTION VGA 3BIT GP8,GP9,GP10,GP11,GP12",
    "OPTION VGA SYNC NEGATIVE,NEGATIVE",
    "OPTION VGA CLOCK 25MHZ",
    "OPTION VGA DRIVE 2",
]


def marker(text: str, key: str) -> str:
    for line in text.splitlines():
        if line.startswith(key):
            return line[len(key):].strip()
    raise RuntimeError(f"missing marker {key!r}\n{text}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--long-timeout", type=float, default=60.0)
    parser.add_argument("--configure-metro-3bit", action="store_true", help="write Metro 3-bit VGA options before testing")
    parser.add_argument("--keep-vga", action="store_true", help="leave VGA options enabled after the smoke")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.long_timeout, boot_wait=args.boot_wait)
        if args.configure_metro_3bit:
            for command in METRO_3BIT_COMMANDS:
                try:
                    basic.command(command, timeout=args.long_timeout)
                except Exception:
                    time.sleep(2)
                    basic.sync(timeout=args.long_timeout, boot_wait=max(args.boot_wait, 5.0))
            basic.command("CPU RESTART", timeout=args.timeout, check_error=False)
            time.sleep(3)
            basic.sync(timeout=args.long_timeout, boot_wait=max(args.boot_wait, 5.0))

        basic.command("MODE 2", timeout=args.long_timeout)
        hres = int(float(marker(basic.command('PRINT "HRES=" + STR$(MM.HRES)', timeout=args.timeout).clean_text, "HRES=")))
        vres = int(float(marker(basic.command('PRINT "VRES=" + STR$(MM.VRES)', timeout=args.timeout).clean_text, "VRES=")))
        if hres <= 0 or vres <= 0:
            raise RuntimeError(f"VGA geometry did not initialize: {hres}x{vres}")
        basic.command("CLS RGB(BLACK)", timeout=args.timeout)
        basic.command("PIXEL 5,5,RGB(RED)", timeout=args.timeout)
        pixel_text = basic.command('PRINT "P=" + STR$(PIXEL(5,5))', timeout=args.timeout).clean_text
        pixel = int(float(marker(pixel_text, "P=")))
        if pixel <= 0:
            raise RuntimeError(f"VGA pixel readback failed: {pixel}")
        print(f"PASS LCD_CAM VGA mode - {hres}x{vres}", flush=True)

        if not args.keep_vga:
            try:
                basic.command("OPTION VGA DISABLE", timeout=args.long_timeout)
            except Exception:
                pass
    print("esp32_lcdcam_vga_smoke: PASS", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"esp32_lcdcam_vga_smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
