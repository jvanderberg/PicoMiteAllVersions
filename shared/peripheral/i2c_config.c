/*
 * shared/peripheral/i2c_config.c — I²C SETPIN pin config + SDA/SCL globals.
 *
 * One copy compiled for every port. Owns the four I²C SDA/SCL pin
 * assignments the shared bus driver (drivers/i2c_bus/I2C.c) reads, the
 * SETPIN mode handling (EXT_I2C0SDA .. EXT_I2C1SCL), and the pin-OFF reset.
 * The interpreter's SETPIN path (core/mmbasic/External.c), the device VM
 * pin syscall (runtime/vm/vm_sys_pin.c), and the simulator pin syscall
 * (ports/vm_sys_sim/vm_sys_pin_sim.c) all route their I²C modes here, so
 * every port hits identical logic. All hardware specifics live behind
 * hal_pin_set_function.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "i2c_config.h"
#include "hal/hal_pin.h"

uint8_t I2C1SDApin = 99;
uint8_t I2C1SCLpin = 99;
uint8_t I2C0SDApin = 99;
uint8_t I2C0SCLpin = 99;

int i2c_config_mode_is_i2c(int cfg) {
    return cfg >= EXT_I2C0SDA && cfg <= EXT_I2C1SCL;
}

void i2c_config_setpin(int pin, int cfg) {
    switch (cfg) {
    case EXT_I2C0SDA:
        if (!(PinDef[pin].mode & I2C0SDA)) error("Invalid configuration");
        if (I2C0locked) error("I2C in use for SYSTEM I2C");
        if ((I2C0SDApin != 99 && I2C0SDApin != pin)) error("Already Set to pin %", I2C0SDApin);
        I2C0SDApin = pin;
        break;
    case EXT_I2C0SCL:
        if (!(PinDef[pin].mode & I2C0SCL)) error("Invalid configuration");
        if (I2C0locked) error("I2C in use for SYSTEM I2C");
        if ((I2C0SCLpin != 99 && I2C0SCLpin != pin)) error("Already Set to pin %", I2C0SCLpin);
        I2C0SCLpin = pin;
        break;
    case EXT_I2C1SDA:
        if (!(PinDef[pin].mode & I2C1SDA)) error("Invalid configuration");
        if (I2C1locked) error("I2C2 in use for SYSTEM I2C");
        if ((I2C1SDApin != 99) && I2C1SDApin != pin) error("Already Set to pin %", I2C1SDApin);
        I2C1SDApin = pin;
        break;
    case EXT_I2C1SCL:
        if (!(PinDef[pin].mode & I2C1SCL)) error("Invalid configuration");
        if (I2C1locked) error("I2C2 in use for SYSTEM I2C");
        if ((I2C1SCLpin != 99 && I2C1SCLpin != pin)) error("Already Set to pin %", I2C1SCLpin);
        I2C1SCLpin = pin;
        break;
    default:
        error("Invalid configuration");
        return;
    }
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_I2C);
}

void i2c_config_clear_pin(int pin) {
    if (pin == I2C1SDApin) I2C1SDApin = 99;
    if (pin == I2C1SCLpin) I2C1SCLpin = 99;
    if (pin == I2C0SDApin) I2C0SDApin = 99;
    if (pin == I2C0SCLpin) I2C0SCLpin = 99;
}

void MIPS16 disable_systemi2c(void) {
    if (!IsInvalidPin(Option.SYSTEM_I2C_SCL)) ExtCurrentConfig[Option.SYSTEM_I2C_SCL] = EXT_DIG_IN;
    if (!IsInvalidPin(Option.SYSTEM_I2C_SDA)) ExtCurrentConfig[Option.SYSTEM_I2C_SDA] = EXT_DIG_IN;
    if (!IsInvalidPin(Option.SYSTEM_I2C_SCL)) ExtCfg(Option.SYSTEM_I2C_SCL, EXT_NOT_CONFIG, 0);
    if (!IsInvalidPin(Option.SYSTEM_I2C_SDA)) ExtCfg(Option.SYSTEM_I2C_SDA, EXT_NOT_CONFIG, 0);
    Option.SYSTEM_I2C_SCL = 0;
    Option.SYSTEM_I2C_SDA = 0;
}

void i2c_config_option_system_i2c(unsigned char * tp) {
    int pin1, pin2, channel = -1;
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        if (CurrentLinePtr) error("Invalid in a program");
        /* SSD1306I2C panels don't exist on VGA (the OPTION setter rejects
     * the type), so the runtime DISPLAY_TYPE check is sufficient on
     * every port. */
        if (Option.RTC_Clock || Option.RTC_Data) error("In use");
        if (Option.DISPLAY_TYPE == SSD1306I2C || Option.DISPLAY_TYPE == SSD1306I2C32)
            error("In use");
        disable_systemi2c();
        SaveOptions();
        _excep_code = RESET_COMMAND;
        SoftReset();
        return; // this will restart the processor ? only works when not in debug
    }
    getargs(&tp, 5, (unsigned char *)",");
    if (CurrentLinePtr) error("Invalid in a program");
    if (argc < 3) error("Syntax");
    if (Option.SYSTEM_I2C_SCL) error("I2C already configured");
    unsigned char code;
    if (!(code = codecheck(argv[0]))) argv[0] += 2;
    pin1 = getinteger(argv[0]);
    if (!code) pin1 = codemap(pin1);
    if (IsInvalidPin(pin1)) error("Invalid pin");
    if (ExtCurrentConfig[pin1] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin1, pin1);
    if (!(code = codecheck(argv[2]))) argv[2] += 2;
    pin2 = getinteger(argv[2]);
    if (!code) pin2 = codemap(pin2);
    if (IsInvalidPin(pin2)) error("Invalid pin");
    if (ExtCurrentConfig[pin2] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin2, pin2);
    if (PinDef[pin1].mode & I2C0SDA && PinDef[pin2].mode & I2C0SCL) channel = 0;
    if (PinDef[pin1].mode & I2C1SDA && PinDef[pin2].mode & I2C1SCL) channel = 1;
    if (channel == -1) error("Invalid I2C pins");
    Option.SYSTEM_I2C_SLOW = 0;
    if (argc == 5) {
        if (checkstring(argv[4], (unsigned char *)"SLOW"))
            Option.SYSTEM_I2C_SLOW = 1;
        else if (checkstring(argv[4], (unsigned char *)"FAST"))
            Option.SYSTEM_I2C_SLOW = 0;
        else
            error("Syntax");
    }
    Option.SYSTEM_I2C_SDA = pin1;
    Option.SYSTEM_I2C_SCL = pin2;
    SaveOptions();
    _excep_code = RESET_COMMAND;
    SoftReset();
    return;
}

void i2c_config_print_option(void) {
    if (Option.SYSTEM_I2C_SDA) {
        MMPrintString("OPTION SYSTEM I2C ");
        MMPrintString((char *)PinDef[Option.SYSTEM_I2C_SDA].pinname);
        MMputchar(',', 1);
        MMPrintString((char *)PinDef[Option.SYSTEM_I2C_SCL].pinname);
        if (Option.SYSTEM_I2C_SLOW)
            MMPrintString(", SLOW\r\n");
        else
            MMPrintString("\r\n");
    }
}
