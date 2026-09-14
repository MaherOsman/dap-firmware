#ifndef PLATFORM_AUDIO_H
#define PLATFORM_AUDIO_H

#include "main.h"
#include <stdbool.h>

/* One-time setup. Pass the SAI handle from main.c. */
void plat_audio_init(SAI_HandleTypeDef *hsai);

/* Open a WAV file and begin buffering. Returns 0 on success. */
int  plat_audio_play(const char *path);

/* Call every pass of the main loop. Does the SD reads. */
void plat_audio_service(void);

void plat_audio_stop(void);
bool plat_audio_is_active(void);

/* For status printing. */
unsigned plat_audio_underruns(void);
unsigned plat_audio_position_ms(void);
/* Pause stops the DMA rather than just the player: the ISR drains the ring
 * regardless of player state, so pausing the player alone keeps the sound
 * going until the buffer empties. */
void plat_audio_pause(void);
void plat_audio_resume(void);
bool plat_audio_is_paused(void);

/* delta is added to the current level and clamped to 0..100. Returns the
 * new level. Only affects audio buffered after the call. */
uint8_t plat_audio_set_volume(int delta);
uint8_t plat_audio_volume(void);

/* Facts about the open track, for the now-playing and info screens.
 * All return 0 / "" when nothing is open. */
uint32_t plat_audio_duration_ms(void);
uint32_t plat_audio_sample_rate(void);
uint8_t  plat_audio_bit_depth(void);
uint8_t  plat_audio_channels(void);
const char *plat_audio_format(void);
#endif
