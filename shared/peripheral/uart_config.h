#ifndef UART_CONFIG_H
#define UART_CONFIG_H

#include <stdint.h>

/*
 * shared/peripheral/uart_config.h — TX/RX pin assignments for the shared
 * serial driver, plus the SETPIN pin-config and pin-OFF reset that every
 * port reaches identically.
 *
 * 99 = unset. The shared driver (drivers/serial/Serial.c) reads these to
 * resolve the COM port pins and guards OPEN on the 99 sentinel.
 */
extern uint8_t UART0TXpin;
extern uint8_t UART0RXpin;
extern uint8_t UART1TXpin;
extern uint8_t UART1RXpin;

/* True when `cfg` is one of the four UART SETPIN modes (EXT_UART0TX ..
 * EXT_UART1RX / VM_PIN_MODE_UART0TX .. VM_PIN_MODE_UART1RX — the two
 * enumerations share the same numeric codes). */
int uart_config_mode_is_uart(int cfg);

/* Apply a UART SETPIN mode to `pin`: validate the pin's UART capability,
 * record the TX/RX assignment, and route the GPIO to its UART function
 * (RX pins also get a pull-up). Errors (via error()) on an invalid pin or a
 * conflicting prior assignment. `cfg` must satisfy uart_config_mode_is_uart. */
void uart_config_setpin(int pin, int cfg);

/* Release `pin` from any UART TX/RX assignment (turns the matching global
 * back to 99). Safe to call for any pin; non-UART pins are left untouched. */
void uart_config_clear_pin(int pin);

#endif
