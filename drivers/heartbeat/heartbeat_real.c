/*
 * drivers/heartbeat/heartbeat_real.c — real heartbeat-LED toggle.
 * Linked on ports with an onboard heartbeat LED owned by GPIO.
 * Mutually exclusive with heartbeat_cyw43.c (WiFi ports, LED on the
 * CYW43 module) and heartbeat_stub.c (no LED).
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_heartbeat.h"
#include "hal/hal_pin.h"
#include "hardware/gpio.h"

void hal_heartbeat_tick(void) {
    if (ExtCurrentConfig[PinDef[HEARTBEATpin].pin] == EXT_HEARTBEAT) {
        gpio_xor_mask64(1ULL << PinDef[HEARTBEATpin].GPno);
    }
}

void hal_heartbeat_assert_supported(void) { /* heartbeat LED present, OK. */ }

void hal_heartbeat_init_pins(void) {
    if (!Option.AllPins) {
        if (CheckPin(41, CP_NOABORT | CP_IGNORE_INUSE | CP_IGNORE_RESERVED))
            ExtCfg(41, EXT_DIG_OUT, Option.PWM);
        if (CheckPin(42, CP_NOABORT | CP_IGNORE_INUSE | CP_IGNORE_RESERVED))
            ExtCfg(42, EXT_DIG_IN, 0);
        if (CheckPin(44, CP_NOABORT | CP_IGNORE_INUSE | CP_IGNORE_RESERVED))
            ExtCfg(44, EXT_ANA_IN, 0);
    }
    if (CheckPin(HEARTBEATpin, CP_NOABORT | CP_IGNORE_INUSE | CP_IGNORE_RESERVED) && !Option.NoHeartbeat) {
        hal_pin_init_digital(PinDef[HEARTBEATpin].GPno);
        hal_pin_set_dir(PinDef[HEARTBEATpin].GPno, HAL_PIN_DIR_OUT);
        ExtCurrentConfig[PinDef[HEARTBEATpin].pin] = EXT_HEARTBEAT;
    }
}

/* Cadence on GPIO-LED ports is hal_heartbeat_tick(); the raw led hooks
 * are only driven by poll loops on CYW43 ports. */
int hal_heartbeat_led_get(void) {
    return 0;
}

void hal_heartbeat_led_put(int on) {
    (void)on;
}
