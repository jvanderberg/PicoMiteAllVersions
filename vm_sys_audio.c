/*
 * VM syscall conversion rule:
 * - copy/adapt legacy implementation code as closely as possible
 * - copy/adapt dependent legacy helpers too when needed
 * - do not invent new algorithms when legacy code already exists
 * - do not call, wrap, or dispatch back into legacy handlers
 * Any deviation from legacy implementation shape must be explicit and justified.
 */

#include <stdint.h>

#include "vm_sys_audio.h"
#include "vm_device_support.h"

#if defined(MMBASIC_HOST)

void vm_sys_audio_play_stop(void) {
}

void vm_sys_audio_play_tone(MMFLOAT left_hz, MMFLOAT right_hz,
                            int has_duration, int64_t duration_ms) {
    (void)left_hz;
    (void)right_hz;
    (void)has_duration;
    (void)duration_ms;
}

#else

#include "Audio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

extern void setrate(int rate);
extern void playimmediatevs1053(int play);

void vm_sys_audio_play_stop(void) {
    if (CurrentlyPlaying != P_NOTHING)
        CloseAudio(1);
}

void vm_sys_audio_play_tone(MMFLOAT left_hz, MMFLOAT right_hz,
                            int has_duration, int64_t duration_ms) {
    uint64_t play_duration = 0xffffffffffffffffULL;

    if (!(Option.AUDIO_L || Option.AUDIO_CLK_PIN || Option.audio_i2s_bclk))
        error("Audio not enabled");
    if (!(CurrentlyPlaying == P_NOTHING || CurrentlyPlaying == P_TONE ||
          CurrentlyPlaying == P_PAUSE_TONE || CurrentlyPlaying == P_STOP ||
          CurrentlyPlaying == P_WAVOPEN))
        error("Sound output in use");
    if (left_hz < 0.0 || left_hz > 22050.0 ||
        right_hz < 0.0 || right_hz > 22050.0)
        error("Valid is 0Hz to 20KHz");
    if (has_duration && duration_ms < 0)
        error("Number out of bounds");
    if (has_duration && duration_ms == 0)
        return;

    SoundPlay = 1000;
    mono = (left_hz == right_hz && vol_left == vol_right) ? 1 : 0;

    if (has_duration) {
        MMFLOAT duration_sec = (MMFLOAT)duration_ms / 1000.0;
        if (left_hz >= 10.0) {
            MMFLOAT hw = (MMFLOAT)PWM_FREQ / left_hz;
            MMFLOAT duration_ticks = duration_sec * (MMFLOAT)PWM_FREQ;
            play_duration = (uint64_t)(duration_ticks / hw) * (uint64_t)hw;
        } else {
            play_duration = (uint64_t)(duration_sec * (MMFLOAT)PWM_FREQ);
        }
    }

    pwm_set_irq0_enabled(AUDIO_SLICE, false);
    PhaseM_left = left_hz / (MMFLOAT)PWM_FREQ * 4096.0;
    PhaseM_right = right_hz / (MMFLOAT)PWM_FREQ * 4096.0;
    WAV_fnbr = 0;
    SoundPlay = play_duration;

    if (!(CurrentlyPlaying == P_PAUSE_TONE || CurrentlyPlaying == P_TONE)) {
        setrate(PWM_FREQ);
        PhaseAC_right = 0.0;
        PhaseAC_left = 0.0;
        if (Option.AUDIO_MISO_PIN)
            playimmediatevs1053(P_TONE);
    }

    CurrentlyPlaying = P_TONE;
    pwm_set_irq0_enabled(AUDIO_SLICE, true);
    pwm_set_enabled(AUDIO_SLICE, true);
}

#endif
