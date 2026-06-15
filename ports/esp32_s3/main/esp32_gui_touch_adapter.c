/*
 * ESP32-S3 bridge for the legacy GUI-control touch API.
 *
 * The active board touch driver already returns display pixel coordinates.
 * This adapter presents those mapped coordinates to the shared GUI control
 * stack.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp32_touch_port.h"
#include "esp32_option_ext.h"

#define GUI_TOUCH_SAMPLE_CACHE_US 5000

int calibrate = 0;
int TOUCH_GETIRQTRIS = 0;
int TOUCH_IRQ_PIN = 0;
int TOUCH_CS_PIN = 0;
int TOUCH_Click_PIN = 0;

static int s_down;
static int s_x = TOUCH_ERROR;
static int s_y = TOUCH_ERROR;
static int s_x2 = TOUCH_ERROR;
static int s_y2 = TOUCH_ERROR;
static int64_t s_sample_us;

static int clamp_axis(int v, int limit) {
    if (limit <= 0) return v;
    if (v < 0) return TOUCH_ERROR;
    if (v >= limit) return TOUCH_ERROR;
    return v;
}

static int pin_to_gpio(int pin) {
    /* pin is a PinDef[] index; GPno holds its raw GPIO. (codemap() maps the
     * other direction, GPIO -> pin index.) */
    if (pin <= 0 || pin > NBRPINS) return -1;
    int gpio = PinDef[pin].GPno;
    if (gpio < 0 || gpio >= HAL_PORT_GPIO_COUNT) return -1;
    return gpio;
}

void PinSetBit(int pin, unsigned int offset) {
    int gpio = pin_to_gpio(pin);
    if (gpio < 0) return;

    switch (offset) {
    case LATSET:
        gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)gpio, 1);
        break;
    case LATCLR:
        gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)gpio, 0);
        break;
    case LATINV:
        gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)gpio, gpio_get_level((gpio_num_t)gpio) ? 0 : 1);
        break;
    default:
        break;
    }
}

static void publish_capacitive_option_state(void) {
    /* Option.TOUCH_CAP / TOUCH_IRQ / TOUCH_CS are the persisted
     * configuration (OPTION TOUCH FT6336, or a profile seed); only the
     * threshold default needs filling in here. */
    if (!Option.TOUCH_CAP) return;
    if (!Option.THRESHOLD_CAP) Option.THRESHOLD_CAP = 22;
}

static void sample_touch(void) {
    int64_t now = esp_timer_get_time();
    if (s_sample_us && now - s_sample_us < GUI_TOUCH_SAMPLE_CACHE_US) return;
    s_sample_us = now;

    s_x = s_y = s_x2 = s_y2 = TOUCH_ERROR;
    s_down = esp32_touch_port_read(0, &s_x, &s_y);
    if (s_down) {
        s_x = clamp_axis(s_x, HRes);
        s_y = clamp_axis(s_y, VRes);
        if (esp32_touch_port_read(1, &s_x2, &s_y2)) {
            s_x2 = clamp_axis(s_x2, HRes);
            s_y2 = clamp_axis(s_y2, VRes);
        }
    }
}

static void clear_cached_sample(void) {
    s_down = 0;
    s_x = s_y = s_x2 = s_y2 = TOUCH_ERROR;
    s_sample_us = 0;
}

int esp32_gui_touch_down_for_gui(void) {
    sample_touch();
    return s_down;
}

void InitTouch(void) {
    clear_cached_sample();
    esp32_touch_port_init();
    if (!esp32_touch_port_is_ready()) {
        /* Probe failure leaves the persisted configuration alone — the
         * controller is retried next boot. */
        TOUCH_GETIRQTRIS = 0;
        return;
    }

    publish_capacitive_option_state();
    TOUCH_IRQ_PIN = pin_to_gpio(Option.TOUCH_IRQ);
    TOUCH_CS_PIN = pin_to_gpio(Option.TOUCH_CS);
    TOUCH_GETIRQTRIS = 1;
}

void ConfigTouch(unsigned char * p) {
    (void)p;
    if (!Option.TOUCH_CAP)
        error("Touch not configured (OPTION TOUCH)");
    InitTouch();
}

void GetCalibration(int x, int y, int * xval, int * yval) {
    (void)x;
    (void)y;
    sample_touch();
    if (xval) *xval = s_down ? s_x : TOUCH_ERROR;
    if (yval) *yval = s_down ? s_y : TOUCH_ERROR;
}

int GetTouchValue(int cmd) {
    (void)cmd;
    sample_touch();
    return s_down ? 0 : TOUCH_ERROR;
}

int GetTouchAxis(int cmd) {
    return GetTouch(cmd == CMD_MEASURE_Y ? GET_Y_AXIS : GET_X_AXIS);
}

int GetTouchAxisCap(int axis) {
    return GetTouch(axis);
}

int GetTouch(int axis) {
    sample_touch();
    if (!s_down) return TOUCH_ERROR;
    switch (axis) {
    case GET_X_AXIS:
        return s_x;
    case GET_Y_AXIS:
        return s_y;
    case GET_X_AXIS2:
        return s_x2;
    case GET_Y_AXIS2:
        return s_y2;
    default:
        return TOUCH_ERROR;
    }
}
