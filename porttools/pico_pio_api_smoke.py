#!/usr/bin/env python3
"""Exercise the RP2 PIO BASIC API over the serial prompt.

The smoke keeps all state-machine programs pinless so it is safe on PicoCalc
boards with boot-reserved GP0/GP1 and optional PSRAM on GP0.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from basic_serial import BasicSerial, default_port  # noqa: E402


def command(basic: BasicSerial, cmd: str, timeout: float, *, check_error: bool = True) -> str:
    return basic.command(cmd, timeout=timeout, check_error=check_error).clean_text


def has_error(text: str) -> bool:
    return "Error :" in text or "Error:" in text


def marker_value(text: str, marker: str) -> str:
    for line in text.splitlines():
        if line.startswith(marker):
            return line[len(marker) :].strip()
    raise RuntimeError(f"missing marker {marker!r} in transcript:\n{text}")


def marker_int(text: str, marker: str) -> int:
    value = marker_value(text, marker)
    match = re.search(r"-?\d+", value)
    if not match:
        raise RuntimeError(f"missing integer after marker {marker!r}: {value!r}")
    return int(match.group(0))


def has_pio2(device: str) -> bool:
    return "RP2350" in device


def basic_string_expr(text: str) -> str:
    if '"' not in text:
        return f'"{text}"'
    parts = text.split('"')
    expr: list[str] = []
    for index, part in enumerate(parts):
        if part:
            expr.append(f'"{part}"')
        if index != len(parts) - 1:
            expr.append("CHR$(34)")
    return " + ".join(expr) if expr else '""'


def cleanup_pio(basic: BasicSerial, pios: Iterable[int], timeout: float) -> None:
    command(basic, "PIO DMA RX OFF", timeout, check_error=False)
    command(basic, "PIO DMA TX OFF", timeout, check_error=False)
    for pio in pios:
        for sm in range(4):
            command(basic, f"PIO INTERRUPT {pio},{sm},0,0", timeout, check_error=False)
            command(basic, f"PIO STOP {pio},{sm}", timeout, check_error=False)
        command(basic, f"PIO CLEAR {pio}", timeout, check_error=False)
    command(basic, "NEW", timeout, check_error=False)


def setup_push_loop(basic: BasicSerial, pio: int, timeout: float, *, name: str = "dmrx") -> None:
    for cmd in (
        "PIO DMA RX OFF",
        "PIO DMA TX OFF",
        f"PIO STOP {pio},0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program {name}"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"push noblock"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
    ):
        command(basic, cmd, timeout, check_error=not cmd.startswith("PIO STOP"))


def setup_pull_loop(basic: BasicSerial, pio: int, timeout: float, *, name: str = "dmtx") -> None:
    for cmd in (
        "PIO DMA RX OFF",
        "PIO DMA TX OFF",
        f"PIO STOP {pio},0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program {name}"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"pull block"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
    ):
        command(basic, cmd, timeout, check_error=not cmd.startswith("PIO STOP"))


def setup_echo_loop(basic: BasicSerial, pio: int, timeout: float, *, name: str = "echo") -> None:
    for cmd in (
        "PIO DMA RX OFF",
        "PIO DMA TX OFF",
        f"PIO STOP {pio},0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program {name}"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"pull block"',
        f'PIO ASSEMBLE {pio},"mov isr, osr"',
        f'PIO ASSEMBLE {pio},"push block"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
    ):
        command(basic, cmd, timeout, check_error=not cmd.startswith("PIO STOP"))


def wait_busy_clear(basic: BasicSerial, expr: str, marker: str, timeout: float) -> None:
    for _ in range(80):
        text = command(basic, f'PRINT "{marker}="+STR$({expr})', timeout)
        if f"{marker}=0" in text:
            return
        time.sleep(0.05)
    raise RuntimeError(f"{expr} did not clear")


def wait_fifo_level(
    basic: BasicSerial,
    pio: int,
    sm: int,
    fifo: str,
    minimum: int,
    marker: str,
    timeout: float,
) -> None:
    expr = f"PIO(FLEVEL {pio},{sm},{fifo})"
    for _ in range(80):
        text = command(basic, f'PRINT "{marker}="+STR$({expr})', timeout)
        if marker_int(text, f"{marker}=") >= minimum:
            return
        time.sleep(0.05)
    raise RuntimeError(f"{expr} did not reach {minimum}")


def write_program(basic: BasicSerial, path: str, lines: Iterable[str], timeout: float) -> None:
    command(basic, f'ON ERROR SKIP : CLOSE #1', timeout, check_error=False)
    command(basic, f'ON ERROR SKIP : KILL "{path}"', timeout, check_error=False)
    command(basic, f'OPEN "{path}" FOR OUTPUT AS #1', timeout)
    try:
        for line in lines:
            command(basic, f"PRINT #1,{basic_string_expr(line)}", timeout)
    finally:
        command(basic, "CLOSE #1", timeout, check_error=False)


def load_numbered_program(basic: BasicSerial, lines: Iterable[str], timeout: float) -> None:
    command(basic, "NEW", timeout)
    line_no = 10
    for line in lines:
        command(basic, f"{line_no} {line}", timeout)
        line_no += 10


def run_program(basic: BasicSerial, lines: Iterable[str], marker: str, timeout: float) -> str:
    load_numbered_program(basic, lines, timeout)
    text = command(basic, "RUN", timeout=timeout * 3)
    if marker not in text:
        raise RuntimeError(f"missing marker {marker!r} in transcript:\n{text}")
    return text


def run_file_program(
    basic: BasicSerial,
    path: str,
    lines: Iterable[str],
    marker: str,
    timeout: float,
) -> str:
    command(basic, "NEW", timeout)
    write_program(basic, path, lines, timeout)
    text = command(basic, f'RUN "{path}"', timeout=timeout * 3)
    if marker not in text:
        raise RuntimeError(f"missing marker {marker!r} in transcript:\n{text}")
    command(basic, f'ON ERROR SKIP : KILL "{path}"', timeout, check_error=False)
    return text


def smoke_functions_and_configure(basic: BasicSerial, pio: int, timeout: float) -> None:
    command(basic, f"PIO CLEAR {pio}", timeout)
    text = command(
        basic,
        'PRINT "PIO_CFG="+STR$(PIO(PINCTRL 0,0,0,GP0,GP0,GP0,GP0))',
        timeout,
    )
    if marker_int(text, "PIO_CFG=") != 0:
        raise RuntimeError(f"unexpected zero PINCTRL value transcript:\n{text}")
    command(basic, 'PRINT "PIO_EXEC="+STR$(PIO(EXECCTRL GP0,0,31,0,0))', timeout)
    command(basic, 'PRINT "PIO_SHIFT="+STR$(PIO(SHIFTCTRL 31,31,1,1,1,1,0,0))', timeout)
    command(basic, f"PIO CONFIGURE {pio},0,1000000,0", timeout)
    command(basic, f'PRINT "PIO_FSTAT="+STR$(PIO(FSTAT {pio}))', timeout)
    command(basic, f'PRINT "PIO_FDEBUG="+STR$(PIO(FDEBUG {pio}))', timeout)
    command(basic, f'PRINT "PIO_FLEVEL="+STR$(PIO(FLEVEL {pio}))', timeout)
    command(basic, f'PRINT "PIO_NEXT="+STR$(PIO(NEXT LINE {pio}))', timeout)
    print("ok   PIO helper functions and CONFIGURE", flush=True)


def smoke_assembler_surface(basic: BasicSerial, pio: int, timeout: float) -> None:
    commands = [
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program asmsmoke"',
        f'PIO ASSEMBLE {pio},".side_set 1 opt"',
        f'PIO ASSEMBLE {pio},".wrap target"',
        f'PIO ASSEMBLE {pio},"start:"',
        f'PIO ASSEMBLE {pio},"set x, 3 side 0 [1]"',
        f'PIO ASSEMBLE {pio},"mov y, x"',
        f'PIO ASSEMBLE {pio},"in null, 8"',
        f'PIO ASSEMBLE {pio},"out null, 8"',
        f'PIO ASSEMBLE {pio},"push noblock"',
        f'PIO ASSEMBLE {pio},"pull noblock"',
        f'PIO ASSEMBLE {pio},"irq nowait 0"',
        f'PIO ASSEMBLE {pio},"jmp x--, start"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program LIST"',
    ]
    for cmd in commands:
        command(basic, cmd, timeout)
    command(basic, f'PRINT "PIO_WRAP_TARGET="+STR$(PIO(.WRAP TARGET))', timeout)
    command(basic, f'PRINT "PIO_WRAP="+STR$(PIO(.WRAP))', timeout)
    command(basic, f'PRINT "PIO_NEXT="+STR$(PIO(NEXT LINE {pio}))', timeout)
    print("ok   PIO assembler directives/instructions", flush=True)


def smoke_program_load_and_fifo(basic: BasicSerial, pio: int, timeout: float) -> None:
    setup_echo_loop(basic, pio, timeout, name="echoapi")
    command(basic, f"PIO START {pio},0", timeout)
    command(basic, f"PIO WRITE {pio},0,2,&H12345678,&H23456789", timeout)
    wait_fifo_level(basic, pio, 0, "RX", 2, "PIO_ECHO_RXLEVEL", timeout)
    command(basic, "DIM INTEGER pio_rd%(1)", timeout)
    command(basic, f"PIO READ {pio},0,2,pio_rd%()", timeout)
    text = command(
        basic,
        'PRINT "PIO_ECHO="+HEX$(pio_rd%(0))+","+HEX$(pio_rd%(1))',
        timeout,
    )
    if "PIO_ECHO=12345678,23456789" not in text:
        raise RuntimeError(f"PIO WRITE/READ echo mismatch:\n{text}")
    command(basic, f"PIO EXECUTE {pio},0,&HA042", timeout)
    command(basic, f"PIO STOP {pio},0", timeout)

    command(basic, f"PIO CLEAR {pio}", timeout)
    for line, instruction in enumerate(("&HE020", "&H8060", "&H0001")):
        command(basic, f"PIO PROGRAM LINE {pio},{line},{instruction}", timeout)
    command(basic, f"PIO INIT MACHINE {pio},0,1000000,,,,0,,,1", timeout)

    command(basic, f"PIO CLEAR {pio}", timeout)
    command(basic, "DIM INTEGER pio_prog%(7)", timeout)
    command(basic, "pio_prog%(0)=&HA042A042A042A042", timeout)
    command(basic, f"PIO PROGRAM {pio},pio_prog%()", timeout)
    command(basic, f"PIO INIT MACHINE {pio},0,1000000", timeout)
    print("ok   PIO PROGRAM/LINE/START/STOP/EXECUTE/WRITE/READ", flush=True)


def smoke_rp2350_fifo_and_base(basic: BasicSerial, pio: int, timeout: float) -> None:
    command(basic, f"PIO CLEAR {pio}", timeout)
    command(basic, "pio_sc%=PIO(SHIFTCTRL 0,0,0,0,1,1,0,0,1,1)", timeout)
    command(basic, f"PIO INIT MACHINE {pio},0,1000000,,,pio_sc%", timeout)
    command(basic, f"PIO WRITEFIFO {pio},0,0,&H13579BDF", timeout)
    text = command(basic, f'PRINT "PIO_RPFIFO="+HEX$(PIO(READFIFO {pio},0,0))', timeout)
    observed = marker_value(text, "PIO_RPFIFO=")
    command(basic, f"PIO SET BASE {pio},0", timeout)
    text = command(basic, f"PIO SET BASE {pio},16", timeout, check_error=False)
    if has_error(text) and "Invalid for 60-pin chip" not in text:
        raise RuntimeError(f"unexpected PIO SET BASE 16 failure:\n{text}")
    command(basic, f"PIO SET BASE {pio},0", timeout, check_error=False)
    print(f"ok   RP2350 PIO READFIFO/WRITEFIFO/SET BASE - READFIFO observed {observed}", flush=True)


def smoke_dma_one_shot_and_sizes(basic: BasicSerial, pio: int, timeout: float) -> None:
    setup_push_loop(basic, pio, timeout, name="rxsize")
    command(basic, "DIM INTEGER rx32%(1)", timeout)
    command(basic, "rx32%(0)=111:rx32%(1)=222", timeout)
    command(basic, f"PIO DMA RX {pio},0,4,rx32%(),,32", timeout)
    wait_busy_clear(basic, "MM.INFO(PIO RX DMA)", "PIO_RX32", timeout)
    text = command(basic, 'PRINT "PIO_RX32_VAL="+STR$(rx32%(0))+","+STR$(rx32%(1))', timeout)
    if "PIO_RX32_VAL=0,0" not in text:
        raise RuntimeError(f"PIO RX 32-bit data mismatch:\n{text}")

    for bits in (8, 16):
        command(basic, f"PIO STOP {pio},0", timeout, check_error=False)
        setup_push_loop(basic, pio, timeout, name=f"rx{bits}")
        command(basic, f"DIM INTEGER rxsmall{bits}%(1)", timeout)
        command(basic, f"rxsmall{bits}%(0)=&H7FFFFFFFFFFFFFFF", timeout)
        command(basic, f"PIO DMA RX {pio},0,4,rxsmall{bits}%(),,{bits}", timeout)
        wait_busy_clear(basic, "MM.INFO(PIO RX DMA)", f"PIO_RX{bits}", timeout)

    setup_pull_loop(basic, pio, timeout, name="txsize")
    command(basic, "DIM INTEGER tx32%(1)", timeout)
    command(basic, "tx32%(0)=&H2222222211111111:tx32%(1)=&H4444444433333333", timeout)
    command(basic, f"PIO DMA TX {pio},0,4,tx32%(),,32", timeout)
    wait_busy_clear(basic, "MM.INFO(PIO TX DMA)", "PIO_TX32", timeout)
    for bits in (8, 16):
        setup_pull_loop(basic, pio, timeout, name=f"tx{bits}")
        command(basic, f"DIM INTEGER txsmall{bits}%(1)", timeout)
        command(basic, f"txsmall{bits}%(0)=&H0102030405060708", timeout)
        command(basic, f"PIO DMA TX {pio},0,4,txsmall{bits}%(),,{bits}", timeout)
        wait_busy_clear(basic, "MM.INFO(PIO TX DMA)", f"PIO_TX{bits}", timeout)

    command(basic, "PIO DMA RX OFF", timeout, check_error=False)
    command(basic, "PIO DMA TX OFF", timeout, check_error=False)
    command(basic, f"PIO CLEAR {pio}", timeout, check_error=False)
    print(f"ok   PIO{pio} DMA one-shot and 8/16/32-bit sizes", flush=True)


def smoke_dma_ring_and_continuous(basic: BasicSerial, pio: int, timeout: float) -> None:
    setup_push_loop(basic, pio, timeout, name="rxring")
    command(basic, "DIM INTEGER rxring%(1)", timeout)
    command(basic, f"PIO DMA RX {pio},0,4,rxring%(),,32,4", timeout)
    wait_busy_clear(basic, "MM.INFO(PIO RX DMA)", "PIO_RX_RING", timeout)

    setup_pull_loop(basic, pio, timeout, name="txring")
    command(basic, "DIM INTEGER txring%(1)", timeout)
    command(basic, "txring%(0)=&H2222222211111111:txring%(1)=&H4444444433333333", timeout)
    command(basic, f"PIO DMA TX {pio},0,4,txring%(),,32,4", timeout)
    wait_busy_clear(basic, "MM.INFO(PIO TX DMA)", "PIO_TX_RING", timeout)

    setup_push_loop(basic, pio, timeout, name="rxcont")
    command(basic, "DIM INTEGER rxbuf%", timeout)
    command(basic, "PIO MAKE RING BUFFER rxbuf%,1024", timeout)
    command(basic, f"PIO DMA RX {pio},0,0,rxbuf%(),,32,256", timeout)
    time.sleep(0.2)
    text = command(basic, 'PRINT "PIO_RX_CONT="+STR$(MM.INFO(PIO RX DMA))', timeout)
    if "PIO_RX_CONT=1" not in text:
        raise RuntimeError(f"continuous RX DMA was not busy:\n{text}")
    command(basic, 'PRINT "PIO_RX_PTR="+STR$(MM.INFO(PIO RX DMA POINTER))', timeout)
    command(basic, "PIO DMA RX OFF", timeout, check_error=False)
    command(basic, "NEW", timeout)

    setup_pull_loop(basic, pio, timeout, name="txcont")
    command(basic, "DIM INTEGER txbuf%", timeout)
    command(basic, "PIO MAKE RING BUFFER txbuf%,1024", timeout)
    command(basic, "FOR i%=0 TO 31:txbuf%(i%)=i%:NEXT i%", timeout)
    command(basic, f"PIO DMA TX {pio},0,0,txbuf%(),,32,256", timeout)
    time.sleep(0.2)
    text = command(basic, 'PRINT "PIO_TX_CONT="+STR$(MM.INFO(PIO TX DMA))', timeout)
    if "PIO_TX_CONT=1" not in text:
        raise RuntimeError(f"continuous TX DMA was not busy:\n{text}")
    command(basic, 'PRINT "PIO_TX_PTR="+STR$(MM.INFO(PIO TX DMA POINTER))', timeout)
    command(basic, "PIO DMA TX OFF", timeout, check_error=False)
    command(basic, f"PIO CLEAR {pio}", timeout, check_error=False)
    print(f"ok   PIO{pio} DMA ring args and continuous ring buffers", flush=True)


def dma_callback_program(pio: int) -> list[str]:
    return [
        "GOTO MainStart",
        "SUB RxDone",
        "rx%=rx%+1",
        "END SUB",
        "SUB TxDone",
        "tx%=tx%+1",
        "END SUB",
        "MainStart:",
        "rx%=0:tx%=0",
        "PIO DMA RX OFF:PIO DMA TX OFF",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program cb_rx"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"push noblock"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
        "DIM INTEGER a%(1)",
        f"PIO DMA RX {pio},0,4,a%(),RxDone,32",
        "t%=TIMER",
        "DO WHILE rx%=0 AND TIMER-t%<3000:LOOP",
        'IF rx%=0 THEN ERROR "rx dma cb"',
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program cb_tx"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"pull block"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
        "DIM INTEGER b%(1)",
        "b%(0)=&H2222222211111111:b%(1)=&H4444444433333333",
        f"PIO DMA TX {pio},0,4,b%(),TxDone,32",
        "t%=TIMER",
        "DO WHILE tx%=0 AND TIMER-t%<3000:LOOP",
        'IF tx%=0 THEN ERROR "tx dma cb"',
        f"PIO CLEAR {pio}",
        'PRINT "PIO_DMA_CALLBACK_OK"',
        "END",
    ]


def pio_interrupt_program(pio: int) -> list[str]:
    return [
        "GOTO MainStart",
        "SUB OnRx",
        f"PIO READ {pio},0,1,r%",
        "rx%=rx%+1",
        f"PIO INTERRUPT {pio},0,0,0",
        "END SUB",
        "SUB OnTx",
        "tx%=tx%+1",
        f"PIO INTERRUPT {pio},0,0,0",
        "END SUB",
        "MainStart:",
        "rx%=0:tx%=0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program intrx"',
        f'PIO ASSEMBLE {pio},"loop:"',
        f'PIO ASSEMBLE {pio},"push noblock"',
        f'PIO ASSEMBLE {pio},"jmp loop"',
        f'PIO ASSEMBLE {pio},".wrap"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
        "DIM INTEGER r%",
        f"PIO INTERRUPT {pio},0,OnRx,0",
        f"PIO START {pio},0",
        "t%=TIMER",
        "DO WHILE rx%=0 AND TIMER-t%<3000:LOOP",
        'IF rx%=0 THEN ERROR "pio rx int"',
        f"PIO STOP {pio},0",
        f"PIO INTERRUPT {pio},0,0,0",
        f"PIO CLEAR {pio}",
        f'PIO ASSEMBLE {pio},".program inttx"',
        f'PIO ASSEMBLE {pio},"hold:"',
        f'PIO ASSEMBLE {pio},"jmp hold"',
        f'PIO ASSEMBLE {pio},".end program"',
        f"PIO INIT MACHINE {pio},0,1000000",
        f"PIO START {pio},0",
        f"PIO INTERRUPT {pio},0,0,OnTx",
        f"PIO WRITE {pio},0,4,1,2,3,4",
        "t%=TIMER",
        "DO WHILE TIMER-t%<100:LOOP",
        f"PIO EXECUTE {pio},0,&H80A0",
        "t%=TIMER",
        "DO WHILE tx%=0 AND TIMER-t%<3000:LOOP",
        'IF tx%=0 THEN ERROR "pio tx int"',
        f"PIO INTERRUPT {pio},0,0,0",
        f"PIO CLEAR {pio}",
        'PRINT "PIO_INTERRUPT_OK"',
        "END",
    ]


def smoke_callbacks_and_interrupts(basic: BasicSerial, pio: int, timeout: float) -> None:
    run_file_program(basic, "A:/pio_dma_cb.bas", dma_callback_program(pio), "PIO_DMA_CALLBACK_OK", timeout)
    print(f"ok   PIO{pio} DMA completion callbacks", flush=True)
    run_file_program(basic, "A:/pio_int_cb.bas", pio_interrupt_program(pio), "PIO_INTERRUPT_OK", timeout)
    print(f"ok   PIO{pio} FIFO interrupts", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port(), help="serial device path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument(
        "--all-pios",
        action="store_true",
        help="run heavier one-shot DMA cases on every available PIO, not just pio0/pio2",
    )
    args = parser.parse_args()

    with BasicSerial(args.port, args.baud) as basic:
        basic.sync(timeout=args.timeout, boot_wait=args.boot_wait)
        device = command(basic, 'PRINT "PIO_API_DEVICE="+MM.DEVICE$', args.timeout)
        available = [0, 2] if has_pio2(device) else [0]
        if args.all_pios and has_pio2(device):
            available = [0, 1, 2]
        cleanup_pio(basic, available, args.timeout)

        primary = available[0]
        smoke_functions_and_configure(basic, primary, args.timeout)
        smoke_assembler_surface(basic, primary, args.timeout)
        smoke_program_load_and_fifo(basic, primary, args.timeout)
        if has_pio2(device):
            smoke_rp2350_fifo_and_base(basic, 2, args.timeout)

        for pio in available:
            command(basic, "NEW", args.timeout)
            smoke_dma_one_shot_and_sizes(basic, pio, args.timeout)
            command(basic, "NEW", args.timeout)
            smoke_dma_ring_and_continuous(basic, pio, args.timeout)

        command(basic, "NEW", args.timeout)
        smoke_callbacks_and_interrupts(basic, primary, args.timeout)
        cleanup_pio(basic, available, args.timeout)

    total = 5 + len(available) * 2 + 2
    print(f"{total}/{total} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
