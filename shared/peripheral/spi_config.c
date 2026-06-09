/*
 * shared/peripheral/spi_config.c — SPI SETPIN pin config + TX/RX/SCK globals.
 *
 * One copy compiled for every port. Owns the six SPI TX/RX/SCK pin
 * assignments the shared bus driver (drivers/spi_bus/SPI.c) reads, the
 * SETPIN mode handling (EXT_SPI0RX .. EXT_SPI1SCK), and the pin-OFF reset.
 * The interpreter's SETPIN path (core/mmbasic/External.c), the device VM
 * pin syscall (runtime/vm/vm_sys_pin.c), and the simulator pin syscall
 * (ports/vm_sys_sim/vm_sys_pin_sim.c) all route their SPI modes here, so
 * every port hits identical logic. SPI0locked / SPI1locked are defined per
 * port (host_runtime.c, pc386_state.c, esp32) and referenced via extern.
 * All hardware specifics live behind hal_pin_set_function.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "spi_config.h"
#include "hal/hal_pin.h"

uint8_t SPI1TXpin = 99;
uint8_t SPI1RXpin = 99;
uint8_t SPI1SCKpin = 99;
uint8_t SPI0TXpin = 99;
uint8_t SPI0RXpin = 99;
uint8_t SPI0SCKpin = 99;

int spi_config_mode_is_spi(int cfg) {
    return cfg >= EXT_SPI0RX && cfg <= EXT_SPI1SCK;
}

void spi_config_setpin(int pin, int cfg) {
    switch (cfg) {
    case EXT_SPI0TX:
        if (!(PinDef[pin].mode & SPI0TX)) error("Invalid configuration");
        if (SPI0locked) error("SPI in use for SYSTEM SPI");
        if ((SPI0TXpin != 99 && SPI0TXpin != pin)) error("Already Set to pin %", SPI0TXpin);
        SPI0TXpin = pin;
        break;
    case EXT_SPI0RX:
        if (!(PinDef[pin].mode & SPI0RX)) error("Invalid configuration");
        if (SPI0locked) error("SPI in use for SYSTEM SPI");
        if ((SPI0RXpin != 99 && SPI0RXpin != pin)) error("Already Set to pin %", SPI0RXpin);
        SPI0RXpin = pin;
        break;
    case EXT_SPI0SCK:
        if (!(PinDef[pin].mode & SPI0SCK)) error("Invalid configuration");
        if (SPI0locked) error("SPI in use for SYSTEM SPI");
        if ((SPI0SCKpin != 99 && SPI0SCKpin != pin)) error("Already Set to pin %", SPI0SCKpin);
        SPI0SCKpin = pin;
        break;
    case EXT_SPI1TX:
        if (!(PinDef[pin].mode & SPI1TX)) error("Invalid configuration");
        if (SPI1locked) error("SPI2 in use for SYSTEM SPI");
        if ((SPI1TXpin != 99 && SPI1TXpin != pin)) error("Already Set to pin %", SPI1TXpin);
        SPI1TXpin = pin;
        break;
    case EXT_SPI1RX:
        if (!(PinDef[pin].mode & SPI1RX)) error("Invalid configuration");
        if (SPI1locked) error("SPI2 in use for SYSTEM SPI");
        if ((SPI1RXpin != 99 && SPI1RXpin != pin)) error("Already Set to pin %", SPI1RXpin);
        SPI1RXpin = pin;
        break;
    case EXT_SPI1SCK:
        if (!(PinDef[pin].mode & SPI1SCK)) error("Invalid configuration");
        if (SPI1locked) error("SPI2 in use for SYSTEM SPI");
        if ((SPI1SCKpin != 99 && SPI1SCKpin != pin)) error("Already Set to pin %", SPI1SCKpin);
        SPI1SCKpin = pin;
        break;
    default:
        error("Invalid configuration");
        return;
    }
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_SPI);
}

void spi_config_clear_pin(int pin) {
    if (pin == SPI1TXpin) SPI1TXpin = 99;
    if (pin == SPI1RXpin) SPI1RXpin = 99;
    if (pin == SPI1SCKpin) SPI1SCKpin = 99;
    if (pin == SPI0TXpin) SPI0TXpin = 99;
    if (pin == SPI0RXpin) SPI0RXpin = 99;
    if (pin == SPI0SCKpin) SPI0SCKpin = 99;
}
