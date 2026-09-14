/* test_config.c — persisted settings.
 *
 * The theme of these tests: a settings file is the least important data on
 * the card, so every way it can go wrong must end in defaults rather than
 * in a failure the user has to care about.
 */

#include "test.h"

#include <stdlib.h>

#include "../src/core/config.h"
#include "../src/core/theme.h"

/* ---- in-RAM file ---- */

static uint8_t  g_data[256];
static uint32_t g_len;
static uint32_t g_pos;
static int      g_exists;
static int      g_fail_open;
static int      g_fail_write;
static int      g_short_read;

static int f_open_(void *c, const char *p, int mode, void **fh)
{
    (void)c; (void)p;
    if (g_fail_open) return -1;
    if (mode == LIB_IO_READ && !g_exists) return -1;
    if (mode == LIB_IO_WRITE) { g_len = 0; g_exists = 1; }
    g_pos = 0;
    *fh = &g_pos;
    return 0;
}

static int f_read_(void *c, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    uint32_t n;
    (void)c; (void)fh;
    if (g_pos >= g_len) { *got = 0; return 0; }
    n = g_len - g_pos;
    if (n > len) n = len;
    if (g_short_read && n > 0u) n--;
    memcpy(dst, g_data + g_pos, n);
    g_pos += n;
    *got = n;
    return 0;
}

static int f_write_(void *c, void *fh, const void *src, uint32_t len)
{
    (void)c; (void)fh;
    if (g_fail_write) return -1;
    if (g_pos + len > sizeof(g_data)) return -1;
    memcpy(g_data + g_pos, src, len);
    g_pos += len;
    if (g_pos > g_len) g_len = g_pos;
    return 0;
}

static int f_seek_(void *c, void *fh, uint32_t off)
{ (void)c; (void)fh; if (off > g_len) return -1; g_pos = off; return 0; }

static int f_close_(void *c, void *fh) { (void)c; (void)fh; return 0; }

static lib_io_t make_io(void)
{
    lib_io_t io;
    memset(&io, 0, sizeof(io));
    io.open = f_open_; io.read = f_read_; io.write = f_write_;
    io.seek = f_seek_; io.close = f_close_;
    return io;
}

static void fs_reset(void)
{
    memset(g_data, 0, sizeof(g_data));
    g_len = 0; g_pos = 0; g_exists = 0;
    g_fail_open = 0; g_fail_write = 0; g_short_read = 0;
}

/* ===================================================================== */

TEST(defaults_are_sane)
{
    dap_config_t c;

    memset(&c, 0xFF, sizeof(c));
    config_defaults(&c);

    CHECK_EQ(c.theme, 0u);
    CHECK_EQ(c.repeat, 0u);
    CHECK(c.volume > 0u);
    CHECK(c.volume <= 100u);
}

TEST(a_saved_config_reads_back_identical)
{
    dap_config_t out, in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    in.theme = 2u;
    in.repeat = 1u;
    in.volume = 43u;

    CHECK(config_save(&in, &io, CFG_PATH));
    CHECK_EQ(g_len, CFG_SIZE);

    CHECK(config_load(&out, &io, CFG_PATH));
    CHECK_EQ(out.theme, 2u);
    CHECK_EQ(out.repeat, 1u);
    CHECK_EQ(out.volume, 43u);
}

TEST(a_missing_file_yields_defaults_not_an_error_worth_stopping_for)
{
    dap_config_t c, d;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&d);

    CHECK(!config_load(&c, &io, CFG_PATH));   /* reports "no file" */
    CHECK_EQ(c.theme, d.theme);               /* but is fully usable */
    CHECK_EQ(c.repeat, d.repeat);
    CHECK_EQ(c.volume, d.volume);
}

TEST(a_corrupt_file_yields_defaults)
{
    dap_config_t c, in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    in.theme = 1u;
    CHECK(config_save(&in, &io, CFG_PATH));

    /* flip a byte in the payload — the checksum must catch it */
    g_data[13] ^= 0xFFu;
    CHECK(!config_load(&c, &io, CFG_PATH));
    CHECK_EQ(c.theme, 0u);
}

TEST(a_wrong_magic_yields_defaults)
{
    dap_config_t c, in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    CHECK(config_save(&in, &io, CFG_PATH));
    g_data[0] = 'X';
    CHECK(!config_load(&c, &io, CFG_PATH));
}

TEST(a_file_from_a_future_version_yields_defaults)
{
    dap_config_t c, in;
    lib_io_t io = make_io();
    uint8_t buf[CFG_SIZE];

    fs_reset();
    config_defaults(&in);
    in.theme = 2u;
    config_encode(buf, &in);
    buf[8] = (uint8_t)(CFG_VERSION + 1u);     /* bump version, break sum */
    memcpy(g_data, buf, CFG_SIZE);
    g_len = CFG_SIZE;
    g_exists = 1;

    CHECK(!config_load(&c, &io, CFG_PATH));
    CHECK_EQ(c.theme, 0u);
}

TEST(a_truncated_file_yields_defaults)
{
    dap_config_t c, in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    CHECK(config_save(&in, &io, CFG_PATH));
    g_len = CFG_SIZE - 4u;                    /* half-written */

    CHECK(!config_load(&c, &io, CFG_PATH));
    CHECK_EQ(c.theme, 0u);
}

TEST(a_short_read_yields_defaults)
{
    dap_config_t c, in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    CHECK(config_save(&in, &io, CFG_PATH));
    g_short_read = 1;

    CHECK(!config_load(&c, &io, CFG_PATH));
}

TEST(out_of_range_values_are_clamped_on_load)
{
    dap_config_t c, wild, clamped;
    uint8_t buf[CFG_SIZE];
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&wild);
    wild.theme = 200u;
    wild.repeat = 200u;
    wild.volume = 200u;

    /* Encode a file that is valid on its own terms — correct magic and
     * checksum — but holds values the build no longer allows, which is what
     * a card written by a future firmware would look like. */
    clamped = wild;
    config_clamp(&clamped);
    config_encode(buf, &clamped);
    memcpy(g_data, buf, CFG_SIZE);
    g_len = CFG_SIZE;
    g_exists = 1;

    CHECK(config_load(&c, &io, CFG_PATH));
    CHECK(c.theme < (uint8_t)THEME_COUNT);
    CHECK(c.repeat <= 2u);
    CHECK(c.volume <= 100u);
}

TEST(clamp_fixes_every_field)
{
    dap_config_t c;

    c.theme = 250u; c.repeat = 250u; c.volume = 250u;
    config_clamp(&c);
    CHECK(c.theme < (uint8_t)THEME_COUNT);
    CHECK_EQ(c.repeat, 0u);
    CHECK_EQ(c.volume, 100u);

    /* legal values are left alone */
    c.theme = 1u; c.repeat = 2u; c.volume = 50u;
    config_clamp(&c);
    CHECK_EQ(c.theme, 1u);
    CHECK_EQ(c.repeat, 2u);
    CHECK_EQ(c.volume, 50u);
}

TEST(saving_never_writes_a_value_it_could_not_read_back)
{
    dap_config_t in, out;
    lib_io_t io = make_io();

    fs_reset();
    in.theme = 99u; in.repeat = 99u; in.volume = 250u;
    CHECK(config_save(&in, &io, CFG_PATH));
    CHECK(config_load(&out, &io, CFG_PATH));
    CHECK(out.theme < (uint8_t)THEME_COUNT);
    CHECK(out.repeat <= 2u);
    CHECK_EQ(out.volume, 100u);
}

TEST(a_failed_write_is_reported)
{
    dap_config_t in;
    lib_io_t io = make_io();

    fs_reset();
    config_defaults(&in);
    g_fail_write = 1;
    CHECK(!config_save(&in, &io, CFG_PATH));

    g_fail_write = 0;
    g_fail_open = 1;
    CHECK(!config_save(&in, &io, CFG_PATH));
}

TEST(null_arguments_are_safe)
{
    dap_config_t c;
    lib_io_t io = make_io();

    config_defaults(NULL);
    config_clamp(NULL);
    CHECK(!config_save(NULL, &io, CFG_PATH));
    CHECK(!config_load(&c, NULL, CFG_PATH));
    CHECK(!config_save(&c, NULL, CFG_PATH));
}

TEST(every_theme_index_survives_a_round_trip)
{
    lib_io_t io = make_io();
    int i;

    for (i = 0; i < THEME_COUNT; i++) {
        dap_config_t in, out;
        fs_reset();
        config_defaults(&in);
        in.theme = (uint8_t)i;
        CHECK(config_save(&in, &io, CFG_PATH));
        CHECK(config_load(&out, &io, CFG_PATH));
        CHECK_EQ(out.theme, (uint8_t)i);
    }
}

int main(void)
{
    printf("config\n");

    RUN(defaults_are_sane);
    RUN(a_saved_config_reads_back_identical);
    RUN(a_missing_file_yields_defaults_not_an_error_worth_stopping_for);
    RUN(a_corrupt_file_yields_defaults);
    RUN(a_wrong_magic_yields_defaults);
    RUN(a_file_from_a_future_version_yields_defaults);
    RUN(a_truncated_file_yields_defaults);
    RUN(a_short_read_yields_defaults);
    RUN(out_of_range_values_are_clamped_on_load);
    RUN(clamp_fixes_every_field);
    RUN(saving_never_writes_a_value_it_could_not_read_back);
    RUN(a_failed_write_is_reported);
    RUN(null_arguments_are_safe);
    RUN(every_theme_index_survives_a_round_trip);

    return TEST_SUMMARY();
}
