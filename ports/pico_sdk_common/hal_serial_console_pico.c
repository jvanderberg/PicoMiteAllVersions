/* Raw serial-console byte I/O for XMODEM transfers. The console UART
 * is uart0 when (Option.SerialConsole & 3) == 1, otherwise uart1. */

#include "hardware/uart.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_serial_console.h"

static uart_inst_t * console_uart(void) {
    return (Option.SerialConsole & 3) == 1 ? uart0 : uart1;
}

bool hal_serial_console_raw_enter(void) {
    if (!Option.SerialConsole) return false;
    uart_set_irq_enables(console_uart(), false, false);
    return true;
}

void hal_serial_console_raw_exit(void) {
    uart_set_irq_enables(console_uart(), true, false);
}

bool hal_serial_console_raw_readable(void) {
    return uart_is_readable(console_uart());
}

int hal_serial_console_raw_getc(void) {
    return (unsigned char)uart_getc(console_uart());
}

void hal_serial_console_raw_putc(char c) {
    uart_putc_raw(console_uart(), c);
}
