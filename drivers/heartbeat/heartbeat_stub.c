/*
 * drivers/heartbeat/heartbeat_stub.c — heartbeat-LED no-op stub.
 * Linked on ports with no MMBasic-owned heartbeat LED (host family,
 * pc386). WiFi ports link heartbeat_cyw43.c instead.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_heartbeat.h"

void hal_heartbeat_tick(void) {}

void hal_heartbeat_assert_supported(void) {
    error("Invalid configuration");
}

void hal_heartbeat_init_pins(void) {}

int hal_heartbeat_led_get(void) {
    return 0;
}

void hal_heartbeat_led_put(int on) {
    (void)on;
}
