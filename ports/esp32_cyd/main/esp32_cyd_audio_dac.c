/*
 * esp32_cyd_audio_dac.c - CYD onboard speaker output.
 *
 * ESP32-2432S028R boards route GPIO26 (DAC2 / legacy I2S left channel)
 * through the onboard amplifier to the 2-pin speaker connector. ESP-IDF's
 * new I2S driver does not expose the original ESP32 internal DAC path, so the
 * CYD build uses the legacy I2S DAC API on I2S0 for this one board sink.
 */

#include <stdint.h>
#include <string.h>

#ifndef CONFIG_I2S_SUPPRESS_DEPRECATE_WARN
#define CONFIG_I2S_SUPPRESS_DEPRECATE_WARN 1
#endif
#include "driver/dac.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "hal/dac_ll.h"

#define CYD_DAC_I2S_NUM I2S_NUM_0
#define CYD_DAC_DMA_DESC_NUM 2
#define CYD_DAC_DMA_FRAME_NUM 128
#define CYD_DAC_CHANNEL DAC_CHAN_1 /* GPIO26 / DAC2 / legacy I2S left */
#define CYD_DAC_MIDPOINT 128

static int s_installed;
static int s_active;
static uint16_t s_dac_buf[CYD_DAC_DMA_FRAME_NUM * 2u];

esp_err_t esp32_audio_internal_dac_set_rate(int rate_hz);

static void cyd_dac_hold_midpoint(void) {
    dac_ll_digi_enable_dma(false);
    (void)dac_output_enable(CYD_DAC_CHANNEL);
    (void)dac_output_voltage(CYD_DAC_CHANNEL, CYD_DAC_MIDPOINT);
}

esp_err_t esp32_audio_internal_dac_begin(int rate_hz) {
    if (s_installed) return esp32_audio_internal_dac_set_rate(rate_hz);
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
        .sample_rate = rate_hz,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = CYD_DAC_DMA_DESC_NUM,
        .dma_buf_len = CYD_DAC_DMA_FRAME_NUM,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    esp_err_t err = i2s_driver_install(CYD_DAC_I2S_NUM, &cfg, 0, NULL);
    if (err != ESP_OK) return err;
    err = i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
    if (err != ESP_OK) {
        i2s_driver_uninstall(CYD_DAC_I2S_NUM);
        return err;
    }
    cyd_dac_hold_midpoint();
    s_installed = 1;
    s_active = 0;
    return ESP_OK;
}

esp_err_t esp32_audio_internal_dac_set_rate(int rate_hz) {
    if (!s_installed) return ESP_ERR_INVALID_STATE;
    return i2s_set_clk(CYD_DAC_I2S_NUM, (uint32_t)rate_hz,
                       I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

esp_err_t esp32_audio_internal_dac_write(const int16_t * frames,
                                         size_t frame_count,
                                         size_t * bytes_written) {
    if (bytes_written) *bytes_written = 0;
    size_t words = frame_count * 2u;
    if (frame_count > CYD_DAC_DMA_FRAME_NUM) return ESP_ERR_INVALID_SIZE;
    if (!s_active) {
        esp_err_t err = i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
        if (err != ESP_OK) return err;
        err = i2s_start(CYD_DAC_I2S_NUM);
        if (err != ESP_OK) return err;
        s_active = 1;
    }

    for (size_t i = 0; i < frame_count; i++) {
        int32_t mono = ((int32_t)frames[i * 2u] + (int32_t)frames[i * 2u + 1u]) / 2;
        mono >>= 4; /* The CYD amp is very hot; keep DAC swing modest. */
        uint16_t dac = (uint16_t)((((mono >> 8) + 128) & 0xff) << 8);
        s_dac_buf[i * 2u] = dac;
        s_dac_buf[i * 2u + 1u] = dac;
    }

    size_t wr = 0;
    esp_err_t err = i2s_write(CYD_DAC_I2S_NUM, s_dac_buf,
                              words * sizeof(uint16_t), &wr, portMAX_DELAY);
    if (bytes_written) *bytes_written = wr;
    return err;
}

void esp32_audio_internal_dac_idle(void) {
    if (!s_installed) return;
    if (s_active) (void)i2s_stop(CYD_DAC_I2S_NUM);
    cyd_dac_hold_midpoint();
    s_active = 0;
}

int esp32_audio_internal_dac_active(void) {
    return s_active ? 1 : 0;
}
