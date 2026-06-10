/*
 * hal/hal_heartbeat.h — onboard heartbeat-LED service hook.
 *
 * Real impl in drivers/heartbeat/heartbeat_real.c (linked on ports
 * whose heartbeat LED is owned by GPIO) toggles the configured GPIO
 * once per second. drivers/heartbeat/heartbeat_cyw43.c is the WiFi-port
 * impl (the LED hangs off the CYW43 radio; cadence is driven from the
 * network service poll through the led_get/led_put hooks below). Stub
 * in drivers/heartbeat/heartbeat_stub.c is a no-op (linked on ports
 * with no MMBasic-owned LED — host family, pc386). Linkage is selected
 * per-port in port_sources.cmake / the port Makefile.
 */

#ifndef HAL_HEARTBEAT_H
#define HAL_HEARTBEAT_H

#ifdef __cplusplus
extern "C" {
#endif

void hal_heartbeat_tick(void);

/* Error if the port doesn't support a heartbeat LED — called from
 * the EXT_HEARTBEAT case in External.c's ExtCfg dispatch. Real impl
 * is a no-op; stub errors with "Invalid configuration". */
void hal_heartbeat_assert_supported(void);

/* Boot-time pin claim for the heartbeat LED + the CYW43-shadow pins
 * (41/42/44) on PicoMite/VGA/HDMI/PSRAM ports. WiFi ports stub this
 * out — the CYW43 radio owns those pins. */
void hal_heartbeat_init_pins(void);

/* Raw LED state access for ports whose blink cadence is driven from an
 * external poll loop instead of hal_heartbeat_tick() — WiFi ports blink
 * from the network service poll (ProcessWeb in shared/net/MMtelnet.c),
 * with the LED itself on a CYW43 GPIO. Real on CYW43 ports; no-op
 * (get returns 0) elsewhere. */
int hal_heartbeat_led_get(void);
void hal_heartbeat_led_put(int on);

#ifdef __cplusplus
}
#endif

#endif /* HAL_HEARTBEAT_H */
