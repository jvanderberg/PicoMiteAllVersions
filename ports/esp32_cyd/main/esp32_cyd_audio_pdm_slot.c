/*
 * esp32_cyd_audio_pdm_slot.c — PDM TX slot shape for I2S hardware v1
 * (classic ESP32).
 *
 * OPTION AUDIO left,right uses I2S PDM TX; hardware v1 exposes a single
 * PDM TX slot configuration (the DAC-style slot of the S3's hardware v2
 * does not exist on this chip).
 */

#include "driver/i2s_pdm.h"

i2s_pdm_tx_slot_config_t esp32_audio_pdm_tx_slot_config(void) {
    const i2s_pdm_tx_slot_config_t cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    return cfg;
}
