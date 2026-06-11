/*
 * hal/hal_serial_console.h — serial-console raw byte-transfer surface.
 *
 * XMODEM transfers over a UART serial console bypass the
 * interrupt-driven console rx queue: while a transfer is active the
 * console rx interrupt is masked and bytes move by polling the UART
 * directly. Real impl in ports/pico_sdk_common/hal_serial_console_pico.c
 * selects the console UART from Option.SerialConsole. Ports without a
 * UART serial console (pc386) provide a stub whose enter() returns
 * false, which keeps transfers on the normal buffered console path.
 */

#ifndef HAL_SERIAL_CONSOLE_H
#define HAL_SERIAL_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mask the serial-console rx interrupt and switch to polled raw I/O.
 * Returns true when a UART serial console is active and raw mode is
 * engaged; returns false (changing nothing) when console I/O should
 * continue through the normal buffered path. */
bool hal_serial_console_raw_enter(void);

/* Re-enable the serial-console rx interrupt. Call only after a
 * successful hal_serial_console_raw_enter(). */
void hal_serial_console_raw_exit(void);

/* True when a byte is waiting in the serial-console rx hardware. */
bool hal_serial_console_raw_readable(void);

/* Read one byte (0-255) directly from the serial console. Blocks until
 * a byte arrives; poll hal_serial_console_raw_readable() first to
 * avoid blocking. */
int hal_serial_console_raw_getc(void);

/* Write one byte directly to the serial console, bypassing console
 * mirroring and cursor-position bookkeeping. */
void hal_serial_console_raw_putc(char c);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SERIAL_CONSOLE_H */
