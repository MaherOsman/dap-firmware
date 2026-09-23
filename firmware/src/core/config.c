/* config.c — persisted settings. No HAL, no stdio. */

#include "config.h"

#include <string.h>

#include "library_index.h"   /* lib_rd_u32 / lib_wr_u32 */
#include "theme.h"           /* THEME_COUNT */

#define CFG_DEFAULT_THEME   0u
#define CFG_DEFAULT_REPEAT  0u
#define CFG_DEFAULT_VOLUME  80u

#define CFG_REPEAT_MAX      2u   /* off / one / all */
#define CFG_NP_LAYOUT_MAX   1u   /* standard / art only */

/* Not a CRC: this only needs to catch a half-written or zeroed file, and a
 * sum is enough for that at a fraction of the code. */
static uint32_t checksum(const uint8_t *buf, uint32_t len)
{
    uint32_t sum = 0x9E3779B9u;
    uint32_t i;
    for (i = 0; i < len; i++) {
        sum = (sum << 3) | (sum >> 29);
        sum += buf[i];
    }
    return sum;
}

void config_defaults(dap_config_t *cfg)
{
    if (cfg == NULL) return;
    cfg->theme = CFG_DEFAULT_THEME;
    cfg->repeat = CFG_DEFAULT_REPEAT;
    cfg->volume = CFG_DEFAULT_VOLUME;
    cfg->np_layout = 0u;
}

void config_clamp(dap_config_t *cfg)
{
    if (cfg == NULL) return;

    if (THEME_COUNT > 0 && cfg->theme >= (uint8_t)THEME_COUNT) {
        cfg->theme = 0u;
    }
    if (cfg->repeat > CFG_REPEAT_MAX) cfg->repeat = 0u;
    if (cfg->volume > 100u)           cfg->volume = 100u;
    if (cfg->np_layout > CFG_NP_LAYOUT_MAX) cfg->np_layout = 0u;
}

void config_encode(uint8_t *buf, const dap_config_t *cfg)
{
    memset(buf, 0, CFG_SIZE);
    memcpy(buf, CFG_MAGIC, CFG_MAGIC_LEN);
    lib_wr_u32(buf + 8, CFG_VERSION);
    buf[12] = cfg->theme;
    buf[13] = cfg->repeat;
    buf[14] = cfg->volume;
    buf[15] = cfg->np_layout;
    lib_wr_u32(buf + 44, checksum(buf, 44u));
}

bool config_decode(const uint8_t *buf, dap_config_t *cfg)
{
    if (memcmp(buf, CFG_MAGIC, CFG_MAGIC_LEN) != 0)   return false;
    if (lib_rd_u32(buf + 8) != CFG_VERSION)           return false;
    if (lib_rd_u32(buf + 44) != checksum(buf, 44u))   return false;

    cfg->theme = buf[12];
    cfg->repeat = buf[13];
    cfg->volume = buf[14];
    cfg->np_layout = buf[15];
    config_clamp(cfg);
    return true;
}

bool config_load(dap_config_t *cfg, const lib_io_t *io, const char *path)
{
    uint8_t buf[CFG_SIZE];
    void *fh = NULL;
    uint32_t got = 0;
    bool ok = false;

    config_defaults(cfg);   /* set first, so every failure path lands here */

    if (cfg == NULL || io == NULL || io->open == NULL || io->read == NULL ||
        io->close == NULL) {
        return false;
    }
    if (io->open(io->ctx, path ? path : CFG_PATH, LIB_IO_READ, &fh) != 0) {
        return false;
    }
    if (io->read(io->ctx, fh, buf, CFG_SIZE, &got) == 0 && got == CFG_SIZE) {
        dap_config_t tmp;
        if (config_decode(buf, &tmp)) {
            *cfg = tmp;
            ok = true;
        }
    }
    io->close(io->ctx, fh);
    if (!ok) config_defaults(cfg);
    return ok;
}

bool config_save(const dap_config_t *cfg, const lib_io_t *io,
                 const char *path)
{
    uint8_t buf[CFG_SIZE];
    dap_config_t safe;
    void *fh = NULL;
    bool ok;

    if (cfg == NULL || io == NULL || io->open == NULL || io->write == NULL ||
        io->close == NULL) {
        return false;
    }

    /* Never write a value that would not survive being read back. */
    safe = *cfg;
    config_clamp(&safe);
    config_encode(buf, &safe);

    if (io->open(io->ctx, path ? path : CFG_PATH, LIB_IO_WRITE, &fh) != 0) {
        return false;
    }
    ok = (io->write(io->ctx, fh, buf, CFG_SIZE) == 0);
    io->close(io->ctx, fh);
    return ok;
}
