#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SD_BLOCK_SIZE 512u

/* ------------------------------------------------------------------ */
/* Injected bus — the only thing the platform layer must supply.       */
/* ------------------------------------------------------------------ */

typedef enum {
    SD_SPEED_INIT = 0,  /* <= 400 kHz. Mandatory for CMD0..CMD58.      */
    SD_SPEED_FULL = 1   /* shared with the display (15 MHz).           */
} sd_speed_t;

typedef struct {
    void *ctx;

    /* Full-duplex transfer of `len` bytes.
     *   tx == NULL  -> MUST clock out 0xFF for every byte.
     *   rx == NULL  -> received bytes are discarded.
     * MUST NOT touch the CS line. Returns false on bus error only. */
    bool (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);

    /* assert == true drives the SD CS line LOW. */
    void (*cs)(void *ctx, bool assert);

    /* Reprogram the SPI clock. Called by sd_init() only. */
    void (*set_speed)(void *ctx, sd_speed_t speed);

    /* Free-running millisecond counter, for timeouts. */
    uint32_t (*millis)(void *ctx);
} sd_bus_t;

/* ------------------------------------------------------------------ */

typedef enum {
    SD_CARD_UNKNOWN = 0,
    SD_CARD_V1,        /* SDSC, byte addressing                        */
    SD_CARD_V2_BYTE,   /* SDSC v2, byte addressing                     */
    SD_CARD_V2_BLOCK   /* SDHC/SDXC, block addressing (your 128 GB)    */
} sd_card_type_t;

typedef enum {
    SD_OK = 0,
    SD_ERR_PARAM,
    SD_ERR_BUS,             /* xfer() returned false                   */
    SD_ERR_NOT_INIT,
    SD_ERR_CMD0,            /* card never reached idle                 */
    SD_ERR_CMD8_PATTERN,    /* v2 card echoed the wrong check pattern  */
    SD_ERR_ACMD41_TIMEOUT,  /* card never left idle                    */
    SD_ERR_CMD58,
    SD_ERR_CMD16,
    SD_ERR_VOLTAGE,         /* card refuses 3.3 V                      */
    SD_ERR_READ_CMD,        /* CMD17/18 returned non-zero R1           */
    SD_ERR_READ_TOKEN,      /* data token never arrived / error token  */
    SD_ERR_TIMEOUT
} sd_err_t;

typedef struct {
    sd_bus_t        bus;
    sd_card_type_t  type;
    bool            block_addressed;
    bool            initialised;
    uint8_t         last_r1;   /* kept for diagnostics after a failure */
} sd_t;

/* Runs the full SPI-mode init handshake. Leaves the bus at
 * SD_SPEED_FULL on every exit path, success or failure. */
sd_err_t sd_init(sd_t *sd, const sd_bus_t *bus);

/* Reads `count` 512-byte blocks starting at logical block address
 * `lba` into `dst` (which must hold count * SD_BLOCK_SIZE bytes).
 * Uses CMD17 for count == 1, CMD18 + CMD12 otherwise. */
sd_err_t sd_read_blocks(sd_t *sd, uint32_t lba, uint8_t *dst, uint32_t count);

static inline bool sd_is_initialised(const sd_t *sd) {
    return sd != NULL && sd->initialised;
}

const char *sd_err_str(sd_err_t e);

#endif /* SD_SPI_H */
