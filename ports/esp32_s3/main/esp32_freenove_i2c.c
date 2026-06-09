/*
 * Shared Freenove I2C bus for onboard touch/audio peripherals.
 *
 * The FT6336U touch and ES8311 codec share the bus-0 master with the BASIC
 * system I²C. The new i2c-master driver permits only one master bus per
 * port, so this module does not create its own bus: it obtains the per-port
 * master bus and per-address device handles from the shared owner in
 * hal_i2c_esp32.c (esp32_i2c_master_bus / esp32_i2c_master_device), holding
 * one reference while installed. Register reads/writes go through
 * i2c_master_transmit / i2c_master_transmit_receive on those device handles.
 */

#include "esp32_freenove_i2c.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal_i2c_esp32.h"

#define FREENOVE_I2C_PORT 0
#define FREENOVE_I2C_TIMEOUT_MS 100

static const char * TAG = "freenove_i2c";
static SemaphoreHandle_t s_lock;
static int s_installed;
static int s_sda = -1;
static int s_scl = -1;
static uint32_t s_hz;

static esp_err_t take_bus(void) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(FREENOVE_I2C_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void give_bus(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t esp32_freenove_i2c_init(int sda_gpio, int scl_gpio, uint32_t hz) {
    uint32_t requested_hz = hz ? hz : 400000;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    esp_err_t lock_err = take_bus();
    if (lock_err != ESP_OK) return lock_err;
    if (s_installed) {
        if (s_sda == sda_gpio && s_scl == scl_gpio) {
            /* Already up on these pins; the per-device speed is applied when
             * a device handle is fetched, so just adopt the requested rate. */
            s_hz = requested_hz;
            give_bus();
            return ESP_OK;
        }
        ESP_LOGE(TAG, "I2C bus already installed on GPIO%d/GPIO%d", s_sda, s_scl);
        give_bus();
        return ESP_ERR_INVALID_STATE;
    }
    if (esp32_i2c_master_bus(FREENOVE_I2C_PORT, sda_gpio, scl_gpio) == NULL) {
        give_bus();
        return ESP_FAIL;
    }
    s_installed = 1;
    s_sda = sda_gpio;
    s_scl = scl_gpio;
    s_hz = requested_hz;
    ESP_LOGI(TAG, "I2C ready on GPIO%d/GPIO%d at %lu Hz",
             s_sda, s_scl, (unsigned long)s_hz);
    give_bus();
    return ESP_OK;
}

void esp32_freenove_i2c_deinit(void) {
    if (!s_lock) return;
    esp_err_t lock_err = take_bus();
    if (lock_err != ESP_OK) return;
    if (s_installed) {
        esp32_i2c_master_bus_release(FREENOVE_I2C_PORT);
        s_installed = 0;
        s_sda = -1;
        s_scl = -1;
        s_hz = 0;
        ESP_LOGI(TAG, "I2C deinitialized");
    }
    give_bus();
}

esp_err_t esp32_freenove_i2c_read_reg(uint8_t addr, uint8_t reg,
                                      uint8_t * data, size_t len) {
    if (!s_installed || !data || len == 0) return ESP_ERR_INVALID_STATE;
    esp_err_t err = take_bus();
    if (err != ESP_OK) return err;
    i2c_master_dev_handle_t dev =
        esp32_i2c_master_device(FREENOVE_I2C_PORT, addr, s_hz);
    if (dev == NULL) {
        give_bus();
        return ESP_FAIL;
    }
    err = i2c_master_transmit_receive(dev, &reg, 1, data, len,
                                      FREENOVE_I2C_TIMEOUT_MS);
    give_bus();
    return err;
}

esp_err_t esp32_freenove_i2c_write_reg(uint8_t addr, uint8_t reg,
                                       const uint8_t * data, size_t len) {
    if (!s_installed) return ESP_ERR_INVALID_STATE;
    if (len && !data) return ESP_ERR_INVALID_ARG;
    if (len > 15) return ESP_ERR_INVALID_SIZE;
    uint8_t buf[16];
    buf[0] = reg;
    if (len && data) memcpy(&buf[1], data, len);
    esp_err_t err = take_bus();
    if (err != ESP_OK) return err;
    i2c_master_dev_handle_t dev =
        esp32_i2c_master_device(FREENOVE_I2C_PORT, addr, s_hz);
    if (dev == NULL) {
        give_bus();
        return ESP_FAIL;
    }
    err = i2c_master_transmit(dev, buf, len + 1, FREENOVE_I2C_TIMEOUT_MS);
    give_bus();
    return err;
}

esp_err_t esp32_freenove_i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
    return esp32_freenove_i2c_write_reg(addr, reg, &value, 1);
}
