/*
 * config — the handful of settings that survive a reboot.
 *
 * A theme that resets every boot is not really a setting, so this writes to
 * the card. It is deliberately tiny and deliberately forgiving: a missing,
 * truncated, or corrupt file is not an error worth stopping for, it just
 * means defaults. A player that refuses to boot because its preferences
 * file went bad would be a worse device than one that forgets your theme.
 *
 * Same lib_io_t seam as the index, so it is fully testable on a PC.
 *
 * Format (48 bytes, little-endian):
 *    0   8  magic "DAPCFG01"
 *    8   4  version
 *   12   1  theme index
 *   13   1  repeat mode
 *   14   1  volume 0..100
 *   15   1  now-playing layout (0 standard, 1 art only) — this byte was
 *           reserved and always written as 0, so older files read back as
 *           the standard layout with no version bump
 *   16  28  reserved (zero) — room to add settings without a version bump
 *   44   4  checksum of bytes 0..43
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "lib_io.h"

#define CFG_PATH         "/dap.cfg"
#define CFG_MAGIC        "DAPCFG01"
#define CFG_MAGIC_LEN    8u
#define CFG_VERSION      1u
#define CFG_SIZE         48u

typedef struct {
    uint8_t theme;    /* index into ALL_THEMES */
    uint8_t repeat;   /* pq_repeat_t */
    uint8_t volume;   /* 0..100 */
    uint8_t np_layout;/* np_layout_t: 0 standard, 1 art only */
} dap_config_t;

/* What you get with no file, a corrupt file, or a file from the future. */
void config_defaults(dap_config_t *cfg);

/* Forces every field into a legal range. Called by config_load, and exposed
 * because a value cycled in the UI should go through the same gate. */
void config_clamp(dap_config_t *cfg);

/* Returns true if a valid file was read. On false, `cfg` holds defaults —
 * so the return value is worth logging but never worth failing on. */
bool config_load(dap_config_t *cfg, const lib_io_t *io, const char *path);

/* Returns false if the write failed. Worth reporting: silently not saving
 * is the failure people notice three boots later. */
bool config_save(const dap_config_t *cfg, const lib_io_t *io,
                 const char *path);

/* Encode/decode a 48-byte buffer. Exposed for tests. */
void config_encode(uint8_t *buf, const dap_config_t *cfg);
bool config_decode(const uint8_t *buf, dap_config_t *cfg);

#endif /* CONFIG_H */
