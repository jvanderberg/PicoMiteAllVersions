/*
 * ESP32-S3 TOUCH() function surface.
 *
 * Direct TOUCH() path over the active board touch driver.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_gui_controls.h"
#include "esp32_touch_port.h"

#define ESP32_TOUCH_ERROR -1

static int read_axis(int index, int want_y) {
    int x = 0, y = 0;
    if (!esp32_touch_port_read(index, &x, &y)) return ESP32_TOUCH_ERROR;
    return want_y ? y : x;
}

static int read_raw_axis(int index, int want_y) {
    int x = 0, y = 0;
    if (!esp32_touch_port_read_raw_mapped(index, &x, &y))
        return ESP32_TOUCH_ERROR;
    return want_y ? y : x;
}

void fun_touch(void) {
    if (checkstring(ep, (unsigned char *)"X"))
        iret = read_axis(0, 0);
    else if (checkstring(ep, (unsigned char *)"Y"))
        iret = read_axis(0, 1);
    else if (checkstring(ep, (unsigned char *)"RAWX"))
        iret = read_raw_axis(0, 0);
    else if (checkstring(ep, (unsigned char *)"RAWY"))
        iret = read_raw_axis(0, 1);
    else if (checkstring(ep, (unsigned char *)"X2"))
        iret = read_axis(1, 0);
    else if (checkstring(ep, (unsigned char *)"Y2"))
        iret = read_axis(1, 1);
    else if (checkstring(ep, (unsigned char *)"DOWN"))
        iret = esp32_touch_port_down() ? 1 : 0;
    else if (checkstring(ep, (unsigned char *)"UP"))
        iret = esp32_touch_port_down() ? 0 : 1;
    else if (hal_gui_controls_get_touch_attr(ep, &iret))
        ;
    else
        error("Invalid argument");
    targ = T_INT;
}
