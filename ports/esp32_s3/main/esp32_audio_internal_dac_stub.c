/*
 * esp32_audio_internal_dac_stub.c - default backend for boards without the
 * classic ESP32 GPIO25/26 internal-DAC audio path.
 */

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t esp32_audio_internal_dac_begin(int rate_hz) {
    (void)rate_hz;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp32_audio_internal_dac_set_rate(int rate_hz) {
    (void)rate_hz;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp32_audio_internal_dac_write(const int16_t * frames,
                                         size_t frame_count,
                                         size_t * bytes_written) {
    (void)frames;
    (void)frame_count;
    if (bytes_written) *bytes_written = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

void esp32_audio_internal_dac_idle(void) {
}

int esp32_audio_internal_dac_active(void) {
    return 0;
}
