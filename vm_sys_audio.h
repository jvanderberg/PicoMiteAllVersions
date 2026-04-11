#ifndef VM_SYS_AUDIO_H
#define VM_SYS_AUDIO_H

#include "MMBasic.h"

void vm_sys_audio_play_stop(void);
void vm_sys_audio_play_tone(MMFLOAT left_hz, MMFLOAT right_hz,
                            int has_duration, int64_t duration_ms);

#endif
