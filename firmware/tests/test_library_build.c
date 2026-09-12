/* test_library_build.c — scan a fake card, then open the result with the
 * real reader. Nothing here is mocked except the card itself: the bytes that
 * libidx_open() validates are the bytes libidx_scan() actually wrote.
 *
 * The card is a flat list of file paths; directories are inferred. Entries
 * come back from readdir in the order they were added, and the fixtures add
 * them deliberately scrambled, so any ordering the tests assert on has to
 * come from the builder's sort rather than from discovery order.
 */

#include "test.h"

#include <stdlib.h>

#include "../src/core/library_build.h"
#include "../src/core/library_index.h"

/* =====================================================================
 * fake card: file list
 * ===================================================================== */

#define CARD_MAX 512

typedef struct {
    const char *path;
    uint32_t    size;
} cardfile_t;

static cardfile_t g_card[CARD_MAX];
static uint32_t   g_ncard;

static void card_reset(void)
{
    g_ncard = 0;
    memset(g_card, 0, sizeof(g_card));
}

static void card_add(const char *path, uint32_t size)
{
    if (g_ncard < CARD_MAX) {
        g_card[g_ncard].path = path;
        g_card[g_ncard].size = size;
        g_ncard++;
    }
}

/* =====================================================================
 * fake card: directories
 * ===================================================================== */

#define VDIR_MAX_ENTS 64
#define VDIR_POOL     (LIB_BUILD_MAX_DEPTH + 2u)

typedef struct {
    lib_dirent_t ents[VDIR_MAX_ENTS];
    uint32_t     n;
    uint32_t     pos;
    int          in_use;
} vdir_t;

static vdir_t g_vdirs[VDIR_POOL];
static int    g_opendir_fail;

static void vdir_push(vdir_t *d, const char *name, int is_dir, uint32_t size)
{
    uint32_t i;
    for (i = 0; i < d->n; i++) {
        if (strcmp(d->ents[i].name, name) == 0) return;   /* dedup dirs */
    }
    if (d->n >= VDIR_MAX_ENTS) return;
    strncpy(d->ents[d->n].name, name, LIB_FILENAME_MAX - 1u);
    d->ents[d->n].name[LIB_FILENAME_MAX - 1u] = '\0';
    d->ents[d->n].is_dir = is_dir;
    d->ents[d->n].size = size;
    d->n++;
}

static int v_opendir(void *ctx, const char *path, void **dh)
{
    vdir_t *d = NULL;
    uint32_t i, plen;
    int is_root;

    (void)ctx;
    if (g_opendir_fail) return -1;

    for (i = 0; i < VDIR_POOL; i++) {
        if (!g_vdirs[i].in_use) { d = &g_vdirs[i]; break; }
    }
    if (!d) return -1;

    memset(d, 0, sizeof(*d));
    d->in_use = 1;

    is_root = (strcmp(path, "/") == 0);
    plen = is_root ? 0u : (uint32_t)strlen(path);

    for (i = 0; i < g_ncard; i++) {
        const char *p = g_card[i].path;
        const char *rest;
        const char *slash;

        if (!is_root) {
            if (strncmp(p, path, plen) != 0) continue;
            if (p[plen] != '/') continue;
        }
        rest = p + plen + 1u;
        if (rest[0] == '\0') continue;

        slash = strchr(rest, '/');
        if (slash) {
            char dirname[LIB_FILENAME_MAX];
            size_t n = (size_t)(slash - rest);
            if (n >= LIB_FILENAME_MAX) n = LIB_FILENAME_MAX - 1u;
            memcpy(dirname, rest, n);
            dirname[n] = '\0';
            vdir_push(d, dirname, 1, 0u);
        } else {
            vdir_push(d, rest, 0, g_card[i].size);
        }
    }

    *dh = d;
    return 0;
}

static int v_readdir(void *ctx, void *dh, lib_dirent_t *out, int *done)
{
    vdir_t *d = (vdir_t *)dh;
    (void)ctx;
    if (d->pos >= d->n) { *done = 1; return 0; }
    *out = d->ents[d->pos++];
    *done = 0;
    return 0;
}

static int v_closedir(void *ctx, void *dh)
{
    vdir_t *d = (vdir_t *)dh;
    (void)ctx;
    d->in_use = 0;
    return 0;
}

static lib_dir_t make_dir(void)
{
    lib_dir_t dir;
    memset(&dir, 0, sizeof(dir));
    dir.opendir = v_opendir;
    dir.readdir = v_readdir;
    dir.closedir = v_closedir;
    return dir;
}

/* =====================================================================
 * fake card: writable files (the temp file and the index)
 * ===================================================================== */

#define VF_MAX 4

typedef struct {
    char     name[64];
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    int      in_use;
} vfile_t;

typedef struct {
    vfile_t *f;
    uint32_t pos;
    int      in_use;
} vhandle_t;

static vfile_t   g_vf[VF_MAX];
static vhandle_t g_vh[VF_MAX];
static int       g_write_fail_after;   /* -1 = never */
static int       g_write_calls;

static void vfs_reset(void)
{
    uint32_t i;
    for (i = 0; i < VF_MAX; i++) {
        if (g_vf[i].data) free(g_vf[i].data);
    }
    memset(g_vf, 0, sizeof(g_vf));
    memset(g_vh, 0, sizeof(g_vh));
    g_write_fail_after = -1;
    g_write_calls = 0;
}

static vfile_t *vfs_find(const char *name)
{
    uint32_t i;
    for (i = 0; i < VF_MAX; i++) {
        if (g_vf[i].in_use && strcmp(g_vf[i].name, name) == 0) return &g_vf[i];
    }
    return NULL;
}

static int v_open(void *ctx, const char *path, int mode, void **fh)
{
    vfile_t *f;
    vhandle_t *h = NULL;
    uint32_t i;

    (void)ctx;
    f = vfs_find(path);

    if (mode == LIB_IO_WRITE) {
        if (!f) {
            for (i = 0; i < VF_MAX; i++) {
                if (!g_vf[i].in_use) { f = &g_vf[i]; break; }
            }
            if (!f) return -1;
            memset(f, 0, sizeof(*f));
            strncpy(f->name, path, sizeof(f->name) - 1u);
            f->in_use = 1;
        }
        f->len = 0;
    } else {
        if (!f) return -1;
    }

    for (i = 0; i < VF_MAX; i++) {
        if (!g_vh[i].in_use) { h = &g_vh[i]; break; }
    }
    if (!h) return -1;
    h->f = f;
    h->pos = 0;
    h->in_use = 1;
    *fh = h;
    return 0;
}

static int v_read(void *ctx, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    vhandle_t *h = (vhandle_t *)fh;
    uint32_t n;
    (void)ctx;

    if (h->pos >= h->f->len) { *got = 0; return 0; }
    n = h->f->len - h->pos;
    if (n > len) n = len;
    memcpy(dst, h->f->data + h->pos, n);
    h->pos += n;
    *got = n;
    return 0;
}

static int v_write(void *ctx, void *fh, const void *src, uint32_t len)
{
    vhandle_t *h = (vhandle_t *)fh;
    (void)ctx;

    g_write_calls++;
    if (g_write_fail_after > 0) {
        g_write_fail_after--;
    } else if (g_write_fail_after == 0) {
        return -1;
    }

    if (h->pos + len > h->f->cap) {
        uint32_t cap = (h->f->cap ? h->f->cap : 1024u);
        uint8_t *nd;
        while (cap < h->pos + len) cap *= 2u;
        nd = (uint8_t *)realloc(h->f->data, cap);
        if (!nd) return -1;
        h->f->data = nd;
        h->f->cap = cap;
    }
    memcpy(h->f->data + h->pos, src, len);
    h->pos += len;
    if (h->pos > h->f->len) h->f->len = h->pos;
    return 0;
}

static int v_seek(void *ctx, void *fh, uint32_t off)
{
    vhandle_t *h = (vhandle_t *)fh;
    (void)ctx;
    if (off > h->f->len) return -1;
    h->pos = off;
    return 0;
}

static int v_close(void *ctx, void *fh)
{
    vhandle_t *h = (vhandle_t *)fh;
    (void)ctx;
    h->in_use = 0;
    return 0;
}

static int v_unlink(void *ctx, const char *path)
{
    vfile_t *f = vfs_find(path);
    (void)ctx;
    if (!f) return -1;
    if (f->data) free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}

static lib_io_t make_io(void)
{
    lib_io_t io;
    memset(&io, 0, sizeof(io));
    io.open = v_open;
    io.read = v_read;
    io.write = v_write;
    io.seek = v_seek;
    io.close = v_close;
    io.unlink = v_unlink;
    return io;
}

/* =====================================================================
 * harness
 * ===================================================================== */

static uint8_t  g_build_arena[256u * 1024u];
static uint8_t  g_read_arena[64u * 1024u];
static lib_io_t g_io;
static lib_dir_t g_dir;
static libidx_scan_stats_t g_stats;
static libidx_t g_lib;

static void cfg_defaults(libidx_scan_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->io = &g_io;
    cfg->dir = &g_dir;
    cfg->root = "/";
    cfg->build_id = 42u;
    cfg->max_tracks = 4000u;
    cfg->max_artists = 512u;
    cfg->max_albums = 2048u;
    cfg->pool_bytes = 48u * 1024u;
    cfg->arena = g_build_arena;
    cfg->arena_len = sizeof(g_build_arena);
}

static void env_reset(void)
{
    vfs_reset();
    memset(g_vdirs, 0, sizeof(g_vdirs));
    g_opendir_fail = 0;
    g_io = make_io();
    g_dir = make_dir();
}

/* Builds, then opens the index it produced. Returns the build result. */
static int build_and_open(libidx_scan_cfg_t *cfg)
{
    int rc = libidx_scan(cfg, &g_stats);
    if (rc != LIB_OK) return rc;
    return libidx_open(&g_lib, &g_io, LIB_INDEX_PATH,
                        g_read_arena, sizeof(g_read_arena));
}

/* index of the artist with this name, or 0xFFFFFFFF */
static uint32_t find_artist(const char *name)
{
    uint32_t i;
    for (i = 0; i < libidx_artist_count(&g_lib); i++) {
        if (strcmp(libidx_artist_name(&g_lib, i), name) == 0) return i;
    }
    return 0xFFFFFFFFu;
}

/* =====================================================================
 * path-derived tags
 * ===================================================================== */

TEST(codec_comes_from_the_extension)
{
    CHECK_EQ(lib_codec_from_path("/a/b/c.wav"), LIB_CODEC_WAV);
    CHECK_EQ(lib_codec_from_path("/a/b/c.flac"), LIB_CODEC_FLAC);
    CHECK_EQ(lib_codec_from_path("/a/b/c.mp3"), LIB_CODEC_MP3);
    CHECK_EQ(lib_codec_from_path("/a/b/c.MP3"), LIB_CODEC_MP3);
    CHECK_EQ(lib_codec_from_path("/a/b/c.FLAC"), LIB_CODEC_FLAC);
    CHECK_EQ(lib_codec_from_path("/a/b/c.txt"), LIB_CODEC_UNKNOWN);
    CHECK_EQ(lib_codec_from_path("/a/b/noext"), LIB_CODEC_UNKNOWN);
    CHECK_EQ(lib_codec_from_path("/a.flac/b"), LIB_CODEC_UNKNOWN);
    CHECK_EQ(lib_codec_from_path(""), LIB_CODEC_UNKNOWN);
}

TEST(tags_come_from_the_folder_layout)
{
    lib_tags_t t;

    lib_tags_from_path("/Music/Radiohead/Kid A/03 The National Anthem.flac", &t);
    CHECK(strcmp(t.artist, "Radiohead") == 0);
    CHECK(strcmp(t.album, "Kid A") == 0);
    CHECK(strcmp(t.title, "The National Anthem") == 0);
    CHECK_EQ(t.track_no, 3u);
}

TEST(track_number_prefixes_are_stripped)
{
    lib_tags_t t;

    lib_tags_from_path("/A/B/01 One.mp3", &t);
    CHECK(strcmp(t.title, "One") == 0);
    CHECK_EQ(t.track_no, 1u);

    lib_tags_from_path("/A/B/07-Seven.mp3", &t);
    CHECK(strcmp(t.title, "Seven") == 0);
    CHECK_EQ(t.track_no, 7u);

    lib_tags_from_path("/A/B/12_Twelve.mp3", &t);
    CHECK(strcmp(t.title, "Twelve") == 0);
    CHECK_EQ(t.track_no, 12u);

    lib_tags_from_path("/A/B/100 Hundred.mp3", &t);
    CHECK(strcmp(t.title, "Hundred") == 0);
    CHECK_EQ(t.track_no, 100u);

    /* a number that is part of the title, not a prefix */
    lib_tags_from_path("/A/B/1999.mp3", &t);
    CHECK(strcmp(t.title, "1999") == 0);
    CHECK_EQ(t.track_no, 0u);

    lib_tags_from_path("/A/B/No Number Here.flac", &t);
    CHECK(strcmp(t.title, "No Number Here") == 0);
    CHECK_EQ(t.track_no, 0u);
}

TEST(shallow_paths_fall_back_to_unknown)
{
    lib_tags_t t;

    lib_tags_from_path("/loose.mp3", &t);
    CHECK(strcmp(t.artist, "Unknown Artist") == 0);
    CHECK(strcmp(t.album, "Unknown Album") == 0);
    CHECK(strcmp(t.title, "loose") == 0);

    lib_tags_from_path("/Album Only/track.mp3", &t);
    CHECK(strcmp(t.artist, "Unknown Artist") == 0);
    CHECK(strcmp(t.album, "Album Only") == 0);

    lib_tags_from_path("/A/B/.flac", &t);
    CHECK(strcmp(t.title, "Untitled") == 0);
}

TEST(deep_paths_use_the_two_nearest_folders)
{
    lib_tags_t t;

    lib_tags_from_path("/Music/Lossless/2001/Artist/Album/02 Song.flac", &t);
    CHECK(strcmp(t.artist, "Artist") == 0);
    CHECK(strcmp(t.album, "Album") == 0);
    CHECK(strcmp(t.title, "Song") == 0);
}

/* =====================================================================
 * a small card, end to end
 * ===================================================================== */

static void card_small(void)
{
    card_reset();
    /* deliberately out of order: reverse track numbers, artists jumbled */
    card_add("/Music/Boards of Canada/Geogaddi/02 Music Is Math.mp3", 8000u);
    card_add("/Music/Aphex Twin/Drukqs/02 Vordhosbn.flac", 2000u);
    card_add("/Music/Aphex Twin/Drukqs/01 Jynweythek.flac", 1000u);
    card_add("/Music/Boards of Canada/Geogaddi/01 Ready Lets Go.mp3", 7000u);
    card_add("/Music/Aphex Twin/SAW II/01 Untitled 1.wav", 5000u);
    card_add("/Music/Aphex Twin/Drukqs/03 Kladfvgbung.flac", 3000u);
}

TEST(a_small_card_builds_and_opens)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    CHECK_EQ(g_stats.tracks, 6u);
    CHECK_EQ(g_stats.artists, 2u);
    CHECK_EQ(g_stats.albums, 3u);
    CHECK_EQ(g_stats.skipped_path_too_long, 0u);

    CHECK_EQ(libidx_artist_count(&g_lib), 2u);
    CHECK_EQ(libidx_album_count(&g_lib), 3u);
    CHECK_EQ(libidx_track_count(&g_lib), 6u);
    CHECK_EQ(g_lib.hdr.build_id, 42u);

    libidx_close(&g_lib);
}

TEST(artists_come_out_sorted_by_name)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    /* discovery order was Boards of Canada first */
    CHECK(strcmp(libidx_artist_name(&g_lib, 0u), "Aphex Twin") == 0);
    CHECK(strcmp(libidx_artist_name(&g_lib, 1u), "Boards of Canada") == 0);

    libidx_close(&g_lib);
}

TEST(albums_are_contiguous_under_their_artist)
{
    libidx_scan_cfg_t cfg;
    uint32_t a, i;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    a = find_artist("Aphex Twin");
    CHECK_EQ(libidx_artist_album_count(&g_lib, a), 2u);
    CHECK(strcmp(libidx_album_name(&g_lib, libidx_artist_album(&g_lib, a, 0u)),
                 "Drukqs") == 0);
    CHECK(strcmp(libidx_album_name(&g_lib, libidx_artist_album(&g_lib, a, 1u)),
                 "SAW II") == 0);

    a = find_artist("Boards of Canada");
    CHECK_EQ(libidx_artist_album_count(&g_lib, a), 1u);

    /* every album's artist back-reference agrees with its position */
    for (i = 0; i < libidx_album_count(&g_lib); i++) {
        uint32_t owner = libidx_album_artist(&g_lib, i);
        uint32_t first = libidx_artist_album(&g_lib, owner, 0u);
        uint32_t n = libidx_artist_album_count(&g_lib, owner);
        CHECK(i >= first);
        CHECK(i < first + n);
    }

    libidx_close(&g_lib);
}

TEST(tracks_come_out_in_track_number_order)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;
    uint32_t a, alb;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    a = find_artist("Aphex Twin");
    alb = libidx_artist_album(&g_lib, a, 0u);      /* Drukqs */
    CHECK_EQ(libidx_album_track_count(&g_lib, alb), 3u);

    CHECK_EQ(libidx_album_track(&g_lib, alb, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Jynweythek") == 0);
    CHECK_EQ(t.track_no, 1u);
    CHECK_EQ(t.codec, LIB_CODEC_FLAC);
    CHECK_EQ(t.file_size, 1000u);
    CHECK(strcmp(t.path, "/Music/Aphex Twin/Drukqs/01 Jynweythek.flac") == 0);

    CHECK_EQ(libidx_album_track(&g_lib, alb, 1u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Vordhosbn") == 0);
    CHECK_EQ(t.track_no, 2u);

    CHECK_EQ(libidx_album_track(&g_lib, alb, 2u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Kladfvgbung") == 0);

    libidx_close(&g_lib);
}

TEST(every_track_points_back_at_its_own_album)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;
    uint32_t alb, n, i;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    for (alb = 0; alb < libidx_album_count(&g_lib); alb++) {
        n = libidx_album_track_count(&g_lib, alb);
        CHECK(n > 0u);
        for (i = 0; i < n; i++) {
            CHECK_EQ(libidx_album_track(&g_lib, alb, i, &t), LIB_OK);
            CHECK_EQ(t.album_idx, alb);
        }
    }

    libidx_close(&g_lib);
}

TEST(the_written_file_is_exactly_the_size_the_header_claims)
{
    libidx_scan_cfg_t cfg;
    vfile_t *f;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    f = vfs_find(LIB_INDEX_PATH);
    CHECK(f != NULL);
    CHECK_EQ(f->len, g_lib.hdr.total_size);

    libidx_close(&g_lib);
}

TEST(the_temp_file_is_removed_on_success)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_OK);
    CHECK(vfs_find(LIB_TEMP_PATH) == NULL);
}

TEST(two_builds_of_the_same_card_are_byte_identical)
{
    libidx_scan_cfg_t cfg;
    uint8_t *first;
    uint32_t first_len;
    vfile_t *f;

    env_reset();
    card_small();
    cfg_defaults(&cfg);

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_OK);
    f = vfs_find(LIB_INDEX_PATH);
    CHECK(f != NULL);
    first_len = f->len;
    first = (uint8_t *)malloc(first_len);
    memcpy(first, f->data, first_len);

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_OK);
    f = vfs_find(LIB_INDEX_PATH);
    CHECK_EQ(f->len, first_len);
    CHECK_EQ(memcmp(f->data, first, first_len), 0);

    free(first);
}

/* =====================================================================
 * grouping edge cases
 * ===================================================================== */

TEST(same_album_name_under_two_artists_stays_separate)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_reset();
    card_add("/Music/Artist A/Greatest Hits/01 One.mp3", 100u);
    card_add("/Music/Artist B/Greatest Hits/01 Two.mp3", 100u);
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_artist_count(&g_lib), 2u);
    CHECK_EQ(libidx_album_count(&g_lib), 2u);
    CHECK_EQ(libidx_artist_album_count(&g_lib, 0u), 1u);
    CHECK_EQ(libidx_artist_album_count(&g_lib, 1u), 1u);
    libidx_close(&g_lib);
}

TEST(folder_name_case_does_not_split_an_artist)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_reset();
    card_add("/Music/Aphex Twin/Drukqs/01 One.flac", 100u);
    card_add("/Music/aphex twin/Drukqs/02 Two.flac", 100u);
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_artist_count(&g_lib), 1u);
    CHECK_EQ(libidx_album_count(&g_lib), 1u);
    CHECK_EQ(libidx_album_track_count(&g_lib, 0u), 2u);
    libidx_close(&g_lib);
}

TEST(untagged_tracks_sort_alphabetically)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;

    env_reset();
    card_reset();
    card_add("/Music/A/B/Zebra.mp3", 100u);
    card_add("/Music/A/B/apple.mp3", 100u);
    card_add("/Music/A/B/Mango.mp3", 100u);
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_album_track_count(&g_lib, 0u), 3u);

    CHECK_EQ(libidx_album_track(&g_lib, 0u, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "apple") == 0);
    CHECK_EQ(libidx_album_track(&g_lib, 0u, 1u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Mango") == 0);
    CHECK_EQ(libidx_album_track(&g_lib, 0u, 2u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Zebra") == 0);

    libidx_close(&g_lib);
}

TEST(non_audio_files_are_skipped_and_counted)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_reset();
    card_add("/Music/A/B/01 Song.flac", 100u);
    card_add("/Music/A/B/cover.jpg", 100u);
    card_add("/Music/A/B/notes.txt", 100u);
    card_add("/System Volume Information/junk.mp3", 100u);
    card_add("/Music/A/B/.hidden.mp3", 100u);
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(g_stats.tracks, 1u);
    CHECK_EQ(g_stats.skipped_not_audio, 2u);
    CHECK_EQ(libidx_track_count(&g_lib), 1u);
    libidx_close(&g_lib);
}

TEST(an_empty_card_produces_a_valid_empty_index)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;

    env_reset();
    card_reset();
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_artist_count(&g_lib), 0u);
    CHECK_EQ(libidx_album_count(&g_lib), 0u);
    CHECK_EQ(libidx_track_count(&g_lib), 0u);
    CHECK_EQ(libidx_track_global(&g_lib, 0u, &t), LIB_E_RANGE);
    libidx_close(&g_lib);
}

TEST(scanning_a_subfolder_only_indexes_that_subfolder)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_reset();
    card_add("/Music/A/B/01 In.flac", 100u);
    card_add("/Other/C/D/01 Out.flac", 100u);
    cfg_defaults(&cfg);
    cfg.root = "/Music";

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_track_count(&g_lib), 1u);
    libidx_close(&g_lib);

    /* and a trailing slash on the root must behave the same */
    env_reset();
    cfg_defaults(&cfg);
    cfg.root = "/Music/";
    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_track_count(&g_lib), 1u);
    libidx_close(&g_lib);
}

/* =====================================================================
 * refusals
 * ===================================================================== */

static char g_long_path[512];

TEST(a_path_too_long_to_store_is_refused_not_truncated)
{
    libidx_scan_cfg_t cfg;
    uint32_t i, n;

    env_reset();
    card_reset();

    /* /Music/A/<200 chars>/01 Song.flac — longer than the 192-byte field */
    strcpy(g_long_path, "/Music/A/");
    n = (uint32_t)strlen(g_long_path);
    for (i = 0; i < 200u; i++) g_long_path[n + i] = 'd';
    strcpy(g_long_path + n + 200u, "/01 Song.flac");

    card_add(g_long_path, 100u);
    card_add("/Music/A/B/01 Fine.flac", 100u);
    cfg_defaults(&cfg);

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(libidx_track_count(&g_lib), 1u);
    CHECK(g_stats.skipped_path_too_long + g_stats.skipped_too_deep > 0u);
    libidx_close(&g_lib);
}

TEST(too_many_tracks_is_reported_not_silently_dropped)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    cfg.max_tracks = 3u;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_FULL);
    CHECK_EQ(g_stats.tracks, 3u);
}

TEST(a_full_string_pool_is_reported)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    cfg.pool_bytes = 16u;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_FULL);
}

TEST(too_many_artists_is_reported)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    cfg.max_artists = 1u;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_FULL);
}

TEST(an_undersized_arena_is_refused_before_any_io)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    cfg.arena_len = 64u;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_NOMEM);
    CHECK(vfs_find(LIB_TEMP_PATH) == NULL);   /* nothing was created */
}

TEST(arena_bytes_is_enough_and_is_checked)
{
    libidx_scan_cfg_t cfg;
    uint32_t need;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    need = libidx_scan_arena_bytes(&cfg);

    cfg.arena_len = need - 1u;
    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_NOMEM);

    cfg.arena_len = need;
    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_OK);
}

TEST(missing_callbacks_are_rejected)
{
    libidx_scan_cfg_t cfg;
    lib_io_t io_no_write;

    env_reset();
    card_small();
    cfg_defaults(&cfg);

    io_no_write = make_io();
    io_no_write.write = NULL;
    cfg.io = &io_no_write;
    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_ARG);

    cfg_defaults(&cfg);
    cfg.root = NULL;
    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_ARG);

    CHECK_EQ(libidx_scan(NULL, &g_stats), LIB_E_ARG);
}

TEST(a_write_failure_aborts_the_build)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    g_write_fail_after = 2;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_IO);
}

TEST(an_unreadable_root_is_an_error)
{
    libidx_scan_cfg_t cfg;

    env_reset();
    card_small();
    cfg_defaults(&cfg);
    g_opendir_fail = 1;

    CHECK_EQ(libidx_scan(&cfg, &g_stats), LIB_E_IO);
}

/* =====================================================================
 * tag hook
 * ===================================================================== */

static int fake_tags(void *ctx, const lib_io_t *io, const char *path,
                     uint32_t file_size, uint8_t codec, lib_tags_t *out)
{
    int *calls = (int *)ctx;
    (void)io;
    (void)file_size;
    (void)codec;

    (*calls)++;
    if (strstr(path, "Drukqs") != NULL) {
        strcpy(out->artist, "Tagged Artist");
        strcpy(out->album, "Tagged Album");
        strcpy(out->title, "Tagged Title");
        out->track_no = 9u;
        out->duration_ms = 123456u;
    }
    return 0;
}

TEST(the_tag_hook_overrides_path_derived_values)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;
    int calls = 0;
    uint32_t a, alb;

    env_reset();
    card_reset();
    card_add("/Music/Aphex Twin/Drukqs/01 Jynweythek.flac", 1000u);
    card_add("/Music/Aphex Twin/SAW II/01 Untitled 1.wav", 5000u);
    cfg_defaults(&cfg);
    cfg.tags = fake_tags;
    cfg.tags_ctx = &calls;

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK_EQ(calls, 2);

    a = find_artist("Tagged Artist");
    CHECK(a != 0xFFFFFFFFu);
    alb = libidx_artist_album(&g_lib, a, 0u);
    CHECK(strcmp(libidx_album_name(&g_lib, alb), "Tagged Album") == 0);
    CHECK_EQ(libidx_album_track(&g_lib, alb, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Tagged Title") == 0);
    CHECK_EQ(t.track_no, 9u);
    CHECK_EQ(t.duration_ms, 123456u);

    /* the untagged file kept its path-derived artist */
    CHECK(find_artist("Aphex Twin") != 0xFFFFFFFFu);

    libidx_close(&g_lib);
}

static int blanking_tags(void *ctx, const lib_io_t *io, const char *path,
                         uint32_t file_size, uint8_t codec, lib_tags_t *out)
{
    (void)ctx; (void)io; (void)path; (void)file_size; (void)codec;
    out->artist[0] = '\0';
    out->album[0] = '\0';
    out->title[0] = '\0';
    return 0;
}

TEST(a_tag_hook_returning_blanks_falls_back_to_unknown)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;

    env_reset();
    card_reset();
    card_add("/Music/A/B/01 Song.flac", 100u);
    cfg_defaults(&cfg);
    cfg.tags = blanking_tags;

    CHECK_EQ(build_and_open(&cfg), LIB_OK);
    CHECK(strcmp(libidx_artist_name(&g_lib, 0u), "Unknown Artist") == 0);
    CHECK(strcmp(libidx_album_name(&g_lib, 0u), "Unknown Album") == 0);
    CHECK_EQ(libidx_album_track(&g_lib, 0u, 0u, &t), LIB_OK);
    CHECK(strcmp(t.title, "Untitled") == 0);
    libidx_close(&g_lib);
}

/* =====================================================================
 * a realistic card
 * ===================================================================== */

static char g_paths[300][96];

TEST(a_three_hundred_track_card_indexes_correctly)
{
    libidx_scan_cfg_t cfg;
    lib_track_t t;
    uint32_t i, total, alb;
    int progress_calls = 0;

    env_reset();
    card_reset();

    /* 10 artists x 3 albums x 10 tracks, added in scrambled order */
    for (i = 0; i < 300u; i++) {
        uint32_t j = (i * 7u) % 300u;           /* scramble */
        uint32_t artist = j / 30u;
        uint32_t album = (j / 10u) % 3u;
        uint32_t track = (j % 10u) + 1u;
        sprintf(g_paths[i], "/Music/Artist %02u/Album %u/%02u Track %02u.flac",
                artist, album, track, track);
        card_add(g_paths[i], 1000u + j);
    }

    cfg_defaults(&cfg);
    cfg.progress_ctx = &progress_calls;

    CHECK_EQ(build_and_open(&cfg), LIB_OK);

    CHECK_EQ(libidx_artist_count(&g_lib), 10u);
    CHECK_EQ(libidx_album_count(&g_lib), 30u);
    CHECK_EQ(libidx_track_count(&g_lib), 300u);

    /* every artist has exactly 3 albums, every album exactly 10 tracks,
     * numbered 1..10 in order */
    total = 0;
    for (i = 0; i < 10u; i++) {
        CHECK_EQ(libidx_artist_album_count(&g_lib, i), 3u);
    }
    for (alb = 0; alb < 30u; alb++) {
        uint32_t n = libidx_album_track_count(&g_lib, alb);
        uint32_t k;
        CHECK_EQ(n, 10u);
        for (k = 0; k < n; k++) {
            CHECK_EQ(libidx_album_track(&g_lib, alb, k, &t), LIB_OK);
            CHECK_EQ(t.track_no, k + 1u);
            CHECK_EQ(t.album_idx, alb);
            CHECK_EQ(t.codec, LIB_CODEC_FLAC);
        }
        total += n;
    }
    CHECK_EQ(total, 300u);

    /* artist names are in order */
    for (i = 1; i < 10u; i++) {
        CHECK(strcmp(libidx_artist_name(&g_lib, i - 1u),
                     libidx_artist_name(&g_lib, i)) < 0);
    }

    libidx_close(&g_lib);
}

int main(void)
{
    printf("libidx_scan\n");

    RUN(codec_comes_from_the_extension);
    RUN(tags_come_from_the_folder_layout);
    RUN(track_number_prefixes_are_stripped);
    RUN(shallow_paths_fall_back_to_unknown);
    RUN(deep_paths_use_the_two_nearest_folders);

    RUN(a_small_card_builds_and_opens);
    RUN(artists_come_out_sorted_by_name);
    RUN(albums_are_contiguous_under_their_artist);
    RUN(tracks_come_out_in_track_number_order);
    RUN(every_track_points_back_at_its_own_album);
    RUN(the_written_file_is_exactly_the_size_the_header_claims);
    RUN(the_temp_file_is_removed_on_success);
    RUN(two_builds_of_the_same_card_are_byte_identical);

    RUN(same_album_name_under_two_artists_stays_separate);
    RUN(folder_name_case_does_not_split_an_artist);
    RUN(untagged_tracks_sort_alphabetically);
    RUN(non_audio_files_are_skipped_and_counted);
    RUN(an_empty_card_produces_a_valid_empty_index);
    RUN(scanning_a_subfolder_only_indexes_that_subfolder);

    RUN(a_path_too_long_to_store_is_refused_not_truncated);
    RUN(too_many_tracks_is_reported_not_silently_dropped);
    RUN(a_full_string_pool_is_reported);
    RUN(too_many_artists_is_reported);
    RUN(an_undersized_arena_is_refused_before_any_io);
    RUN(arena_bytes_is_enough_and_is_checked);
    RUN(missing_callbacks_are_rejected);
    RUN(a_write_failure_aborts_the_build);
    RUN(an_unreadable_root_is_an_error);

    RUN(the_tag_hook_overrides_path_derived_values);
    RUN(a_tag_hook_returning_blanks_falls_back_to_unknown);

    RUN(a_three_hundred_track_card_indexes_correctly);

    vfs_reset();
    return TEST_SUMMARY();
}
