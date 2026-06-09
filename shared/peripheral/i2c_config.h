#ifndef I2C_CONFIG_H
#define I2C_CONFIG_H

#include <stdint.h>

/*
 * shared/peripheral/i2c_config.h — SDA/SCL pin assignments for the shared
 * I²C bus driver, plus the SETPIN pin-config and pin-OFF reset that every
 * port reaches identically.
 *
 * 99 = unset. The shared driver (drivers/i2c_bus/I2C.c) reads these to
 * resolve the bus pins and guards I2C OPEN on the 99 sentinel.
 */
extern uint8_t I2C0SDApin;
extern uint8_t I2C0SCLpin;
extern uint8_t I2C1SDApin;
extern uint8_t I2C1SCLpin;

/* True when `cfg` is one of the four I²C SETPIN modes (EXT_I2C0SDA ..
 * EXT_I2C1SCL / VM_PIN_MODE_I2C0SDA .. VM_PIN_MODE_I2C1SCL — the two
 * enumerations share the same numeric codes). */
int i2c_config_mode_is_i2c(int cfg);

/* Apply an I²C SETPIN mode to `pin`: validate the pin's I²C capability and
 * the SYSTEM-I2C lock, record the SDA/SCL assignment, and route the GPIO to
 * its I²C function. Errors (via error()) on an invalid pin, a locked bus, or
 * a conflicting prior assignment. `cfg` must satisfy i2c_config_mode_is_i2c. */
void i2c_config_setpin(int pin, int cfg);

/* Release `pin` from any I²C SDA/SCL assignment (turns the matching global
 * back to 99). Safe to call for any pin; non-I²C pins are left untouched. */
void i2c_config_clear_pin(int pin);

/* Tear down a configured SYSTEM I²C bus: release its SDA/SCL pins back to
 * EXT_DIG_IN / EXT_NOT_CONFIG and zero Option.SYSTEM_I2C_SDA/SCL. Called from
 * ResetOptions() and from the OPTION SYSTEM I2C DISABLE path. */
void disable_systemi2c(void);

/* Handle the `OPTION SYSTEM I2C …` command tail (`tp` points just past the
 * "SYSTEM I2C" keyword). Parses sda,scl[,SLOW|FAST] or DISABLE, validates the
 * pins, records Option.SYSTEM_I2C_SDA/SCL/SLOW, persists via SaveOptions(),
 * and soft-resets. Errors (via error()) on a syntax/pin/in-use violation.
 * Reached from every port's OPTION dispatcher. */
void i2c_config_option_system_i2c(unsigned char * tp);

/* Emit the `OPTION SYSTEM I2C …` line for OPTION LIST, when a SYSTEM I²C bus
 * is configured. No-op when none is set. */
void i2c_config_print_option(void);

#endif
