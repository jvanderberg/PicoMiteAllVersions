/*
 * Minimal FT6336U driver for the Freenove FNK0104B ILI9341 board.
 */

#include "esp32_ft6336u_touch.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "esp32_board_profile.h"
#include "esp32_freenove_i2c.h"
#include "esp32_option_ext.h"

#define FT6336U_ADDR 0x38
#define FT_REG_DEVICE_MODE 0x00
#define FT_REG_TOUCHES 0x02
#define FT_REG_THRESHOLD 0x80
#define FT_REG_CTRL 0x86
#define FT_REG_TOUCHRATE_ACTIVE 0x88
#define FT_REG_CHIP_ID 0xA3
#define FT_REG_INTERRUPT_MODE 0xA4

#define FT_CHIP_FT6206 0x06
#define FT_CHIP_FT6236 0x36
#define FT_CHIP_FT6336 0x64

#define TOUCH_W 320
#define TOUCH_H 240
#define FREENOVE_TOUCH_DEFAULT_XZERO 9
#define FREENOVE_TOUCH_DEFAULT_YZERO 20
#define FREENOVE_TOUCH_DEFAULT_XSCALE 1.013665f
#define FREENOVE_TOUCH_DEFAULT_YSCALE 1.139668f

static const char * TAG = "ft6336u";
static int s_init_attempted;
static int s_ready;

/* The touch configuration lives in the same Option fields PicoMite uses:
 * the I2C bus in Option.SYSTEM_I2C_SDA/SCL (OPTION SYSTEM I2C) and the
 * controller in Option.TOUCH_IRQ / TOUCH_CS(reset) / TOUCH_CAP /
 * THRESHOLD_CAP (OPTION TOUCH FT6336). Resolve a pin index to the raw GPIO
 * the ESP-IDF drivers need; 0 means not configured. */
static int touch_option_gpio(int pin) {
    if (pin <= 0 || pin > NBRPINS || (PinDef[pin].mode & UNUSED)) return -1;
    return PinDef[pin].GPno;
}

static int touch_pin_available(int pin, int allow_shared_i2c_owner) {
    if (pin == 0) return 1; /* optional pin left unconfigured */
    if (pin < 0 || pin > NBRPINS) return 0;
    if (ExtCurrentConfig[pin] == EXT_NOT_CONFIG) return 1;
    return allow_shared_i2c_owner &&
           esp32_board_profile_pin_owned_by_shared_i2c(pin);
}

static int touch_option_pins_available(void) {
    return touch_pin_available(Option.SYSTEM_I2C_SDA, 1) &&
           touch_pin_available(Option.SYSTEM_I2C_SCL, 1) &&
           touch_pin_available(Option.TOUCH_IRQ, 0) &&
           touch_pin_available(Option.TOUCH_CS, 0);
}

static void release_probe_i2c_if_unowned(void) {
    if (!esp32_board_profile_pin_owned_by_shared_i2c(Option.SYSTEM_I2C_SDA) &&
        !esp32_board_profile_pin_owned_by_shared_i2c(Option.SYSTEM_I2C_SCL))
        esp32_freenove_i2c_deinit();
}

static void release_probe_gpios(int irq_gpio, int rst_gpio) {
    if (irq_gpio >= 0) (void)gpio_reset_pin((gpio_num_t)irq_gpio);
    if (rst_gpio >= 0) (void)gpio_reset_pin((gpio_num_t)rst_gpio);
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int calibrated_axis(int v, int zero, float scale, int max) {
    int corrected = (int)((float)(v - zero) * scale + 0.5f);
    return clampi(corrected, 0, max);
}

static void publish_default_calibration_if_needed(void) {
    if (Option.TOUCH_XZERO || Option.TOUCH_YZERO ||
        Option.TOUCH_XSCALE != 0.0f || Option.TOUCH_YSCALE != 0.0f)
        return;
    Option.TOUCH_SWAPXY = 0;
    Option.TOUCH_XZERO = FREENOVE_TOUCH_DEFAULT_XZERO;
    Option.TOUCH_YZERO = FREENOVE_TOUCH_DEFAULT_YZERO;
    Option.TOUCH_XSCALE = FREENOVE_TOUCH_DEFAULT_XSCALE;
    Option.TOUCH_YSCALE = FREENOVE_TOUCH_DEFAULT_YSCALE;
}

void esp32_ft6336u_touch_set_default_calibration(void) {
    Option.TOUCH_SWAPXY = 0;
    Option.TOUCH_XZERO = FREENOVE_TOUCH_DEFAULT_XZERO;
    Option.TOUCH_YZERO = FREENOVE_TOUCH_DEFAULT_YZERO;
    Option.TOUCH_XSCALE = FREENOVE_TOUCH_DEFAULT_XSCALE;
    Option.TOUCH_YSCALE = FREENOVE_TOUCH_DEFAULT_YSCALE;
}

void esp32_ft6336u_touch_set_identity_calibration(void) {
    Option.TOUCH_SWAPXY = 0;
    Option.TOUCH_XZERO = 0;
    Option.TOUCH_YZERO = 0;
    Option.TOUCH_XSCALE = 1.0f;
    Option.TOUCH_YSCALE = 1.0f;
}

static int known_chip(uint8_t id) {
    return id == FT_CHIP_FT6206 || id == FT_CHIP_FT6236 || id == FT_CHIP_FT6336;
}

void esp32_ft6336u_touch_init(void) {
    if (s_init_attempted) return;
    s_init_attempted = 1;

    if (!Option.TOUCH_CAP) return; /* touch not configured */
    const int sda = touch_option_gpio(Option.SYSTEM_I2C_SDA);
    const int scl = touch_option_gpio(Option.SYSTEM_I2C_SCL);
    const int irq = touch_option_gpio(Option.TOUCH_IRQ);
    const int rst = touch_option_gpio(Option.TOUCH_CS);
    if (sda < 0 || scl < 0) return; /* system I2C not configured */
    if (!touch_option_pins_available()) {
        ESP_LOGW(TAG, "configured touch pins are already in use");
        return;
    }

    esp_err_t err = esp32_freenove_i2c_init(
        sda, scl, Option.SYSTEM_I2C_SLOW ? 100000 : 400000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    if (irq >= 0) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << (uint32_t)irq,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        (void)gpio_config(&in);
    }

    if (rst >= 0) {
        gpio_config_t out = {
            .pin_bit_mask = 1ULL << (uint32_t)rst,
            .mode = GPIO_MODE_OUTPUT,
        };
        (void)gpio_config(&out);
        gpio_set_level((gpio_num_t)rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)rst, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    uint8_t id = 0;
    err = esp32_freenove_i2c_read_reg(FT6336U_ADDR, FT_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not found: %s", esp_err_to_name(err));
        release_probe_gpios(irq, rst);
        release_probe_i2c_if_unowned();
        return;
    }
    if (!known_chip(id)) ESP_LOGW(TAG, "unexpected FT6x36 chip id 0x%02x", id);

    (void)esp32_freenove_i2c_write_reg8(FT6336U_ADDR, FT_REG_DEVICE_MODE, 0x00);
    (void)esp32_freenove_i2c_write_reg8(FT6336U_ADDR, FT_REG_INTERRUPT_MODE, 0x00);
    (void)esp32_freenove_i2c_write_reg8(FT6336U_ADDR, FT_REG_CTRL, 0x00);
    (void)esp32_freenove_i2c_write_reg8(
        FT6336U_ADDR, FT_REG_THRESHOLD,
        Option.THRESHOLD_CAP ? Option.THRESHOLD_CAP : 22);
    (void)esp32_freenove_i2c_write_reg8(FT6336U_ADDR, FT_REG_TOUCHRATE_ACTIVE, 0x01);

    publish_default_calibration_if_needed();
    s_ready = 1;
    esp32_board_profile_reserve_touch_pins();
    ESP_LOGI(TAG, "FT6x36 touch ready, chip id 0x%02x", id);
}

int esp32_ft6336u_touch_is_ready(void) {
    return s_ready;
}

int esp32_ft6336u_touch_read_raw_mapped(int index, int * x, int * y) {
    if (!s_init_attempted) esp32_ft6336u_touch_init();
    if (!s_ready || index < 0 || index > 1) return 0;

    uint8_t buf[11] = {0};
    esp_err_t err = esp32_freenove_i2c_read_reg(FT6336U_ADDR, FT_REG_TOUCHES,
                                                buf, sizeof(buf));
    if (err != ESP_OK) return 0;

    int count = buf[0] & 0x0f;
    if (count <= index || count > 2) return 0;

    int off = index ? 7 : 1;
    int raw_x = ((int)(buf[off] & 0x0f) << 8) | buf[off + 1];
    int raw_y = ((int)(buf[off + 2] & 0x0f) << 8) | buf[off + 3];

    if (x) *x = clampi(raw_y, 0, TOUCH_W - 1);
    if (y) *y = clampi((TOUCH_H - 1) - raw_x, 0, TOUCH_H - 1);
    return 1;
}

int esp32_ft6336u_touch_read(int index, int * x, int * y) {
    int raw_x = 0;
    int raw_y = 0;
    if (!esp32_ft6336u_touch_read_raw_mapped(index, &raw_x, &raw_y)) return 0;
    if (x) *x = calibrated_axis(raw_x, Option.TOUCH_XZERO, Option.TOUCH_XSCALE,
                                TOUCH_W - 1);
    if (y) *y = calibrated_axis(raw_y, Option.TOUCH_YZERO, Option.TOUCH_YSCALE,
                                TOUCH_H - 1);
    return 1;
}

int esp32_ft6336u_touch_down(void) {
    return esp32_ft6336u_touch_read(0, NULL, NULL);
}

/* OPTION TOUCH FT6336, irq, reset [, click] [, threshold] — the PicoMite
 * capacitive-touch form: the I2C bus comes from OPTION SYSTEM I2C, this
 * command attaches the controller to it (Option.TOUCH_IRQ / TOUCH_CS /
 * TOUCH_Click / TOUCH_CAP / THRESHOLD_CAP, the same fields PicoMite
 * stores). OPTION TOUCH DISABLE clears it. OPTION TOUCH CALIBRATE keeps
 * its own setter (it runs earlier in the option chain). Like the other pin
 * setters this saves and reboots so the new wiring is probed at boot. */
int esp32_touch_option_setter(unsigned char * cmdline) {
    extern int esp32_parse_pin_arg(unsigned char * arg);
    unsigned char * tp = checkstring(cmdline, (unsigned char *)"TOUCH");
    if (!tp) return 0;
    if (checkstring(tp, (unsigned char *)"CALIBRATE")) return 0;
    if (CurrentLinePtr) error("Invalid in a program");
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        Option.TOUCH_CAP = 0;
        Option.TOUCH_IRQ = 0;
        Option.TOUCH_CS = 0;
        Option.TOUCH_Click = 0;
        SaveOptions();
        _excep_code = RESET_COMMAND;
        SoftReset();
        return 1;
    }
    unsigned char * p = checkstring(tp, (unsigned char *)"FT6336");
    if (!p) error("Touch type not supported on this port");
    if (Option.TOUCH_CAP) error("Touch already configured");
    if (!Option.SYSTEM_I2C_SDA) error("System I2C not set");
    getargs(&p, 7, (unsigned char *)",");
    if (argc != 3 && argc != 5 && argc != 7)
        error("OPTION TOUCH FT6336, irq, reset [,click] [,threshold]");
    int pins[3] = {0, 0, 0};
    pins[0] = esp32_parse_pin_arg(argv[0]);                            /* IRQ */
    pins[1] = esp32_parse_pin_arg(argv[2]);                            /* RESET */
    if (argc >= 5 && *argv[4]) pins[2] = esp32_parse_pin_arg(argv[4]); /* click */
    int threshold = (argc == 7) ? getint(argv[6], 0, 255) : 22;
    for (int i = 0; i < 3; i++) {
        if (i == 2 && pins[i] == 0) continue;
        if (pins[i] < 1 || pins[i] > NBRPINS || (PinDef[pins[i]].mode & UNUSED))
            error("Invalid pin");
        if (ExtCurrentConfig[pins[i]] != EXT_NOT_CONFIG)
            error("Pin %/| is in use", pins[i], pins[i]);
        for (int j = i + 1; j < 3; j++)
            if (pins[j] && pins[i] == pins[j])
                error("Pin %/| is in use", pins[i], pins[i]);
    }
    Option.TOUCH_IRQ = pins[0];
    Option.TOUCH_CS = pins[1];
    Option.TOUCH_Click = pins[2];
    Option.TOUCH_CAP = 1;
    Option.THRESHOLD_CAP = threshold;
    SaveOptions();
    _excep_code = RESET_COMMAND;
    SoftReset();
    return 1;
}

void esp32_touch_print_options(void) {
    if (!Option.TOUCH_CAP) return;
    MMPrintString("OPTION TOUCH FT6336, ");
    MMPrintString((char *)PinDef[Option.TOUCH_IRQ].pinname);
    MMPrintString(", ");
    MMPrintString((char *)PinDef[Option.TOUCH_CS].pinname);
    if (Option.TOUCH_Click) {
        MMPrintString(", ");
        MMPrintString((char *)PinDef[Option.TOUCH_Click].pinname);
    }
    MMPrintString("\r\n");
}
