#!/usr/bin/env python3
"""Smoke-test PIO DMA over the BASIC serial prompt.

The RP2350 path exercises PIO2, which catches DREQ mapping regressions where a
DMA channel arms but never drains the PIO2 FIFO.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from basic_serial import BasicSerial, default_port  # noqa: E402


def command(basic: BasicSerial, cmd: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(cmd, timeout=timeout, check_error=check_error).clean_text


def has_pio2(device: str) -> bool:
    return "RP2350" in device


def setup_push_loop(basic: BasicSerial, pio: int, timeout: float) -> None:
    commands = [
        "PIO DMA RX OFF",
        "PIO DMA TX OFF",
        f"PIO STOP {pio},0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program dmrx"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"push noblock"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
    ]
    for cmd in commands:
        command(basic, cmd, timeout, check_error=not cmd.startswith("PIO STOP"))


def setup_pull_loop(basic: BasicSerial, pio: int, timeout: float) -> None:
    commands = [
        "PIO DMA RX OFF",
        "PIO DMA TX OFF",
        f"PIO STOP {pio},0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program dmtx"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"pull block"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
    ]
    for cmd in commands:
        command(basic, cmd, timeout, check_error=not cmd.startswith("PIO STOP"))


def run_rx_case(basic: BasicSerial, pio: int, timeout: float) -> None:
    setup_push_loop(basic, pio, timeout)
    command(basic, "DIM INTEGER a%(1)", timeout)
    command(basic, "a%(0)=111:a%(1)=222", timeout)
    command(basic, f"PIO DMA RX {pio},0,4,a%(),,32", timeout)

    done = False
    for _ in range(40):
        status = command(
            basic,
            f'PRINT "PIO{pio}_BUSY="+STR$(MM.INFO(PIO RX DMA))',
            timeout,
        )
        if f"PIO{pio}_BUSY=0" in status:
            done = True
            break
        time.sleep(0.05)

    values = command(
        basic,
        f'PRINT "PIO{pio}_A0="+STR$(a%(0))+" A1="+STR$(a%(1))',
        timeout,
    )
    command(basic, "PIO DMA RX OFF", timeout, check_error=False)
    command(basic, f"PIO STOP {pio},0", timeout, check_error=False)
    command(basic, f"PIO CLEAR {pio}", timeout, check_error=False)

    if not done:
        raise RuntimeError(f"PIO{pio} RX DMA did not complete")
    if f"PIO{pio}_A0=0 A1=0" not in values:
        raise RuntimeError(f"PIO{pio} RX DMA wrote unexpected data:\n{values}")

    print(f"ok   PIO{pio} RX DMA - completed and wrote 4 words", flush=True)


def run_tx_case(basic: BasicSerial, pio: int, timeout: float) -> None:
    setup_pull_loop(basic, pio, timeout)
    command(basic, "DIM INTEGER a%(1)", timeout)
    command(basic, "a%(0)=&H2222222211111111:a%(1)=&H4444444433333333", timeout)
    command(basic, f"PIO DMA TX {pio},0,4,a%(),,32", timeout)

    done = False
    for _ in range(40):
        status = command(
            basic,
            f'PRINT "PIO{pio}_TX_BUSY="+STR$(MM.INFO(PIO TX DMA))',
            timeout,
        )
        if f"PIO{pio}_TX_BUSY=0" in status:
            done = True
            break
        time.sleep(0.05)

    command(basic, "PIO DMA TX OFF", timeout, check_error=False)
    command(basic, f"PIO STOP {pio},0", timeout, check_error=False)
    command(basic, f"PIO CLEAR {pio}", timeout, check_error=False)

    if not done:
        raise RuntimeError(f"PIO{pio} TX DMA did not complete")

    print(f"ok   PIO{pio} TX DMA - completed and drained 4 words", flush=True)


def run_init_outout_case(basic: BasicSerial, timeout: float) -> None:
    command(basic, "PIO CLEAR 0", timeout, check_error=False)
    command(basic, "PIO INIT MACHINE 0,0,1000000,,,,0,,,1", timeout)
    command(basic, "PIO CLEAR 0", timeout, check_error=False)
    print("ok   PIO INIT MACHINE - accepted outout argument", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port(), help="serial device path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    args = parser.parse_args()

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.timeout, boot_wait=args.boot_wait)
        command(basic, "NEW", args.timeout)
        device = command(basic, 'PRINT "PIO_DMA_DEVICE="+MM.DEVICE$', args.timeout)
        pios = [0, 2] if has_pio2(device) else [0]
        passed = 0
        total = len(pios) * 2 + 1
        run_init_outout_case(basic, args.timeout)
        passed += 1
        command(basic, "NEW", args.timeout)
        for pio in pios:
            run_rx_case(basic, pio, args.timeout)
            passed += 1
            command(basic, "NEW", args.timeout)
            run_tx_case(basic, pio, args.timeout)
            passed += 1
            command(basic, "NEW", args.timeout)

    print(f"{passed}/{total} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
