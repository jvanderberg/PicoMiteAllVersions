/*
 * esp32_audio_pdm_slot_s3.c — PDM TX slot shape for I2S hardware v2
 * (ESP32-S3).
 *
 * OPTION AUDIO left,right uses I2S PDM TX; on hardware v2 the slot is the
 * DAC-style configuration. Classic ESP32 (hardware v1) links
 * esp32_cyd_audio_pdm_slot.c instead.
 */

#include "driver/i2s_pdm.h"

i2s_pdm_tx_slot_config_t esp32_audio_pdm_tx_slot_config(void) {
    const i2s_pdm_tx_slot_config_t cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    return cfg;
}
