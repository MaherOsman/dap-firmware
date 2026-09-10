#ifndef PLATFORM_SAI_H
#define PLATFORM_SAI_H

#include "main.h"

/* Starts a continuous 441 Hz test tone on the given SAI block via circular DMA.
   Returns 0 on success, non-zero HAL status on failure. */
int plat_sai_start_tone(SAI_HandleTypeDef *hsai);

#endif
