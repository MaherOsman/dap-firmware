#include "platform_sai.h"
#include "sine_table.h"

int plat_sai_start_tone(SAI_HandleTypeDef *hsai)
{
    HAL_StatusTypeDef st = HAL_SAI_Transmit_DMA(hsai,
                                                (uint8_t *)sine_table,
                                                SINE_TABLE_LEN * 2);
    return (st == HAL_OK) ? 0 : (int)st;
}
