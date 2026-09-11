/*
 * platform_sd_spi — binds the portable SD driver to STM32 HAL.
 *
 * Second of the two files allowed to include the HAL. Shares SPI1 with
 * the ST7789 panel, so it must leave the bus exactly as it found it.
 */
#include "main.h"
#include "sd_spi.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* One byte-time at 375 kHz is ~21 us; a 512-byte block at 24 MHz is
 * ~170 us. A whole second is generous for either. */
#define SD_SPI_TIMEOUT_MS 1000u

static bool plat_sd_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    HAL_StatusTypeDef st;

    if (tx != NULL && rx != NULL) {
        st = HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)tx, rx,
                                     (uint16_t)len, SD_SPI_TIMEOUT_MS);
    } else if (tx != NULL) {
        st = HAL_SPI_Transmit(&hspi1, (uint8_t *)tx,
                              (uint16_t)len, SD_SPI_TIMEOUT_MS);
    } else {
        /* Receive-only. HAL_SPI_Receive would clock out whatever happens
         * to be in the TX register — often 0x00, which the card reads as
         * the start of a command frame. It must be 0xFF. So we transmit
         * an explicit fill buffer instead. */
        static uint8_t ff[64];
        static bool ff_ready = false;
        if (!ff_ready) { memset(ff, 0xFF, sizeof ff); ff_ready = true; }

        size_t done = 0;
        while (done < len) {
            size_t chunk = len - done;
            if (chunk > sizeof ff) chunk = sizeof ff;

            st = HAL_SPI_TransmitReceive(&hspi1, ff,
                                         (rx != NULL) ? rx + done : ff,
                                         (uint16_t)chunk, SD_SPI_TIMEOUT_MS);
            if (st != HAL_OK) return false;
            done += chunk;
        }
        return true;
    }

    return st == HAL_OK;
}

static void plat_sd_cs(void *ctx, bool assert)
{
    (void)ctx;
    /* CS is active low: "assert" means drive the pin low. */
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin,
                      assert ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void plat_sd_set_speed(void *ctx, sd_speed_t speed)
{
    (void)ctx;
    uint32_t mbr = (speed == SD_SPEED_INIT)
                 ? SPI_BAUDRATEPRESCALER_256   /* 96 MHz / 256 = 375 kHz */
                 : SPI_BAUDRATEPRESCALER_8;    /* 96 MHz / 4   = 24 MHz  */

    /* CFG1 is write-protected while the peripheral is enabled. Writing
     * it anyway is silently ignored — no HAL status, no error, the clock
     * simply does not change. Disable, modify, re-enable. */
    __HAL_SPI_DISABLE(&hspi1);
    MODIFY_REG(hspi1.Instance->CFG1, SPI_CFG1_MBR, mbr);
    /* Deliberately left DISABLED. On H7, HAL_SPI_* writes CR2.TSIZE,
        * which is only writable while the peripheral is disabled — HAL
        * enables it itself per transfer. Re-enabling here makes every
        * subsequent HAL transfer silently time out. */
    /* Keep the handle's cached value in step, or a later HAL_SPI_Init()
     * would quietly undo this. */
    hspi1.Init.BaudRatePrescaler = mbr;
}

static uint32_t plat_sd_millis(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

const sd_bus_t platform_sd_bus = {
    .ctx       = NULL,
    .xfer      = plat_sd_xfer,
    .cs        = plat_sd_cs,
    .set_speed = plat_sd_set_speed,
    .millis    = plat_sd_millis
};
