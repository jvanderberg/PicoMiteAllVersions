/*
 * Board-neutral ESP32 touch dispatch.
 */

#ifndef ESP32_TOUCH_PORT_H
#define ESP32_TOUCH_PORT_H

void esp32_touch_port_init(void);
int esp32_touch_port_is_ready(void);
int esp32_touch_port_read(int index, int * x, int * y);
int esp32_touch_port_read_raw_mapped(int index, int * x, int * y);
int esp32_touch_port_down(void);
void esp32_touch_port_set_default_calibration(void);
void esp32_touch_port_set_identity_calibration(void);

#endif /* ESP32_TOUCH_PORT_H */
