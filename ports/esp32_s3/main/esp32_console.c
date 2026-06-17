/*
 * esp32_console.c — dual-headed serial console for the BASIC REPL.
 *
 * The console is exposed on two transports at once so a single firmware
 * image gives a usable prompt regardless of how the board wires its USB:
 *
 *   - USB Serial/JTAG: the chip's native USB pins (GPIO19/20), the same
 *     pipe used by `idf.py monitor` / `picocom /dev/cu.usbmodem*`.
 *   - UART0 (GPIO43/44): what a USB-to-UART bridge board (CP2102/CH340)
 *     routes its only connector to. On such a board the native USB is not
 *     broken out, so UART0 is the *only* place a prompt can appear.
 *
 * Output is written to both; input is polled from both. A transport with
 * no host attached simply drops its bytes (non-blocking writes / zero-
 * timeout reads), so an unplugged side never stalls the connected one.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern int esp32_usb_role_is_serial(void);

/* UART0 is free for an explicit driver because the primary IDF console is
 * USB Serial/JTAG (sdkconfig), so IDF does not own UART0. TX=GPIO43 /
 * RX=GPIO44 are the ESP32-S3 default UART0 pins. */
#define CONSOLE_UART_NUM     UART_NUM_0
#define CONSOLE_UART_TX_GPIO 43
#define CONSOLE_UART_RX_GPIO 44

/* Tracks whether this port installed the USB Serial/JTAG driver. We own the
 * state ourselves rather than querying the IDF, since the
 * usb_serial_jtag_is_driver_installed() query is unavailable on IDF v5.3. */
static bool s_jtag_driver_installed = false;
static bool s_uart_installed = false;

void esp32_console_jtag_uninstall(void) {
    if (s_jtag_driver_installed) {
        usb_serial_jtag_driver_uninstall();
        s_jtag_driver_installed = false;
    }
}

void esp32_console_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    if (!s_jtag_driver_installed) {
        usb_serial_jtag_driver_config_t cfg = {
            .tx_buffer_size = 256,
            .rx_buffer_size = 256,
        };
        if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
            s_jtag_driver_installed = true;
        }
    }
    usb_serial_jtag_vfs_use_driver();

    /* Pass raw bytes both directions — MMBasic owns line endings. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    /* Second console head on UART0 — the only console a USB-UART bridge
     * board exposes. Driven directly (not through the IDF stdio VFS, which
     * is bound to USB Serial/JTAG); esp32_console_write_bytes / the read
     * helpers fan out to both. */
    if (!s_uart_installed) {
        const uart_config_t uc = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(CONSOLE_UART_NUM, 256, 1024, 0, NULL, 0) == ESP_OK) {
            uart_param_config(CONSOLE_UART_NUM, &uc);
            uart_set_pin(CONSOLE_UART_NUM, CONSOLE_UART_TX_GPIO,
                         CONSOLE_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE);
            s_uart_installed = true;
        }
    }
}

/* ---- output hook ----
 * MMBasic's MMputchar / MMPrintString / SerialConsolePutC route every
 * byte through this helper, which fans the bytes out to both console
 * heads. UART0 always emits when installed (a bridge board's only
 * console); USB Serial/JTAG emits via stdout only while the native USB
 * port is acting as the console (not in USB KEYBOARD mode). */

void esp32_console_write_bytes(const char * text, int len) {
    if (len <= 0) return;
    if (s_uart_installed) uart_write_bytes(CONSOLE_UART_NUM, text, (size_t)len);
    if (esp32_usb_role_is_serial()) fwrite(text, 1, len, stdout);
}

/* USB Serial/JTAG is a byte-level raw pipe — no terminal line discipline,
 * no readline. Tell MMInkey to route through the byte-level reads + the
 * ANSI escape decoder rather than the line-buffered fgetc fallback. */
int esp32_console_raw_mode_is_active(void) {
    return 1;
}

/* ---- byte-level reads ----
 * esp32_console_read_byte_nonblock returns -1 when nothing's available so the
 * editor's poll loop spins without blocking. esp32_console_read_byte_blocking_ms
 * waits up to `ms` ticks (negative = forever). The pushback supports
 * the ANSI-escape decoder's one-byte lookahead. */

static int s_pushback = -1;

/* Non-blocking poll of both console heads. USB Serial/JTAG only while it
 * owns the native USB port; UART0 whenever its driver is installed.
 * Returns the byte, or -1 if neither has one ready. */
static int console_read_any_nonblock(void) {
    unsigned char c;
    if (esp32_usb_role_is_serial() && usb_serial_jtag_read_bytes(&c, 1, 0) == 1)
        return (int)c;
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
    return console_read_any_nonblock();
}

int esp32_console_read_byte_blocking_ms(int ms) {
    if (s_pushback >= 0) {
        int c = s_pushback;
        s_pushback = -1;
        return c;
    }
    if (ms == 0) return console_read_any_nonblock();

    /* Poll both heads to a deadline (ms < 0 = forever). A single zero-
     * timeout read on each can't block on one transport while bytes wait
     * on the other, so they're polled in turn with a 1-tick yield. */
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(ms);
    for (;;) {
        int c = console_read_any_nonblock();
        if (c >= 0) return c;
        if (ms > 0 && (xTaskGetTickCount() - start) >= limit) return -1;
        vTaskDelay(1);
    }
}

void esp32_console_push_back_byte(int c) {
    s_pushback = c;
}

/* MMBasic-facing console glue (MMputchar, MMPrintString, MMInkey,
 * SerialConsolePutC, ConsoleRxBuf*, MMgetline, …) lives in
 * esp32_mmbasic_console_glue.c — kept in a separate TU because pulling
 * in MMBasic_Includes.h here clashes with IDF's vfs.h DIR typedef. */
