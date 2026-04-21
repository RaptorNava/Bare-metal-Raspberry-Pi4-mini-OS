#ifndef AUDIO_H
#define AUDIO_H

/**
 * audio.h — PWM audio driver interface
 */

#include <stdint.h>

void audio_init(void);
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void audio_play_melody(void);
void audio_beep(void);
void audio_mute(void);
void audio_unmute(void);
void audio_print_info(void);

#endif /* AUDIO_H */
