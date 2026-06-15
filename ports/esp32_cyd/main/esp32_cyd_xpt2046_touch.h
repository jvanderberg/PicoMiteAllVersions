/*
 * CYD XPT2046 resistive touch helper.
 */

#ifndef ESP32_CYD_XPT2046_TOUCH_H
#define ESP32_CYD_XPT2046_TOUCH_H

void esp32_cyd_xpt2046_touch_init(void);
int esp32_cyd_xpt2046_touch_is_ready(void);
int esp32_cyd_xpt2046_touch_read(int index, int * x, int * y);
int esp32_cyd_xpt2046_touch_read_raw_mapped(int index, int * x, int * y);
int esp32_cyd_xpt2046_touch_down(void);
void esp32_cyd_xpt2046_touch_set_default_calibration(void);
void esp32_cyd_xpt2046_touch_set_identity_calibration(void);

#endif /* ESP32_CYD_XPT2046_TOUCH_H */
