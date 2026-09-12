/* test_library_index.c — reader tests.
 *
 * The fixtures are encoded by hand here (put_u16/put_u32/put_field) rather
 * than with lib_encode_*, so a bug that lives in both the encoder and the
 * decoder still fails the suite instead of cancelling itself out.
 *
 * The lib_io_t backend is a RAM file. It counts read/seek calls, which is how
 * the paging tests prove the cache actually avoids I/O.
 */

#include "test.h"

#include <stdlib.h>

#include "../src/core/library_index.h"

/* =====================================================================
 * RAM-backed lib_io_t
 * ===================================================================== */

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t pos;
    int      is_open;
    int      open_calls;
    int      close_calls;
    int      read_calls;
    int      seek_calls;
    int      fail_open;       /* open() returns an error */
    int      reads_until_fail;/* -1 = never fail, else fail once it hits 0 */
    int      short_read;      /* read() returns one byte less than asked */
} ramio_t;

static int ram_open(void *ctx, const char *path, int mode, void **fh)
{
    ramio_t *r = (ramio_t *)ctx;
    (void)path;
    (void)mode;
    r->open_calls++;
    if (r->fail_open) return -1;
    r->pos = 0;
    r->is_open = 1;
    *fh = r;
    return 0;
}

static int ram_read(void *ctx, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    ramio_t *r = (ramio_t *)ctx;
    uint32_t n;
    (void)fh;

    r->read_calls++;

    if (r->reads_until_fail > 0) {
        r->reads_until_fail--;
    } else if (r->reads_until_fail == 0) {
        return -1;
    }

    if (r->pos >= r->len) {
        *got = 0;
        return 0;
    }
    n = r->len - r->pos;
    if (n > len) n = len;
    if (r->short_read && n > 0u) n--;

    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    *got = n;
    return 0;
}

static int ram_seek(void *ctx, void *fh, uint32_t off)
{
    ramio_t *r = (ramio_t *)ctx;
    (void)fh;
    r->seek_calls++;
    if (off > r->len) return -1;
    r->pos = off;
    return 0;
}

static int ram_close(void *ctx, void *fh)
{
    ramio_t *r = (ramio_t *)ctx;
    (void)fh;
    r->close_calls++;
    r->is_open = 0;
    return 0;
}

static lib_io_t make_io(ramio_t *r)
{
    lib_io_t io;
    memset(&io, 0, sizeof(io));
    io.ctx = r;
    io.open = ram_open;
    io.read = ram_read;
    io.write = NULL;
    io.seek = ram_seek;
    io.close = ram_close;
    io.unlink = NULL;
    return io;
}

/* =====================================================================
 * Independent encoder
 * ===================================================================== */

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_field(uint8_t *p, const char *s, uint32_t cap)
{
    uint32_t i = 0;
    memset(p, 0, cap);
    while (s[i] != '\0' && i < cap) {
        p[i] = (uint8_t)s[i];
        i++;
    }
}

typedef struct {
    const char *title;
    const char *path;
    uint32_t    album;
    uint32_t    dur_ms;
    uint32_t    size;
    uint32_t    no;
    uint32_t    codec;
} ftrack_t;

typedef struct {
    const char *name;
    uint32_t    artist;
    uint32_t    track_first;
    uint32_t    track_count;
} falbum_t;

typedef struct {
    const char *name;
    uint32_t    album_first;
    uint32_t    album_count;
} fartist_t;

/* Builds a complete index image into a freshly malloc'd buffer. */
static uint8_t *build_image(const fartist_t *ar, uint32_t n_ar,
                            const falbum_t *al, uint32_t n_al,
                            const ftrack_t *tr, uint32_t n_tr,
                            uint32_t *out_len)
{
    uint32_t artist_off = LIB_HDR_SIZE;
    uint32_t album_off  = artist_off + n_ar * LIB_ARTIST_REC_SIZE;
    uint32_t strpool_off = album_off + n_al * LIB_ALBUM_REC_SIZE;
    uint32_t pool_len = 0;
    uint32_t track_off, total, i;
    uint32_t *ar_name_off = (uint32_t *)malloc((n_ar + 1u) * sizeof(uint32_t));
    uint32_t *al_name_off = (uint32_t *)malloc((n_al + 1u) * sizeof(uint32_t));
    char *pool;
    uint8_t *img;

    /* string pool: artist names then album names, each NUL-terminated */
    for (i = 0; i < n_ar; i++) pool_len += (uint32_t)strlen(ar[i].name) + 1u;
    for (i = 0; i < n_al; i++) pool_len += (uint32_t)strlen(al[i].name) + 1u;
    if (pool_len == 0u) pool_len = 1u;

    pool = (char *)malloc(pool_len);
    memset(pool, 0, pool_len);
    {
        uint32_t w = 0;
        for (i = 0; i < n_ar; i++) {
            ar_name_off[i] = w;
            strcpy(pool + w, ar[i].name);
            w += (uint32_t)strlen(ar[i].name) + 1u;
        }
        for (i = 0; i < n_al; i++) {
            al_name_off[i] = w;
            strcpy(pool + w, al[i].name);
            w += (uint32_t)strlen(al[i].name) + 1u;
        }
    }

    track_off = strpool_off + pool_len;
    total = track_off + n_tr * LIB_TRACK_REC_SIZE;

    img = (uint8_t *)malloc(total);
    memset(img, 0, total);

    memcpy(img, LIB_IDX_MAGIC, LIB_IDX_MAGIC_LEN);
    put_u32(img + 8,  LIB_IDX_VERSION);
    put_u32(img + 12, n_ar);
    put_u32(img + 16, n_al);
    put_u32(img + 20, n_tr);
    put_u32(img + 24, artist_off);
    put_u32(img + 28, album_off);
    put_u32(img + 32, strpool_off);
    put_u32(img + 36, pool_len);
    put_u32(img + 40, track_off);
    put_u32(img + 44, LIB_TRACK_REC_SIZE);
    put_u32(img + 48, 7u);          /* build_id */
    put_u32(img + 52, total);

    for (i = 0; i < n_ar; i++) {
        uint8_t *p = img + artist_off + i * LIB_ARTIST_REC_SIZE;
        put_u32(p + 0, ar_name_off[i]);
        put_u32(p + 4, ar[i].album_first);
        put_u32(p + 8, ar[i].album_count);
    }
    for (i = 0; i < n_al; i++) {
        uint8_t *p = img + album_off + i * LIB_ALBUM_REC_SIZE;
        put_u32(p + 0, al_name_off[i]);
        put_u32(p + 4, al[i].artist);
        put_u32(p + 8, al[i].track_first);
        put_u32(p + 12, al[i].track_count);
    }
    memcpy(img + strpool_off, pool, pool_len);
    for (i = 0; i < n_tr; i++) {
        uint8_t *p = img + track_off + i * LIB_TRACK_REC_SIZE;
        put_u32(p + 0, tr[i].album);
        put_u32(p + 4, tr[i].dur_ms);
        put_u32(p + 8, tr[i].size);
        put_u16(p + 12, tr[i].no);
        p[14] = (uint8_t)tr[i].codec;
        p[15] = 0;
        put_field(p + 32, tr[i].title, LIB_TITLE_MAX);
        put_field(p + 128, tr[i].path, LIB_PATH_MAX);
    }

    free(ar_name_off);
    free(al_name_off);
    free(pool);

    *out_len = total;
    return img;
}

/* =====================================================================
 * Small fixture: 2 artists, 3 albums, 10 tracks
 * ===================================================================== */

static const fartist_t F_AR[] = {
    { "Aphex Twin",       0u, 2u },
    { "Boards of Canada", 2u, 1u }
};

static const falbum_t F_AL[] = {
    { "Drukqs",   0u, 0u, 4u },
    { "SAW II",   0u, 4u, 2u },
    { "Geogaddi", 1u, 6u, 4u }
};

static const ftrack_t F_TR[] = {
    { "Jynweythek",   "/Music/Aphex Twin/Drukqs/01.flac",   0u, 92000u,  1000u, 1u, LIB_CODEC_FLAC },
    { "Vordhosbn",    "/Music/Aphex Twin/Drukqs/02.flac",   0u, 291000u, 2000u, 2u, LIB_CODEC_FLAC },
    { "Kladfvgbung",  "/Music/Aphex Twin/Drukqs/03.flac",   0u, 173000u, 3000u, 3u, LIB_CODEC_FLAC },
    { "Omgyjya",      "/Music/Aphex Twin/Drukqs/04.flac",   0u, 178000u, 4000u, 4u, LIB_CODEC_FLAC },
    { "Untitled 1",   "/Music/Aphex Twin/SAW II/01.wav",    1u, 260000u, 5000u, 1u, LIB_CODEC_WAV  },
    { "Untitled 2",   "/Music/Aphex Twin/SAW II/02.wav",    1u, 320000u, 6000u, 2u, LIB_CODEC_WAV  },
    { "Ready Lets Go","/Music/Boards of Canada/Geogaddi/01.mp3", 2u, 39000u,  7000u, 1u, LIB_CODEC_MP3 },
    { "Music Is Math","/Music/Boards of Canada/Geogaddi/02.mp3", 2u, 314000u, 8000u, 2u, LIB_CODEC_MP3 },
    { "Beware",       "/Music/Boards of Canada/Geogaddi/03.mp3", 2u, 97000u,  9000u, 3u, LIB_CODEC_MP3 },
    { "Gyroscope",    "/Music/Boards of Canada/Geogaddi/04.mp3", 2u, 209000u, 10000u,4u, LIB_CODEC_MP3 }
};

#define F_NAR  (sizeof(F_AR) / sizeof(F_AR[0]))
#define F_NAL  (sizeof(F_AL) / sizeof(F_AL[0]))
#define F_NTR  (sizeof(F_TR) / sizeof(F_TR[0]))

static uint8_t  g_arena[64u * 1024u];
static ramio_t  g_ram;
static lib_io_t g_io;
static library_t g_lib;
static uint8_t *g_img;
static uint32_t g_img_len;

static void fixture_load(void)
{
    if (g_img) free(g_img);
    g_img = build_image(F_AR, F_NAR, F_AL, F_NAL, F_TR, F_NTR, &g_img_len);
    memset(&g_ram, 0, sizeof(g_ram));
    g_ram.data = g_img;
    g_ram.len = g_img_len;
    g_ram.reads_until_fail = -1;
    g_io = make_io(&g_ram);
}

/* Loads the fixture and opens it; returns the open result. */
static int fixture_open(void)
{
    fixture_load();
    return library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena));
}

/* =====================================================================
 * byte helpers
 * ===================================================================== */

TEST(le_helpers_roundtrip)
{
    uint8_t b[4];

    lib_wr_u32(b, 0x12345678u);
    CHECK_EQ(b[0], 0x78);
    CHECK_EQ(b[1], 0x56);
    CHECK_EQ(b[2], 0x34);
    CHECK_EQ(b[3], 0x12);
    CHECK_EQ(lib_rd_u32(b), 0x12345678u);

    lib_wr_u32(b, 0xFFFFFFFFu);
    CHECK_EQ(lib_rd_u32(b), 0xFFFFFFFFu);
    lib_wr_u32(b, 0u);
    CHECK_EQ(lib_rd_u32(b), 0u);

    lib_wr_u16(b, 0xBEEFu);
    CHECK_EQ(b[0], 0xEF);
    CHECK_EQ(b[1], 0xBE);
    CHECK_EQ(lib_rd_u16(b), 0xBEEFu);
}

TEST(track_record_roundtrips_through_the_codec)
{
    uint8_t rec[LIB_TRACK_REC_SIZE];
    lib_track_t in, out;

    memset(&in, 0, sizeof(in));
    in.album_idx = 12u;
    in.duration_ms = 245000u;
    in.file_size = 40u * 1024u * 1024u;
    in.track_no = 7u;
    in.codec = LIB_CODEC_FLAC;
    in.flags = 0x81u;
    strcpy(in.title, "Everything In Its Right Place");
    strcpy(in.path, "/Music/Radiohead/Kid A/01.flac");

    lib_encode_track(rec, &in);
    lib_decode_track(rec, &out);

    CHECK_EQ(out.album_idx, in.album_idx);
    CHECK_EQ(out.duration_ms, in.duration_ms);
    CHECK_EQ(out.file_size, in.file_size);
    CHECK_EQ(out.track_no, in.track_no);
    CHECK_EQ(out.codec, in.codec);
    CHECK_EQ(out.flags, in.flags);
    CHECK(strcmp(out.title, in.title) == 0);
    CHECK(strcmp(out.path, in.path) == 0);

    /* unused tail must be zero, not stack garbage leaking onto the card */
    CHECK_EQ(rec[16], 0);
    CHECK_EQ(rec[31], 0);
    CHECK_EQ(rec[32 + strlen(in.title)], 0);
}

TEST(full_width_strings_stay_terminated)
{
    uint8_t rec[LIB_TRACK_REC_SIZE];
    lib_track_t in, out;
    uint32_t i;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < LIB_TITLE_MAX; i++) in.title[i] = 'T';
    in.title[LIB_TITLE_MAX] = '\0';
    for (i = 0; i < LIB_PATH_MAX; i++) in.path[i] = 'p';
    in.path[LIB_PATH_MAX] = '\0';

    lib_encode_track(rec, &in);
    lib_decode_track(rec, &out);

    /* the on-card field is full, so there is no NUL in it — the decoder has to
     * supply one from the +1 byte */
    CHECK_EQ(rec[32 + LIB_TITLE_MAX - 1u], 'T');
    CHECK_EQ(strlen(out.title), LIB_TITLE_MAX);
    CHECK_EQ(strlen(out.path), LIB_PATH_MAX);
    CHECK_EQ(out.title[LIB_TITLE_MAX], '\0');
    CHECK_EQ(out.path[LIB_PATH_MAX], '\0');
}

TEST(overlong_strings_truncate_rather_than_overrun)
{
    uint8_t rec[LIB_TRACK_REC_SIZE + 8u];
    lib_track_t in;
    char big[LIB_PATH_MAX * 2u];
    uint32_t i;

    memset(rec, 0xAA, sizeof(rec));
    for (i = 0; i < sizeof(big) - 1u; i++) big[i] = 'x';
    big[sizeof(big) - 1u] = '\0';

    memset(&in, 0, sizeof(in));
    memcpy(in.title, big, LIB_TITLE_MAX);
    memcpy(in.path, big, LIB_PATH_MAX);

    lib_encode_track(rec, &in);

    /* nothing written past the record */
    for (i = 0; i < 8u; i++) {
        CHECK_EQ(rec[LIB_TRACK_REC_SIZE + i], 0xAA);
    }
}

/* =====================================================================
 * opening
 * ===================================================================== */

TEST(open_succeeds_and_reports_counts)
{
    CHECK_EQ(fixture_open(), LIB_OK);
    CHECK_EQ(library_artist_count(&g_lib), F_NAR);
    CHECK_EQ(library_album_count(&g_lib), F_NAL);
    CHECK_EQ(library_track_count(&g_lib), F_NTR);
    CHECK_EQ(g_lib.hdr.build_id, 7u);
    CHECK_EQ(g_lib.hdr.total_size, g_img_len);
    library_close(&g_lib);
    CHECK_EQ(g_ram.close_calls, 1);
}

TEST(arena_bytes_matches_what_open_consumes)
{
    uint32_t need;

    CHECK_EQ(fixture_open(), LIB_OK);
    need = library_arena_bytes(&g_lib.hdr);

    /* tables + pool + one page cache, nothing more */
    CHECK(need >= F_NAR * sizeof(lib_artist_t) + F_NAL * sizeof(lib_album_t)
                 + g_lib.hdr.strpool_len + LIB_PAGE_TRACKS * LIB_TRACK_REC_SIZE);
    CHECK(need < 4096u);

    /* every pointer lands inside the arena */
    CHECK((uint8_t *)g_lib.artists >= g_arena);
    CHECK((uint8_t *)g_lib.page + LIB_PAGE_TRACKS * LIB_TRACK_REC_SIZE
              <= g_arena + need);
    library_close(&g_lib);
}

TEST(arena_too_small_is_refused_with_a_size)
{
    uint32_t need;
    int rc;

    fixture_load();
    rc = library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, 64u);
    CHECK_EQ(rc, LIB_E_NOMEM);
    CHECK(g_lib.required_bytes > 64u);
    CHECK_EQ(g_lib.is_open, 0);
    CHECK_EQ(g_ram.close_calls, 1);   /* the file was not left open */

    need = g_lib.required_bytes;
    rc = library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, need);
    CHECK_EQ(rc, LIB_OK);             /* exactly the reported size is enough */
    library_close(&g_lib);
}

TEST(open_failure_is_reported)
{
    fixture_load();
    g_ram.fail_open = 1;
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_IO);
    CHECK_EQ(g_lib.is_open, 0);
}

TEST(short_read_during_open_is_an_error_not_a_partial_load)
{
    fixture_load();
    g_ram.short_read = 1;
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_IO);
    CHECK_EQ(g_lib.is_open, 0);
    CHECK_EQ(g_ram.close_calls, 1);
}

TEST(read_failure_during_open_closes_the_file)
{
    fixture_load();
    g_ram.reads_until_fail = 0;
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_IO);
    CHECK_EQ(g_ram.close_calls, 1);
    CHECK_EQ(g_ram.is_open, 0);
}

TEST(truncated_file_is_rejected)
{
    fixture_load();
    g_ram.len = 32u;   /* less than a header */
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_IO);
}

/* =====================================================================
 * header validation
 * ===================================================================== */

/* Corrupts one 32-bit header field and checks the open is refused. */
static void expect_header_reject(uint32_t off, uint32_t value, int expect)
{
    fixture_load();
    put_u32(g_img + off, value);
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             expect);
    CHECK_EQ(g_lib.is_open, 0);
}

TEST(bad_magic_is_rejected)
{
    fixture_load();
    g_img[0] = 'X';
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);

    fixture_load();
    g_img[7] = '9';
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);
}

TEST(wrong_version_is_rejected)
{
    expect_header_reject(8u, LIB_IDX_VERSION + 1u, LIB_E_FORMAT);
    expect_header_reject(8u, 0u, LIB_E_FORMAT);
}

TEST(wrong_record_size_is_rejected)
{
    expect_header_reject(44u, 256u, LIB_E_FORMAT);
    expect_header_reject(44u, 0u, LIB_E_FORMAT);
}

TEST(sections_must_fit_inside_the_file)
{
    /* track table claimed past the end */
    expect_header_reject(20u, 100000u, LIB_E_FORMAT);
    /* string pool claimed past the end */
    expect_header_reject(36u, 0x00FFFFFFu, LIB_E_FORMAT);
    /* a section starting inside the header */
    expect_header_reject(24u, 4u, LIB_E_FORMAT);
    expect_header_reject(40u, 0u, LIB_E_FORMAT);
}

TEST(absurd_counts_are_rejected_without_allocating)
{
    expect_header_reject(12u, 0x7FFFFFFFu, LIB_E_FORMAT);
    expect_header_reject(16u, 0x7FFFFFFFu, LIB_E_FORMAT);
}

TEST(unterminated_string_pool_is_rejected)
{
    uint32_t pool_off;

    fixture_load();
    pool_off = lib_rd_u32(g_img + 32);
    g_img[pool_off + lib_rd_u32(g_img + 36) - 1u] = 'z';
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);
}

TEST(out_of_pool_name_offset_is_rejected)
{
    fixture_load();
    put_u32(g_img + LIB_HDR_SIZE + 0, 99999u);   /* artist 0 name_off */
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);
}

TEST(album_range_outside_the_track_table_is_rejected)
{
    uint32_t album_off;

    fixture_load();
    album_off = lib_rd_u32(g_img + 28);
    put_u32(g_img + album_off + 12, 9999u);      /* album 0 track_count */
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);

    fixture_load();
    album_off = lib_rd_u32(g_img + 28);
    put_u32(g_img + album_off + 4, 77u);         /* album 0 artist_idx */
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);
}

TEST(artist_album_range_must_be_inside_the_album_table)
{
    fixture_load();
    put_u32(g_img + LIB_HDR_SIZE + 8, 50u);      /* artist 0 album_count */
    CHECK_EQ(library_open(&g_lib, &g_io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_E_FORMAT);
}

/* =====================================================================
 * navigation
 * ===================================================================== */

TEST(artist_names_come_back)
{
    CHECK_EQ(fixture_open(), LIB_OK);
    CHECK(strcmp(library_artist_name(&g_lib, 0u), "Aphex Twin") == 0);
    CHECK(strcmp(library_artist_name(&g_lib, 1u), "Boards of Canada") == 0);
    library_close(&g_lib);
}

TEST(albums_are_grouped_under_their_artist)
{
    uint32_t a;

    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK_EQ(library_artist_album_count(&g_lib, 0u), 2u);
    CHECK_EQ(library_artist_album_count(&g_lib, 1u), 1u);

    a = library_artist_album(&g_lib, 0u, 0u);
    CHECK_EQ(a, 0u);
    CHECK(strcmp(library_album_name(&g_lib, a), "Drukqs") == 0);
    CHECK_EQ(library_album_artist(&g_lib, a), 0u);
    CHECK_EQ(library_album_track_count(&g_lib, a), 4u);

    a = library_artist_album(&g_lib, 0u, 1u);
    CHECK_EQ(a, 1u);
    CHECK(strcmp(library_album_name(&g_lib, a), "SAW II") == 0);

    a = library_artist_album(&g_lib, 1u, 0u);
    CHECK_EQ(a, 2u);
    CHECK(strcmp(library_album_name(&g_lib, a), "Geogaddi") == 0);
    CHECK_EQ(library_album_artist(&g_lib, a), 1u);

    library_close(&g_lib);
}

TEST(out_of_range_navigation_is_safe)
{
    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK(strcmp(library_artist_name(&g_lib, 99u), "") == 0);
    CHECK(strcmp(library_album_name(&g_lib, 99u), "") == 0);
    CHECK_EQ(library_artist_album_count(&g_lib, 99u), 0u);
    CHECK_EQ(library_artist_album(&g_lib, 0u, 9u), 0xFFFFFFFFu);
    CHECK_EQ(library_artist_album(&g_lib, 99u, 0u), 0xFFFFFFFFu);
    CHECK_EQ(library_album_track_count(&g_lib, 99u), 0u);
    CHECK_EQ(library_album_artist(&g_lib, 99u), 0xFFFFFFFFu);

    library_close(&g_lib);
}

TEST(accessors_on_a_closed_library_are_safe)
{
    lib_track_t t;

    CHECK_EQ(fixture_open(), LIB_OK);
    library_close(&g_lib);

    CHECK_EQ(library_artist_count(&g_lib), 0u);
    CHECK_EQ(library_album_count(&g_lib), 0u);
    CHECK_EQ(library_track_count(&g_lib), 0u);
    CHECK(strcmp(library_artist_name(&g_lib, 0u), "") == 0);
    CHECK_EQ(library_track_global(&g_lib, 0u, &t), LIB_E_ARG);
    CHECK_EQ(library_album_track(&g_lib, 0u, 0u, &t), LIB_E_ARG);
}

/* =====================================================================
 * track reads
 * ===================================================================== */

TEST(track_fields_survive_the_round_trip)
{
    lib_track_t t;
    uint32_t i;

    CHECK_EQ(fixture_open(), LIB_OK);

    for (i = 0; i < F_NTR; i++) {
        CHECK_EQ(library_track_global(&g_lib, i, &t), LIB_OK);
        CHECK_EQ(t.album_idx, F_TR[i].album);
        CHECK_EQ(t.duration_ms, F_TR[i].dur_ms);
        CHECK_EQ(t.file_size, F_TR[i].size);
        CHECK_EQ(t.track_no, F_TR[i].no);
        CHECK_EQ(t.codec, F_TR[i].codec);
        CHECK(strcmp(t.title, F_TR[i].title) == 0);
        CHECK(strcmp(t.path, F_TR[i].path) == 0);
    }

    library_close(&g_lib);
}

TEST(album_relative_indexing_hits_the_right_track)
{
    lib_track_t t;

    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK_EQ(library_album_track(&g_lib, 0u, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Jynweythek") == 0);

    CHECK_EQ(library_album_track(&g_lib, 1u, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Untitled 1") == 0);
    CHECK_EQ(t.album_idx, 1u);

    CHECK_EQ(library_album_track(&g_lib, 2u, 3u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Gyroscope") == 0);
    CHECK_EQ(t.track_no, 4u);

    library_close(&g_lib);
}

TEST(track_index_out_of_range_is_rejected)
{
    lib_track_t t;

    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK_EQ(library_track_global(&g_lib, F_NTR, &t), LIB_E_RANGE);
    CHECK_EQ(library_track_global(&g_lib, 0xFFFFFFFFu, &t), LIB_E_RANGE);
    CHECK_EQ(library_album_track(&g_lib, 0u, 4u, &t), LIB_E_RANGE);
    CHECK_EQ(library_album_track(&g_lib, 9u, 0u, &t), LIB_E_RANGE);
    CHECK_EQ(library_track_global(&g_lib, 0u, NULL), LIB_E_ARG);

    library_close(&g_lib);
}

TEST(reading_the_same_page_twice_does_no_io)
{
    lib_track_t t;
    int reads;

    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK_EQ(library_track_global(&g_lib, 0u, &t), LIB_OK);
    reads = g_ram.read_calls;

    /* tracks 1..7 share page 0 with track 0 */
    CHECK_EQ(library_track_global(&g_lib, 1u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Vordhosbn") == 0);
    CHECK_EQ(library_track_global(&g_lib, 7u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Music Is Math") == 0);
    CHECK_EQ(library_track_global(&g_lib, 0u, &t), LIB_OK);

    CHECK_EQ(g_ram.read_calls, reads);

    /* track 8 is on the next page */
    CHECK_EQ(library_track_global(&g_lib, 8u, &t), LIB_OK);
    CHECK_EQ(g_ram.read_calls, reads + 1);

    library_close(&g_lib);
}

TEST(the_last_partial_page_reads_only_what_exists)
{
    lib_track_t t;

    CHECK_EQ(fixture_open(), LIB_OK);

    /* 10 tracks, page size 8 -> page 1 holds 2 records, and the read must not
     * run off the end of the file */
    CHECK_EQ(library_track_global(&g_lib, 9u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Gyroscope") == 0);
    CHECK_EQ(g_lib.page_first, 8u);
    CHECK_EQ(g_lib.page_n, 2u);

    library_close(&g_lib);
}

TEST(a_failed_page_read_invalidates_the_cache)
{
    lib_track_t t;

    CHECK_EQ(fixture_open(), LIB_OK);

    CHECK_EQ(library_track_global(&g_lib, 0u, &t), LIB_OK);
    g_ram.reads_until_fail = 0;
    CHECK_EQ(library_track_global(&g_lib, 8u, &t), LIB_E_IO);
    CHECK_EQ(g_lib.page_n, 0u);

    /* recovering: a later successful read must refill, not trust stale bytes */
    g_ram.reads_until_fail = -1;
    CHECK_EQ(library_track_global(&g_lib, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Jynweythek") == 0);

    library_close(&g_lib);
}

/* =====================================================================
 * scale: 3000 tracks
 * ===================================================================== */

TEST(a_three_thousand_track_library_stays_small_in_ram)
{
    enum { N_AR = 100, N_AL = 300, N_TR = 3000 };
    static char ar_names[N_AR][24];
    static char al_names[N_AL][24];
    static char tr_titles[N_TR][24];
    static char tr_paths[N_TR][80];
    static fartist_t ar[N_AR];
    static falbum_t  al[N_AL];
    static ftrack_t  tr[N_TR];
    static uint8_t   arena[64u * 1024u];
    uint8_t *img;
    uint32_t len, i, need;
    ramio_t ram;
    lib_io_t io;
    library_t lib;
    lib_track_t t;
    int reads;

    for (i = 0; i < N_AR; i++) {
        sprintf(ar_names[i], "Artist %03u", i);
        ar[i].name = ar_names[i];
        ar[i].album_first = i * 3u;
        ar[i].album_count = 3u;
    }
    for (i = 0; i < N_AL; i++) {
        sprintf(al_names[i], "Album %03u", i);
        al[i].name = al_names[i];
        al[i].artist = i / 3u;
        al[i].track_first = i * 10u;
        al[i].track_count = 10u;
    }
    for (i = 0; i < N_TR; i++) {
        sprintf(tr_titles[i], "Track %04u", i);
        sprintf(tr_paths[i], "/Music/Artist %03u/Album %03u/%02u.flac",
                i / 30u, i / 10u, (i % 10u) + 1u);
        tr[i].title = tr_titles[i];
        tr[i].path = tr_paths[i];
        tr[i].album = i / 10u;
        tr[i].dur_ms = 180000u + i;
        tr[i].size = 30000000u + i;
        tr[i].no = (i % 10u) + 1u;
        tr[i].codec = LIB_CODEC_FLAC;
    }

    img = build_image(ar, N_AR, al, N_AL, tr, N_TR, &len);
    memset(&ram, 0, sizeof(ram));
    ram.data = img;
    ram.len = len;
    ram.reads_until_fail = -1;
    io = make_io(&ram);

    CHECK_EQ(library_open(&lib, &io, LIB_INDEX_PATH, arena, sizeof(arena)),
             LIB_OK);

    need = library_arena_bytes(&lib.hdr);
    /* the whole point: ~1 MB of index, but RAM stays under 20 KB */
    CHECK(len > 900000u);
    CHECK(need < 20u * 1024u);

    /* opening reads the header, the pool and the two tables — and no tracks */
    CHECK(ram.read_calls < 20);

    /* random access anywhere costs one page read */
    reads = ram.read_calls;
    CHECK_EQ(library_track_global(&lib, 2999u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Track 2999") == 0);
    CHECK_EQ(ram.read_calls, reads + 1);

    CHECK_EQ(library_album_track(&lib, 299u, 9u, &t), LIB_OK);
    CHECK_EQ(t.album_idx, 299u);
    CHECK_EQ(t.track_no, 10u);

    CHECK_EQ(library_track_global(&lib, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Track 0000") == 0);

    /* a full sequential walk costs exactly one read per page */
    ram.read_calls = 0;
    lib.page_first = 0xFFFFFFFFu;
    lib.page_n = 0;
    for (i = 0; i < N_TR; i++) {
        CHECK_EQ(library_track_global(&lib, i, &t), LIB_OK);
    }
    CHECK_EQ(ram.read_calls, (int)(N_TR / LIB_PAGE_TRACKS));

    library_close(&lib);
    free(img);
}

TEST(an_empty_library_opens_cleanly)
{
    uint8_t *img;
    uint32_t len;
    ramio_t ram;
    lib_io_t io;
    library_t lib;
    lib_track_t t;

    img = build_image(NULL, 0u, NULL, 0u, NULL, 0u, &len);
    memset(&ram, 0, sizeof(ram));
    ram.data = img;
    ram.len = len;
    ram.reads_until_fail = -1;
    io = make_io(&ram);

    CHECK_EQ(library_open(&lib, &io, LIB_INDEX_PATH, g_arena, sizeof(g_arena)),
             LIB_OK);
    CHECK_EQ(library_artist_count(&lib), 0u);
    CHECK_EQ(library_track_count(&lib), 0u);
    CHECK_EQ(library_track_global(&lib, 0u, &t), LIB_E_RANGE);
    library_close(&lib);
    free(img);
}

int main(void)
{
    printf("library_index\n");

    RUN(le_helpers_roundtrip);
    RUN(track_record_roundtrips_through_the_codec);
    RUN(full_width_strings_stay_terminated);
    RUN(overlong_strings_truncate_rather_than_overrun);

    RUN(open_succeeds_and_reports_counts);
    RUN(arena_bytes_matches_what_open_consumes);
    RUN(arena_too_small_is_refused_with_a_size);
    RUN(open_failure_is_reported);
    RUN(short_read_during_open_is_an_error_not_a_partial_load);
    RUN(read_failure_during_open_closes_the_file);
    RUN(truncated_file_is_rejected);

    RUN(bad_magic_is_rejected);
    RUN(wrong_version_is_rejected);
    RUN(wrong_record_size_is_rejected);
    RUN(sections_must_fit_inside_the_file);
    RUN(absurd_counts_are_rejected_without_allocating);
    RUN(unterminated_string_pool_is_rejected);
    RUN(out_of_pool_name_offset_is_rejected);
    RUN(album_range_outside_the_track_table_is_rejected);
    RUN(artist_album_range_must_be_inside_the_album_table);

    RUN(artist_names_come_back);
    RUN(albums_are_grouped_under_their_artist);
    RUN(out_of_range_navigation_is_safe);
    RUN(accessors_on_a_closed_library_are_safe);

    RUN(track_fields_survive_the_round_trip);
    RUN(album_relative_indexing_hits_the_right_track);
    RUN(track_index_out_of_range_is_rejected);
    RUN(reading_the_same_page_twice_does_no_io);
    RUN(the_last_partial_page_reads_only_what_exists);
    RUN(a_failed_page_read_invalidates_the_cache);

    RUN(a_three_thousand_track_library_stays_small_in_ram);
    RUN(an_empty_library_opens_cleanly);

    if (g_img) free(g_img);
    return TEST_SUMMARY();
}
