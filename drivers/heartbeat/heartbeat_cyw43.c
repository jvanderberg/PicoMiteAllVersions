/*
 * drivers/heartbeat/heartbeat_cyw43.c — CYW43-owned heartbeat LED.
 * Linked on WiFi ports, where the onboard LED hangs off the CYW43
 * radio's GPIO. The blink cadence is driven from the network service
 * poll (ProcessWeb in shared/net/MMtelnet.c), so hal_heartbeat_tick()
 * is a no-op here and the poll loop drives the LED through
 * hal_heartbeat_led_get() / hal_heartbeat_led_put().
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_heartbeat.h"
#include "pico/cyw43_arch.h"

void hal_heartbeat_tick(void) {}

void hal_heartbeat_assert_supported(void) {
    error("Invalid configuration");
}

void hal_heartbeat_init_pins(void) {
    /* CYW43 owns the LED and the shadow pins — leave them for the radio. */
}

int hal_heartbeat_led_get(void) {
    return cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN);
}

void hal_heartbeat_led_put(int on) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}
