#ifndef SPI_CONFIG_H
#define SPI_CONFIG_H

#include <stdint.h>

/*
 * shared/peripheral/spi_config.h — TX/RX/SCK pin assignments for the shared
 * SPI bus driver, plus the SETPIN pin-config and pin-OFF reset that every
 * port reaches identically.
 *
 * 99 = unset. The shared driver (drivers/spi_bus/SPI.c) reads these to
 * resolve the bus pins and guards SPI OPEN on the 99 sentinel. SPI0locked /
 * SPI1locked (declared in External.h, defined per port) gate the SETPIN path
 * while a bus is held for SYSTEM SPI.
 */
extern uint8_t SPI0TXpin;
extern uint8_t SPI0RXpin;
extern uint8_t SPI0SCKpin;
extern uint8_t SPI1TXpin;
extern uint8_t SPI1RXpin;
extern uint8_t SPI1SCKpin;

/* True when `cfg` is one of the six SPI SETPIN modes (EXT_SPI0RX ..
 * EXT_SPI1SCK / VM_PIN_MODE_SPI0RX .. VM_PIN_MODE_SPI1SCK — the two
 * enumerations share the same numeric codes). */
int spi_config_mode_is_spi(int cfg);

/* Apply an SPI SETPIN mode to `pin`: validate the pin's SPI capability and
 * the SYSTEM-SPI lock, record the TX/RX/SCK assignment, and route the GPIO
 * to its SPI function. Errors (via error()) on an invalid pin, a locked bus,
 * or a conflicting prior assignment. `cfg` must satisfy spi_config_mode_is_spi. */
void spi_config_setpin(int pin, int cfg);

/* Release `pin` from any SPI TX/RX/SCK assignment (turns the matching global
 * back to 99). Safe to call for any pin; non-SPI pins are left untouched. */
void spi_config_clear_pin(int pin);

#endif
