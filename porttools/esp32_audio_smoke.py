#!/usr/bin/env python3
"""ESP32-S3 BASIC audio command smoke.

Checks that the configured ESP32 audio backend accepts the shared PLAY command
surface. This is not an acoustic validation; it verifies command plumbing,
backend initialization, status reporting, and stop/recovery.
"""

from __future__ import annotations

import argparse
import sys

from basic_serial import BasicSerial, default_port


def clean_marker(text: str, marker: str) -> str:
    for line in text.splitlines():
        if line.startswith(marker):
            return line[len(marker):].strip()
    raise RuntimeError(f"missing marker {marker!r}\n{text}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--long-timeout", type=float, default=30.0)
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.long_timeout, boot_wait=args.boot_wait)
        audio = basic.command('PRINT "AUDIO=" + MM.INFO$(AUDIO)', timeout=args.timeout).clean_text
        status = basic.command('PRINT "AUDIO_STATUS=" + MM.INFO$(AUDIO STATUS)', timeout=args.timeout).clean_text
        audio_value = clean_marker(audio, "AUDIO=")
        status_value = clean_marker(status, "AUDIO_STATUS=")
        if not audio_value or audio_value.upper() == "OFF":
            raise RuntimeError(f"audio backend is not configured: {audio_value!r}")
        print(f"PASS audio option - {audio_value}", flush=True)
        print(f"PASS audio status - {status_value}", flush=True)

        commands = [
            "PLAY STOP",
            "PLAY VOLUME 8,8",
            "PLAY TONE 440,440,200",
            "PAUSE 300",
            'PLAY SOUND 1,B,S,660,8',
            "PAUSE 150",
            "PLAY NOTE ON 0,60,64",
            "PAUSE 150",
            "PLAY NOTE OFF 0,60,64",
            "PLAY STOP",
        ]
        for command in commands:
            timeout = args.long_timeout if command.startswith("PAUSE") else args.timeout
            basic.command(command, timeout=timeout)
        print("PASS PLAY VOLUME/TONE/SOUND/NOTE/STOP", flush=True)
    print("esp32_audio_smoke: PASS", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"esp32_audio_smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
