/*
 * esp32_audio_es8311.c - ESP32-S3 glue for the ES8311 codec recipe.
 *
 * The codec knowledge lives in drivers/es8311/ (MCU-neutral); this file
 * supplies the transport and the board-wiring side: the control bus from
 * OPTION SYSTEM I2C, the amplifier enable pin and its active level from
 * ESP32_OPTION_AUDIO_AMP_EN / _AMP_ACTIVE_HIGH (OPTION AUDIO ES8311, or a
 * board profile's seed).
 */

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "es8311.h"
#include "esp32_audio_es8311.h"
#include "esp32_freenove_i2c.h"
#include "esp32_option_ext.h"

static const char * TAG = "es8311";

static int option_gpio(int pin) {
    if (pin <= 0 || pin > NBRPINS || (PinDef[pin].mode & UNUSED)) return -1;
    return PinDef[pin].GPno;
}

static void set_amp_level(int active) {
    int gpio = option_gpio(ESP32_OPTION_AUDIO_AMP_EN);
    if (gpio < 0) return;
    int active_high = ESP32_OPTION_AUDIO_AMP_ACTIVE_HIGH ? 1 : 0;
    gpio_set_level((gpio_num_t)gpio, active ? active_high : !active_high);
}

static int bus_write_reg(uint8_t reg, uint8_t value) {
    return esp32_freenove_i2c_write_reg8(ES8311_I2C_ADDR, reg, value) == ESP_OK
               ? 0
               : -1;
}

static int bus_read_reg(uint8_t reg, uint8_t * value) {
    return esp32_freenove_i2c_read_reg(ES8311_I2C_ADDR, reg, value, 1) == ESP_OK
               ? 0
               : -1;
}

static void bus_delay_ms(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static const es8311_bus_t s_bus = {
    .write_reg = bus_write_reg,
    .read_reg = bus_read_reg,
    .delay_ms = bus_delay_ms,
};

esp_err_t esp32_audio_es8311_init(void) {
    const int sda = option_gpio(Option.SYSTEM_I2C_SDA);
    const int scl = option_gpio(Option.SYSTEM_I2C_SCL);
    if (sda < 0 || scl < 0) return ESP_ERR_INVALID_STATE; /* OPTION SYSTEM I2C */

    int amp = option_gpio(ESP32_OPTION_AUDIO_AMP_EN);
    if (amp >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (uint32_t)amp,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) return err;
        set_amp_level(1);
    }

    esp_err_t err = esp32_freenove_i2c_init(
        sda, scl, Option.SYSTEM_I2C_SLOW ? 100000 : 400000);
    if (err != ESP_OK) {
        set_amp_level(0);
        return err;
    }

    uint8_t id = 0;
    if (es8311_start(&s_bus, &id) != 0) {
        set_amp_level(0);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 ready id=0x%02x", id);
    return ESP_OK;
}

void esp32_audio_es8311_deinit(void) {
    (void)es8311_mute(&s_bus);
    int amp = option_gpio(ESP32_OPTION_AUDIO_AMP_EN);
    if (amp >= 0) {
        set_amp_level(0);
        gpio_reset_pin((gpio_num_t)amp);
    }
}
