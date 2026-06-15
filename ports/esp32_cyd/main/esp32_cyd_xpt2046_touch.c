/*
 * ESP32-2432S028R CYD XPT2046 resistive touch reader.
 *
 * The LCD already owns SPI3 and the SD card owns SPI2, so the tiny touch
 * transactions are bit-banged on the CYD touch header pins. This keeps touch
 * sampling off the display bus and avoids the readback/write-device contention
 * that previously blanked the panel.
 */

#include "esp32_cyd_xpt2046_touch.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "esp32_board_profile.h"

#define XPT_SCLK_GPIO ESP32_BOARD_CYD_TOUCH_SCLK
#define XPT_MOSI_GPIO ESP32_BOARD_CYD_TOUCH_MOSI
#define XPT_MISO_GPIO ESP32_BOARD_CYD_TOUCH_MISO
#define XPT_CS_GPIO ESP32_BOARD_CYD_TOUCH_CS
#define XPT_IRQ_GPIO ESP32_BOARD_CYD_TOUCH_IRQ

#define XPT_CMD_X 0xD0
#define XPT_CMD_Y 0x90
#define XPT_CMD_Z1 0xB0
#define XPT_CMD_Z2 0xC0
#define XPT_SAMPLES 7
#define XPT_DISCARD 2
#define XPT_PRESSURE_MIN 550

#define CYD_TOUCH_DEFAULT_XZERO 3860
#define CYD_TOUCH_DEFAULT_YZERO 370
#define CYD_TOUCH_DEFAULT_XSCALE -0.0909f
#define CYD_TOUCH_DEFAULT_YSCALE 0.0692f

static int s_ready;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int pin_index_valid(int pin) {
    return pin > 0 && pin <= NBRPINS && !(PinDef[pin].mode & UNUSED);
}

static int pin_gpio(int pin) {
    return pin_index_valid(pin) ? PinDef[pin].GPno : -1;
}

static void clk_pulse(void) {
    gpio_set_level((gpio_num_t)XPT_SCLK_GPIO, 1);
    esp_rom_delay_us(1);
    gpio_set_level((gpio_num_t)XPT_SCLK_GPIO, 0);
    esp_rom_delay_us(1);
}

static uint16_t xpt_transfer(uint8_t cmd) {
    uint16_t rx = 0;
    gpio_set_level((gpio_num_t)XPT_CS_GPIO, 0);
    esp_rom_delay_us(1);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)XPT_MOSI_GPIO, (cmd >> i) & 1);
        clk_pulse();
    }
    gpio_set_level((gpio_num_t)XPT_MOSI_GPIO, 0);
    for (int i = 0; i < 16; i++) {
        gpio_set_level((gpio_num_t)XPT_SCLK_GPIO, 1);
        esp_rom_delay_us(1);
        rx = (uint16_t)((rx << 1) |
                        (gpio_get_level((gpio_num_t)XPT_MISO_GPIO) ? 1 : 0));
        gpio_set_level((gpio_num_t)XPT_SCLK_GPIO, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level((gpio_num_t)XPT_CS_GPIO, 1);
    return (uint16_t)((rx >> 3) & 0x0fff);
}

static int read_axis(uint8_t cmd) {
    int v[XPT_SAMPLES];
    for (int i = 0; i < XPT_SAMPLES; i++) {
        v[i] = xpt_transfer(cmd);
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int t = v[j - 1];
            v[j - 1] = v[j];
            v[j] = t;
        }
    }
    int sum = 0;
    for (int i = XPT_DISCARD; i < XPT_SAMPLES - XPT_DISCARD; i++) sum += v[i];
    return sum / (XPT_SAMPLES - (XPT_DISCARD * 2));
}

static int read_raw(int * raw_x, int * raw_y) {
    if (!s_ready || !esp32_cyd_xpt2046_touch_down()) return 0;
    /* XPT2046 axis naming is panel-wiring dependent. These are raw controller
     * channels; calibration below maps them into display pixels. */
    int x = read_axis(XPT_CMD_Y);
    int y = read_axis(XPT_CMD_X);
    if (!esp32_cyd_xpt2046_touch_down()) return 0;
    if (raw_x) *raw_x = x;
    if (raw_y) *raw_y = y;
    return 1;
}

static int calibrated_axis(int raw, int zero, MMFLOAT scale, int limit) {
    int v = (int)(((MMFLOAT)raw - (MMFLOAT)zero) * scale);
    return clampi(v, 0, limit);
}

void esp32_cyd_xpt2046_touch_set_default_calibration(void) {
    Option.TOUCH_SWAPXY = 0;
    Option.TOUCH_XZERO = CYD_TOUCH_DEFAULT_XZERO;
    Option.TOUCH_YZERO = CYD_TOUCH_DEFAULT_YZERO;
    Option.TOUCH_XSCALE = CYD_TOUCH_DEFAULT_XSCALE;
    Option.TOUCH_YSCALE = CYD_TOUCH_DEFAULT_YSCALE;
}

void esp32_cyd_xpt2046_touch_set_identity_calibration(void) {
    Option.TOUCH_SWAPXY = 0;
    Option.TOUCH_XZERO = 0;
    Option.TOUCH_YZERO = 0;
    Option.TOUCH_XSCALE = 1.0f;
    Option.TOUCH_YSCALE = 1.0f;
}

void esp32_cyd_xpt2046_touch_init(void) {
    s_ready = 0;
    esp32_board_profile_reserve_touch_pins();
    if (pin_gpio(Option.TOUCH_CS) != XPT_CS_GPIO ||
        pin_gpio(Option.TOUCH_IRQ) != XPT_IRQ_GPIO)
        return;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << XPT_SCLK_GPIO) |
                        (1ULL << XPT_MOSI_GPIO) |
                        (1ULL << XPT_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level((gpio_num_t)XPT_CS_GPIO, 1);
    gpio_set_level((gpio_num_t)XPT_SCLK_GPIO, 0);
    gpio_set_level((gpio_num_t)XPT_MOSI_GPIO, 0);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << XPT_MISO_GPIO) | (1ULL << XPT_IRQ_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    s_ready = 1;
}

int esp32_cyd_xpt2046_touch_is_ready(void) {
    return s_ready;
}

int esp32_cyd_xpt2046_touch_down(void) {
    if (!s_ready) return 0;
    if (gpio_get_level((gpio_num_t)XPT_IRQ_GPIO) == 0) return 1;
    int z1 = read_axis(XPT_CMD_Z1);
    int z2 = read_axis(XPT_CMD_Z2);
    int z = z1 + 4095 - z2;
    return z > XPT_PRESSURE_MIN;
}

int esp32_cyd_xpt2046_touch_read_raw_mapped(int index, int * x, int * y) {
    if (index != 0) return 0;
    return read_raw(x, y);
}

int esp32_cyd_xpt2046_touch_read(int index, int * x, int * y) {
    int raw_x, raw_y;
    if (index != 0 || !read_raw(&raw_x, &raw_y)) return 0;
    int mx = raw_x;
    int my = raw_y;
    if (Option.TOUCH_SWAPXY) {
        mx = raw_y;
        my = raw_x;
    }
    if (x) *x = calibrated_axis(mx, Option.TOUCH_XZERO, Option.TOUCH_XSCALE,
                                HRes > 0 ? HRes - 1 : 319);
    if (y) *y = calibrated_axis(my, Option.TOUCH_YZERO, Option.TOUCH_YSCALE,
                                VRes > 0 ? VRes - 1 : 239);
    return 1;
}
