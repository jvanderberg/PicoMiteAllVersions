/*
 * Board-neutral ESP32 touch dispatch.
 */

#include "esp32_touch_port.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "esp32_board_profile.h"
#include "esp32_ft6336u_touch.h"

#if defined(CONFIG_IDF_TARGET_ESP32)
#include "esp32_cyd_xpt2046_touch.h"
#endif

static int is_cyd_touch(void) {
#if defined(CONFIG_IDF_TARGET_ESP32)
    return esp32_board_profile_current_id() == ESP32_BOARD_PROFILE_ID_CYD;
#else
    return 0;
#endif
}

void esp32_touch_port_init(void) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        esp32_cyd_xpt2046_touch_init();
#endif
    } else {
        esp32_ft6336u_touch_init();
    }
}

int esp32_touch_port_is_ready(void) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return esp32_cyd_xpt2046_touch_is_ready();
#else
        return 0;
#endif
    }
    return esp32_ft6336u_touch_is_ready();
}

int esp32_touch_port_read(int index, int * x, int * y) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return esp32_cyd_xpt2046_touch_read(index, x, y);
#else
        return 0;
#endif
    }
    return esp32_ft6336u_touch_read(index, x, y);
}

int esp32_touch_port_read_raw_mapped(int index, int * x, int * y) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return esp32_cyd_xpt2046_touch_read_raw_mapped(index, x, y);
#else
        return 0;
#endif
    }
    return esp32_ft6336u_touch_read_raw_mapped(index, x, y);
}

int esp32_touch_port_down(void) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return esp32_cyd_xpt2046_touch_down();
#else
        return 0;
#endif
    }
    return esp32_ft6336u_touch_down();
}

void esp32_touch_port_set_default_calibration(void) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        esp32_cyd_xpt2046_touch_set_default_calibration();
#endif
    } else {
        esp32_ft6336u_touch_set_default_calibration();
    }
}

void esp32_touch_port_set_identity_calibration(void) {
    if (is_cyd_touch()) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        esp32_cyd_xpt2046_touch_set_identity_calibration();
#endif
    } else {
        esp32_ft6336u_touch_set_identity_calibration();
    }
}
