/*
 * platform_st7789.c
 *
 *  Created on: Sep 2, 2026
 *      Author: maher
 */

/*
 * platform_st7789 — binds the portable ST7789 driver to STM32 HAL.
 *
 * This is the only file in firmware/src that is allowed to include the HAL.
 * Everything above it (st7789.c, gfx.c) stays hardware-independent.
 */
#include "main.h"
#include "st7789.h"
#include <stdio.h>

extern SPI_HandleTypeDef hspi1;

/* Tracks whether DC currently selects data (true) or command (false).
 * plat_write needs to know which, so it can pulse CS for commands only. */
static bool dc_is_data = false;

static void plat_set_dc(void *ctx, bool data)
{
    (void)ctx;
    dc_is_data = data;
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin,
                      data ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void plat_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    /* This panel latches on the CS rising edge — both for commands and for
     * pixel data. The portable driver holds CS across whole sequences, so
     * the glue has to supply the edge itself, per transfer. */
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, (uint16_t)len, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);

}
static void plat_set_cs(void *ctx, bool selected)
{
    (void)ctx;
    /* CS is active low: "selected" means drive the pin low. */
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin,
                      selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void plat_set_reset(void *ctx, bool asserted)
{
    (void)ctx;
    /* RST is active low: "asserted" means drive the pin low. */
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin,
                      asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void plat_delay_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    HAL_Delay(ms);
}

const st7789_bus_t platform_st7789_bus = {
    .write     = plat_write,
    .set_dc    = plat_set_dc,
    .set_cs    = plat_set_cs,
    .set_reset = plat_set_reset,
    .delay_ms  = plat_delay_ms,
    .ctx       = NULL
};
