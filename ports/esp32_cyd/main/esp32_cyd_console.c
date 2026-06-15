/*
 * esp32_cyd_console.c — UART0 console for the BASIC REPL.
 *
 * CYD-class boards route their only connector through a USB-UART bridge
 * (CH340/CP2102) wired to UART0, and classic ESP32 has no USB Serial/JTAG
 * controller, so UART0 is the console transport. The driver keeps the
 * chip's default UART0 pins (TX=GPIO1, RX=GPIO3).
 *
 * MMBasic-facing console glue (MMputchar, MMPrintString, MMInkey,
 * SerialConsolePutC, ConsoleRxBuf*, MMgetline, …) lives in
 * esp32_mmbasic_console_glue.c and consumes the byte-level entry points
 * below.
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONSOLE_UART_NUM UART_NUM_0
#define CONSOLE_UART_BAUD 115200

static bool s_uart_installed = false;

void esp32_console_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    const uart_config_t uc = {
        .baud_rate = CONSOLE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(CONSOLE_UART_NUM, 1024, 1024, 0, NULL, 0) == ESP_OK) {
        uart_param_config(CONSOLE_UART_NUM, &uc);
        uart_set_pin(CONSOLE_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        s_uart_installed = true;
    }
}

/* ---- output hook ----
 * MMBasic's MMputchar / MMPrintString / SerialConsolePutC route every
 * byte through this helper. */

void esp32_console_write_bytes(const char * text, int len) {
    if (len <= 0 || !s_uart_installed) return;
    uart_write_bytes(CONSOLE_UART_NUM, text, (size_t)len);
}

/* The UART is a byte-level raw pipe — no terminal line discipline, no
 * readline. Tell MMInkey to route through the byte-level reads + the ANSI
 * escape decoder rather than the line-buffered fgetc fallback. */
int esp32_console_raw_mode_is_active(void) {
    return 1;
}

/* ---- byte-level reads ----
 * esp32_console_read_byte_nonblock returns -1 when nothing's available so
 * the editor's poll loop spins without blocking.
 * esp32_console_read_byte_blocking_ms waits up to `ms` ticks (negative =
 * forever). The pushback supports the ANSI-escape decoder's one-byte
 * lookahead. */

static int s_pushback = -1;

static int console_read_nonblock(void) {
    unsigned char c;
    if (s_uart_installed && uart_read_bytes(CONSOLE_UART_NUM, &c, 1, 0) == 1)
        return (int)c;
    return -1;
}

int esp32_console_read_byte_nonblock(void) {
    if (s_pushback >= 0) {
        int c = s_pushback;
        s_pushback = -1;
        return c;
    }
    return console_read_nonblock();
}

int esp32_console_read_byte_blocking_ms(int ms) {
    if (s_pushback >= 0) {
        int c = s_pushback;
        s_pushback = -1;
        return c;
    }
    if (ms == 0) return console_read_nonblock();

    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(ms);
    for (;;) {
        int c = console_read_nonblock();
        if (c >= 0) return c;
        if (ms > 0 && (xTaskGetTickCount() - start) >= limit) return -1;
        vTaskDelay(1);
    }
}

void esp32_console_push_back_byte(int c) {
    s_pushback = c;
}
