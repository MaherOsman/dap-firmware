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
#endif
