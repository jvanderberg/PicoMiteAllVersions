/*
 * shared/peripheral/uart_config.c — UART SETPIN pin config + TX/RX globals.
 *
 * One copy compiled for every port. Owns the four UART TX/RX pin
 * assignments the shared serial driver (drivers/serial/Serial.c) reads, the
 * SETPIN mode handling (EXT_UART0TX .. EXT_UART1RX), and the pin-OFF reset.
 * The interpreter's SETPIN path (core/mmbasic/External.c), the device VM
 * pin syscall (runtime/vm/vm_sys_pin.c), and the simulator pin syscall
 * (ports/vm_sys_sim/vm_sys_pin_sim.c) all route their UART modes here, so
 * every port hits identical logic. All hardware specifics live behind
 * hal_pin_set_function / hal_pin_set_pulls.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "uart_config.h"
#include "hal/hal_pin.h"

uint8_t UART1RXpin = 99;
uint8_t UART1TXpin = 99;
uint8_t UART0TXpin = 99;
uint8_t UART0RXpin = 99;

int uart_config_mode_is_uart(int cfg) {
    return cfg >= EXT_UART0TX && cfg <= EXT_UART1RX;
}

void uart_config_setpin(int pin, int cfg) {
    switch (cfg) {
    case EXT_UART0TX:
        if (!(PinDef[pin].mode & UART0TX)) error("Invalid configuration");
        if ((UART0TXpin != 99)) error("Already Set to pin %", UART0TXpin);
        UART0TXpin = pin;
        break;
    case EXT_UART0RX:
        if (!(PinDef[pin].mode & UART0RX)) error("Invalid configuration");
        if ((UART0RXpin != 99)) error("Already Set to pin %", UART0RXpin);
        UART0RXpin = pin;
        break;
    case EXT_UART1TX:
        if (!(PinDef[pin].mode & UART1TX)) error("Invalid configuration");
        if ((UART1TXpin != 99)) error("Already Set to pin %", UART1TXpin);
        UART1TXpin = pin;
        break;
    case EXT_UART1RX:
        if (!(PinDef[pin].mode & UART1RX)) error("Invalid configuration");
        if ((UART1RXpin != 99)) error("Already Set to pin %", UART1RXpin);
        UART1RXpin = pin;
        break;
    default:
        error("Invalid configuration");
        return;
    }
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_UART);
    if (cfg == EXT_UART0RX || cfg == EXT_UART1RX) hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_UP);
}

void uart_config_clear_pin(int pin) {
    if (pin == UART0TXpin) UART0TXpin = 99;
    if (pin == UART0RXpin) UART0RXpin = 99;
    if (pin == UART1RXpin) UART1RXpin = 99;
    if (pin == UART1TXpin) UART1TXpin = 99;
}
