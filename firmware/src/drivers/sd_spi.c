#include "sd_spi.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Command indices                                                     */
/* ------------------------------------------------------------------ */
#define CMD0    0   /* GO_IDLE_STATE        -> R1                      */
#define CMD8    8   /* SEND_IF_COND         -> R7 (R1 + 4 bytes)       */
#define CMD12  12   /* STOP_TRANSMISSION    -> R1b                     */
#define CMD16  16   /* SET_BLOCKLEN         -> R1                      */
#define CMD17  17   /* READ_SINGLE_BLOCK    -> R1 + data packet        */
#define CMD18  18   /* READ_MULTIPLE_BLOCK  -> R1 + data packets       */
#define CMD55  55   /* APP_CMD              -> R1                      */
#define CMD58  58   /* READ_OCR             -> R3 (R1 + 4 bytes)       */
#define ACMD41 41   /* SD_SEND_OP_COND      -> R1  (needs CMD55 first) */

/* R1 response bits. Bit 7 is always 0 in a valid R1 -- that is how we
 * recognise the response amongst the 0xFF idle bytes. */
#define R1_IDLE_STATE   0x01
#define R1_ILLEGAL_CMD  0x04
#define R1_INVALID      0xFF   /* nothing responded                    */

/* NCR: the card may take up to 8 byte-times to answer. Allow a little
 * slack -- some cards are lazier than the spec permits. */
#define NCR_MAX_BYTES   16

/* ------------------------------------------------------------------ */
/* Low-level bus helpers                                               */
/* ------------------------------------------------------------------ */

static bool tx(sd_t *sd, const uint8_t *buf, size_t len) {
    return sd->bus.xfer(sd->bus.ctx, buf, NULL, len);
}

/* Clock out 0xFF and capture what comes back. */
static bool rx(sd_t *sd, uint8_t *buf, size_t len) {
    return sd->bus.xfer(sd->bus.ctx, NULL, buf, len);
}

static uint8_t rx_byte(sd_t *sd) {
    uint8_t b = 0xFF;
    if (!rx(sd, &b, 1)) return 0xFF;
    return b;
}

static void cs_low(sd_t *sd)  { sd->bus.cs(sd->bus.ctx, true);  }

/* Deselect, then clock 8 more bits. The card only releases DO one
 * clock edge AFTER CS goes high -- without these it keeps driving the
 * line and the display's next transfer sees a fighting driver. */
static void cs_high(sd_t *sd) {
    sd->bus.cs(sd->bus.ctx, false);
    uint8_t dummy;
    (void)rx(sd, &dummy, 1);
}

/* ------------------------------------------------------------------ */
/* CRC7 -- required on CMD0 and CMD8 even though CRC is otherwise off  */
/* ------------------------------------------------------------------ */

static uint8_t crc7(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            crc <<= 1;
            if ((b ^ crc) & 0x80) crc ^= 0x09;  /* poly x^7 + x^3 + 1  */
            b <<= 1;
        }
    }
    return crc & 0x7F;
}

/* ------------------------------------------------------------------ */
/* Command / response                                                  */
/* ------------------------------------------------------------------ */

/* Sends a 6-byte command frame and returns the R1 byte.
 * CS must already be asserted by the caller -- a command and its
 * response are ONE transaction. Returns 0xFF if the card never
 * answered. */
static uint8_t send_cmd(sd_t *sd, uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];

    frame[0] = 0x40 | (cmd & 0x3F);      /* start bit 0, tx bit 1     */
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)(arg);
    frame[5] = (uint8_t)((crc7(frame, 5) << 1) | 0x01);  /* end bit 1  */

    /* One idle byte before the frame: gives the card a clock edge to
     * settle on and costs nothing. */
    uint8_t ff = 0xFF;
    if (!tx(sd, &ff, 1))       return R1_INVALID;
    if (!tx(sd, frame, 6))     return R1_INVALID;

    /* CMD12 emits one stuff byte before its response. */
    if (cmd == CMD12) (void)rx_byte(sd);

    for (int i = 0; i < NCR_MAX_BYTES; i++) {
        uint8_t r = rx_byte(sd);
        if ((r & 0x80) == 0) {           /* valid R1: MSB clear        */
            sd->last_r1 = r;
            return r;
        }
    }
    sd->last_r1 = R1_INVALID;
    return R1_INVALID;
}

/* CMD55 + ACMD41. Both share the transaction. */
static uint8_t send_acmd41(sd_t *sd, uint32_t arg) {
    uint8_t r = send_cmd(sd, CMD55, 0);
    if (r & 0x80) return r;              /* no response at all         */
    return send_cmd(sd, ACMD41, arg);
}

/* ------------------------------------------------------------------ */
/* Power-up: 74+ clocks with CS HIGH and DI HIGH                       */
/* ------------------------------------------------------------------ */

static bool powerup_clocks(sd_t *sd) {
    sd->bus.cs(sd->bus.ctx, false);      /* deasserted -- deliberate   */
    uint8_t buf[10];
    return rx(sd, buf, sizeof buf);      /* 80 clocks of 0xFF          */
}

/* ------------------------------------------------------------------ */

const char *sd_err_str(sd_err_t e) {
    switch (e) {
    case SD_OK:                 return "OK";
    case SD_ERR_PARAM:          return "bad parameter";
    case SD_ERR_BUS:            return "SPI bus error";
    case SD_ERR_NOT_INIT:       return "card not initialised";
    case SD_ERR_CMD0:           return "CMD0: card never went idle";
    case SD_ERR_CMD8_PATTERN:   return "CMD8: check pattern mismatch";
    case SD_ERR_ACMD41_TIMEOUT: return "ACMD41: card stayed idle";
    case SD_ERR_CMD58:          return "CMD58: OCR read failed";
    case SD_ERR_CMD16:          return "CMD16: set blocklen failed";
    case SD_ERR_VOLTAGE:        return "card rejects 3.3V";
    case SD_ERR_READ_CMD:       return "read command rejected";
    case SD_ERR_READ_TOKEN:     return "no data token from card";
    case SD_ERR_TIMEOUT:        return "timeout";
    default:                    return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

#define CMD0_MAX_ATTEMPTS   16
#define ACMD41_TIMEOUT_MS   1000u   /* spec says 1 s; cards vary wildly */
#define CMD8_PATTERN        0xAAu
#define CMD8_ARG            0x000001AAu  /* 2.7-3.6 V + check pattern   */
#define OCR_CCS_BIT         0x40000000u  /* 1 = block addressed (SDHC)  */
#define OCR_VOLTAGE_3V3     0x00300000u  /* 3.2-3.4 V window            */
#define ACMD41_HCS          0x40000000u  /* tell card we support SDHC   */

/* Reads the 4 trailing bytes of an R3/R7 response (big-endian). */
static uint32_t read_r3_r7_tail(sd_t *sd) {
    uint8_t b[4];
    if (!rx(sd, b, 4)) return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

sd_err_t sd_init(sd_t *sd, const sd_bus_t *bus) {
    if (sd == NULL || bus == NULL ||
        bus->xfer == NULL || bus->cs == NULL ||
        bus->set_speed == NULL || bus->millis == NULL) {
        return SD_ERR_PARAM;
    }

    memset(sd, 0, sizeof *sd);
    sd->bus  = *bus;
    sd->type = SD_CARD_UNKNOWN;

    sd_err_t result = SD_ERR_TIMEOUT;   /* overwritten on every path    */

    /* ---- slow clock for the whole handshake --------------------- */
    sd->bus.set_speed(sd->bus.ctx, SD_SPEED_INIT);

    if (!powerup_clocks(sd)) { result = SD_ERR_BUS; goto done; }

    /* ---- CMD0: software reset into SPI mode --------------------- */
    /* Asserting CS during CMD0 is what selects SPI mode rather than
     * SD native mode. This is a one-way door: the card stays in SPI
     * mode until power is physically removed. */
    {
        uint8_t r1 = R1_INVALID;
        for (int i = 0; i < CMD0_MAX_ATTEMPTS; i++) {
            cs_low(sd);
            r1 = send_cmd(sd, CMD0, 0);
            cs_high(sd);
            if (r1 == R1_IDLE_STATE) break;
        }
        if (r1 != R1_IDLE_STATE) { result = SD_ERR_CMD0; goto done; }
    }

    /* ---- CMD8: is this a v2 card, and does it accept 3.3 V? ----- */
    bool is_v2 = false;
    {
        cs_low(sd);
        uint8_t r1 = send_cmd(sd, CMD8, CMD8_ARG);
        if (r1 & R1_ILLEGAL_CMD) {
            /* v1 card, or MMC. It does not understand CMD8 at all. */
            cs_high(sd);
            is_v2 = false;
        } else if (r1 == R1_IDLE_STATE) {
            uint32_t tail = read_r3_r7_tail(sd);
            cs_high(sd);
            /* The card echoes our check pattern back. If it does not,
             * something on the bus is mangling data -- treat it as a
             * wiring fault, not as a v1 card. */
            if ((tail & 0xFF) != CMD8_PATTERN) {
                result = SD_ERR_CMD8_PATTERN; goto done;
            }
            if ((tail & 0x0F00) == 0) { result = SD_ERR_VOLTAGE; goto done; }
            is_v2 = true;
        } else {
            cs_high(sd);
            result = SD_ERR_CMD8_PATTERN; goto done;
        }
    }

    /* ---- ACMD41: start initialisation, poll until not idle ------ */
    {
        uint32_t start = sd->bus.millis(sd->bus.ctx);
        uint8_t  r1;
        for (;;) {
            cs_low(sd);
            r1 = send_acmd41(sd, is_v2 ? ACMD41_HCS : 0);
            cs_high(sd);

            if (r1 == 0x00) break;                 /* ready           */
            if (r1 & 0x80) { result = SD_ERR_BUS; goto done; }

            if (sd->bus.millis(sd->bus.ctx) - start > ACMD41_TIMEOUT_MS) {
                result = SD_ERR_ACMD41_TIMEOUT; goto done;
            }
        }
    }

    /* ---- CMD58: byte addressed or block addressed? -------------- */
    if (is_v2) {
        cs_low(sd);
        uint8_t r1 = send_cmd(sd, CMD58, 0);
        if (r1 != 0x00) { cs_high(sd); result = SD_ERR_CMD58; goto done; }
        uint32_t ocr = read_r3_r7_tail(sd);
        cs_high(sd);

        if ((ocr & OCR_VOLTAGE_3V3) == 0) { result = SD_ERR_VOLTAGE; goto done; }

        sd->block_addressed = (ocr & OCR_CCS_BIT) != 0;
        sd->type = sd->block_addressed ? SD_CARD_V2_BLOCK : SD_CARD_V2_BYTE;
    } else {
        sd->block_addressed = false;
        sd->type = SD_CARD_V1;
    }

    /* ---- CMD16: force 512-byte blocks on byte-addressed cards --- */
    /* SDHC ignores this (block length is fixed at 512), so only send
     * it where it means something. */
    if (!sd->block_addressed) {
        cs_low(sd);
        uint8_t r1 = send_cmd(sd, CMD16, SD_BLOCK_SIZE);
        cs_high(sd);
        if (r1 != 0x00) { result = SD_ERR_CMD16; goto done; }
    }

    sd->initialised = true;
    result = SD_OK;

done:
    /* Every exit path restores the shared bus speed -- the display is
     * on this SPI and must not be left crawling at 400 kHz. */
    sd->bus.set_speed(sd->bus.ctx, SD_SPEED_FULL);
    return result;
}

/* ------------------------------------------------------------------ */
/* Block reads                                                         */
/* ------------------------------------------------------------------ */

#define TOKEN_START_BLOCK   0xFEu   /* precedes every 512-byte payload */
#define TOKEN_ERROR_MASK    0xF0u   /* error tokens have high nibble 0 */
#define READ_TOKEN_TIMEOUT_MS  200u /* spec NAC max is 100 ms          */
#define BUSY_TIMEOUT_MS      500u

/* Waits for the data start token. The card sends 0xFF while it is
 * still fetching from flash -- that wait is normal and can be tens of
 * milliseconds on a large card. Anything that is neither 0xFF nor 0xFE
 * is an error token and means the read has already failed. */
static sd_err_t wait_data_token(sd_t *sd) {
    uint32_t start = sd->bus.millis(sd->bus.ctx);
    for (;;) {
        uint8_t t = rx_byte(sd);
        if (t == TOKEN_START_BLOCK) return SD_OK;
        if (t != 0xFF) return SD_ERR_READ_TOKEN;  /* error token       */

        if (sd->bus.millis(sd->bus.ctx) - start > READ_TOKEN_TIMEOUT_MS) {
            return SD_ERR_READ_TOKEN;
        }
    }
}

/* Reads one 512-byte payload plus its 2-byte CRC (discarded -- CRC is
 * off in SPI mode by default and the card still sends the field). */
static sd_err_t read_data_block(sd_t *sd, uint8_t *dst) {
    sd_err_t e = wait_data_token(sd);
    if (e != SD_OK) return e;

    if (!rx(sd, dst, SD_BLOCK_SIZE)) return SD_ERR_BUS;

    uint8_t crc[2];
    if (!rx(sd, crc, 2)) return SD_ERR_BUS;
    return SD_OK;
}

/* After CMD12 the card holds DO low while it finishes up. */
static sd_err_t wait_not_busy(sd_t *sd) {
    uint32_t start = sd->bus.millis(sd->bus.ctx);
    while (rx_byte(sd) != 0xFF) {
        if (sd->bus.millis(sd->bus.ctx) - start > BUSY_TIMEOUT_MS) {
            return SD_ERR_TIMEOUT;
        }
    }
    return SD_OK;
}

sd_err_t sd_read_blocks(sd_t *sd, uint32_t lba, uint8_t *dst, uint32_t count) {
    if (sd == NULL || dst == NULL || count == 0) return SD_ERR_PARAM;
    if (!sd->initialised)                        return SD_ERR_NOT_INIT;

    /* SDSC addresses by byte, SDHC/SDXC by block. Getting this wrong
     * does not error -- you silently read the wrong part of the card,
     * which FatFs reports as "no filesystem". */
    uint32_t addr = sd->block_addressed ? lba : (lba * SD_BLOCK_SIZE);

    sd_err_t result;

    if (count == 1) {
        cs_low(sd);
        if (send_cmd(sd, CMD17, addr) != 0x00) { result = SD_ERR_READ_CMD; goto out; }
        result = read_data_block(sd, dst);
        goto out;
    }

    cs_low(sd);
    if (send_cmd(sd, CMD18, addr) != 0x00) { result = SD_ERR_READ_CMD; goto out; }

    result = SD_OK;
    for (uint32_t i = 0; i < count; i++) {
        result = read_data_block(sd, dst + (i * SD_BLOCK_SIZE));
        if (result != SD_OK) break;
    }

    /* CMD12 must be sent even if a block failed -- the card is still
     * streaming and will keep driving the bus otherwise. */
    (void)send_cmd(sd, CMD12, 0);
    if (wait_not_busy(sd) != SD_OK && result == SD_OK) result = SD_ERR_TIMEOUT;

out:
    cs_high(sd);
    return result;
}
