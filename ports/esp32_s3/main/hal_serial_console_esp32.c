/*
 * hal_serial_console_esp32.c - raw serial-console byte transfer for XMODEM.
 *
 * The S3 USB-Serial/JTAG console and the classic ESP32 CYD UART0 console both
 * expose the same esp32_console_* byte primitives. XMODEM polls readable()
 * before getc(), so keep one byte of local lookahead instead of consuming a
 * byte in readable() and losing it.
 */

#include <stdbool.h>

#include "hal/hal_serial_console.h"

extern void esp32_console_write_bytes(const char * text, int len);
extern int esp32_console_read_byte_nonblock(void);
extern int esp32_console_read_byte_blocking_ms(int ms);

static int s_raw_peek = -1;

bool hal_serial_console_raw_enter(void) {
    s_raw_peek = -1;
    return true;
}

void hal_serial_console_raw_exit(void) {
    s_raw_peek = -1;
}

bool hal_serial_console_raw_readable(void) {
    if (s_raw_peek >= 0) return true;
    s_raw_peek = esp32_console_read_byte_nonblock();
    return s_raw_peek >= 0;
}

int hal_serial_console_raw_getc(void) {
    if (s_raw_peek >= 0) {
        int c = s_raw_peek;
        s_raw_peek = -1;
        return c;
    }
    return esp32_console_read_byte_blocking_ms(-1);
}

void hal_serial_console_raw_putc(char c) {
    esp32_console_write_bytes(&c, 1);
}
