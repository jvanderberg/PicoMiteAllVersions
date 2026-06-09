#ifndef ESP32_AUDIO_ES8311_H
#define ESP32_AUDIO_ES8311_H

#include "esp_err.h"

/* ES8311 codec recipe (OPTION AUDIO ES8311). Wiring comes from the audio
 * Option fields + OPTION SYSTEM I2C; see esp32_audio_es8311.c. */
esp_err_t esp32_audio_es8311_init(void);
void esp32_audio_es8311_deinit(void);

#endif /* ESP32_AUDIO_ES8311_H */
