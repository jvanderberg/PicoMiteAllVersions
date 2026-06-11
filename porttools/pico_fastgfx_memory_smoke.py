#!/usr/bin/env python3
"""Smoke-test FASTGFX allocation failure recovery."""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from basic_serial import BasicSerial, default_port  # noqa: E402


def command(basic: BasicSerial, cmd: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(cmd, timeout=timeout, check_error=check_error).clean_text


def marker_int(text: str, marker: str) -> int:
    for line in text.splitlines():
        if line.startswith(marker):
            match = re.search(r"-?\d+", line[len(marker) :])
            if match:
                return int(match.group(0))
    raise RuntimeError(f"missing integer marker {marker!r} in transcript:\n{text}")


def heap_free(basic: BasicSerial, timeout: float) -> int:
    return marker_int(command(basic, 'PRINT "FG_HEAP="+STR$(MM.INFO(HEAP))', timeout), "FG_HEAP=")


def fastgfx_errno(basic: BasicSerial, timeout: float) -> int:
    command(basic, "ON ERROR SKIP : FASTGFX CREATE", timeout, check_error=False)
    return marker_int(command(basic, 'PRINT "FG_ERR="+STR$(MM.ERRNO)', timeout), "FG_ERR=")


def cleanup(basic: BasicSerial, timeout: float) -> None:
    command(basic, "ON ERROR SKIP : FASTGFX CLOSE", timeout, check_error=False)
    command(basic, "NEW", timeout, check_error=False)


def allocate_heap_hog(
    basic: BasicSerial,
    *,
    target_free: int,
    timeout: float,
    long_timeout: float,
) -> tuple[int, int]:
    """Allocate integer arrays until free heap is near target_free.

    Returns (array_count, element_count). Multiple arrays are needed on large
    heaps because a single DIM may hit target-specific allocation limits before
    it applies enough pressure for FASTGFX.
    """
    arrays = 0
    total_elements = 0
    while arrays < 32:
        free_now = heap_free(basic, timeout)
        if free_now <= target_free:
            return arrays, total_elements
        hog_bytes = max(0, free_now - target_free)
        elements = max(1, hog_bytes // 8 - 64)

        allocated = False
        while elements >= 256:
            text = command(
                basic,
                f"DIM INTEGER fg_hog{arrays}%({elements})",
                long_timeout,
                check_error=False,
            )
            if "Error :" not in text and "Error:" not in text:
                arrays += 1
                total_elements += elements + 1
                allocated = True
                break
            elements = int(elements * 0.85)

        if not allocated:
            return arrays, total_elements

    return arrays, total_elements


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port(), help="serial device path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--long-timeout", type=float, default=30.0)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument(
        "--target-free",
        type=int,
        default=70000,
        help="try to leave about this many heap bytes before FASTGFX CREATE",
    )
    args = parser.parse_args()

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.timeout, boot_wait=args.boot_wait)
        cleanup(basic, args.timeout)
        baseline = heap_free(basic, args.timeout)

        if baseline <= args.target_free + 16384:
            raise RuntimeError(f"heap too small for controlled FASTGFX OOM smoke: {baseline} bytes")

        oom_seen = False
        last_detail = ""
        for target_free in (args.target_free, 62000, 56000, 50000, 44000):
            cleanup(basic, args.timeout)
            arrays, elements = allocate_heap_hog(
                basic,
                target_free=target_free,
                timeout=args.timeout,
                long_timeout=args.long_timeout,
            )
            if arrays == 0:
                last_detail = f"could not allocate heap hog near target_free={target_free}"
                continue

            free_after_hog = heap_free(basic, args.timeout)
            err = fastgfx_errno(basic, args.long_timeout)
            last_detail = (
                f"baseline={baseline} free_after_hog={free_after_hog} "
                f"hog_arrays={arrays} hog_elements={elements} errno={err}"
            )
            if err != 0:
                oom_seen = True
                break
            command(basic, "FASTGFX CLOSE", args.long_timeout, check_error=False)
            time.sleep(0.05)

        if not oom_seen:
            cleanup(basic, args.timeout)
            raise RuntimeError(f"FASTGFX CREATE did not hit OOM; {last_detail}")

        cleanup(basic, args.timeout)
        command(basic, "FASTGFX CREATE", args.long_timeout)
        command(basic, "FASTGFX CLOSE", args.long_timeout)
        recovered = heap_free(basic, args.timeout)

    print(f"ok   FASTGFX OOM surfaced without lockup - {last_detail}", flush=True)
    print(f"ok   FASTGFX recovery create/close - heap {recovered} bytes", flush=True)
    print("2/2 passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
