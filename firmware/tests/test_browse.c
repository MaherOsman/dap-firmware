/* test_browse.c — navigation over a real index.
 *
 * The fixture is built by libidx_scan() over an in-RAM card and opened with
 * libidx_open(), so these tests exercise the whole stack rather than a
 * hand-made model of it. Page reads are counted: the windowing claim is that
 * scrolling a long album costs a bounded number of them, and an assertion is
 * the only thing that keeps that true.
 */

#include "test.h"

#include <stdlib.h>

#include "../src/core/browse.h"
#include "../src/core/library_build.h"
#include "../src/core/library_index.h"

/* ===================================================================== */
/* in-RAM card                                                            */
/* ===================================================================== */

#define CARD_MAX 600

static const char *g_card[CARD_MAX];
static uint32_t    g_ncard;

static void card_reset(void) { g_ncard = 0; }
static void card_add(const char *p) { if (g_ncard < CARD_MAX) g_card[g_ncard++] = p; }

#define VDIR_MAX_ENTS 512
#define VDIR_POOL     (LIB_BUILD_MAX_DEPTH + 2u)

typedef struct {
    lib_dirent_t ents[VDIR_MAX_ENTS];
    uint32_t n, pos;
    int in_use;
} vdir_t;

static vdir_t g_vdirs[VDIR_POOL];

static void vdir_push(vdir_t *d, const char *name, int is_dir)
{
    uint32_t i;
    for (i = 0; i < d->n; i++) {
        if (strcmp(d->ents[i].name, name) == 0) return;
    }
    if (d->n >= VDIR_MAX_ENTS) return;
    strncpy(d->ents[d->n].name, name, LIB_FILENAME_MAX - 1u);
    d->ents[d->n].name[LIB_FILENAME_MAX - 1u] = '\0';
    d->ents[d->n].is_dir = is_dir;
    d->ents[d->n].size = 1000u;
    d->n++;
}

static int v_opendir(void *ctx, const char *path, void **dh)
{
    vdir_t *d = NULL;
    uint32_t i, plen;
    int is_root;
    (void)ctx;

    for (i = 0; i < VDIR_POOL; i++) {
        if (!g_vdirs[i].in_use) { d = &g_vdirs[i]; break; }
    }
    if (!d) return -1;
    memset(d, 0, sizeof(*d));
    d->in_use = 1;

    is_root = (strcmp(path, "/") == 0);
    plen = is_root ? 0u : (uint32_t)strlen(path);

    for (i = 0; i < g_ncard; i++) {
        const char *p = g_card[i];
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
            char dn[LIB_FILENAME_MAX];
            size_t n = (size_t)(slash - rest);
            if (n >= LIB_FILENAME_MAX) n = LIB_FILENAME_MAX - 1u;
            memcpy(dn, rest, n);
            dn[n] = '\0';
            vdir_push(d, dn, 1);
        } else {
            vdir_push(d, rest, 0);
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
    (void)ctx;
    ((vdir_t *)dh)->in_use = 0;
    return 0;
}

/* ---- files ---- */

#define VF_MAX 4

typedef struct {
    char name[64];
    uint8_t *data;
    uint32_t len, cap;
    int in_use;
} vfile_t;

typedef struct { vfile_t *f; uint32_t pos; int in_use; } vhandle_t;

static vfile_t   g_vf[VF_MAX];
static vhandle_t g_vh[VF_MAX];
static int       g_reads;      /* every read of the index file */

static void vfs_reset(void)
{
    uint32_t i;
    for (i = 0; i < VF_MAX; i++) if (g_vf[i].data) free(g_vf[i].data);
    memset(g_vf, 0, sizeof(g_vf));
    memset(g_vh, 0, sizeof(g_vh));
    g_reads = 0;
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
            for (i = 0; i < VF_MAX; i++) if (!g_vf[i].in_use) { f = &g_vf[i]; break; }
            if (!f) return -1;
            memset(f, 0, sizeof(*f));
            strncpy(f->name, path, sizeof(f->name) - 1u);
            f->in_use = 1;
        }
        f->len = 0;
    } else if (!f) {
        return -1;
    }

    for (i = 0; i < VF_MAX; i++) if (!g_vh[i].in_use) { h = &g_vh[i]; break; }
    if (!h) return -1;
    h->f = f; h->pos = 0; h->in_use = 1;
    *fh = h;
    return 0;
}

static int v_read(void *ctx, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    vhandle_t *h = (vhandle_t *)fh;
    uint32_t n;
    (void)ctx;

    g_reads++;
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

    if (h->pos + len > h->f->cap) {
        uint32_t cap = h->f->cap ? h->f->cap : 1024u;
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
    (void)ctx;
    ((vhandle_t *)fh)->in_use = 0;
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

/* ===================================================================== */
/* harness                                                                */
/* ===================================================================== */

static lib_io_t   g_io;
static lib_dir_t  g_dir;
static libidx_t   g_idx;
static browse_t   g_br;
static uint8_t    g_scan_arena[256u * 1024u];
static uint8_t    g_read_arena[64u * 1024u];
static lib_row_t  g_rows[BROWSE_MAX_ROWS];

static int build_and_browse(void)
{
    libidx_scan_cfg_t cfg;
    int rc;

    vfs_reset();
    memset(g_vdirs, 0, sizeof(g_vdirs));

    memset(&g_io, 0, sizeof(g_io));
    g_io.open = v_open; g_io.read = v_read; g_io.write = v_write;
    g_io.seek = v_seek; g_io.close = v_close; g_io.unlink = v_unlink;

    memset(&g_dir, 0, sizeof(g_dir));
    g_dir.opendir = v_opendir; g_dir.readdir = v_readdir;
    g_dir.closedir = v_closedir;

    memset(&cfg, 0, sizeof(cfg));
    cfg.io = &g_io; cfg.dir = &g_dir; cfg.root = "/";
    cfg.build_id = 1u;
    cfg.max_tracks = 4000u; cfg.max_artists = 512u;
    cfg.max_albums = 2048u; cfg.pool_bytes = 48u * 1024u;
    cfg.arena = g_scan_arena; cfg.arena_len = (uint32_t)sizeof(g_scan_arena);

    rc = libidx_scan(&cfg, NULL);
    if (rc != LIB_OK) return rc;

    rc = libidx_open(&g_idx, &g_io, LIB_INDEX_PATH, g_read_arena,
                     (uint32_t)sizeof(g_read_arena));
    if (rc != LIB_OK) return rc;

    browse_init(&g_br, &g_idx);
    return LIB_OK;
}

/* Two artists, three albums, a handful of tracks each. */
static void card_small(void)
{
    card_reset();
    card_add("/Music/Aphex Twin/Drukqs/01 Jynweythek.flac");
    card_add("/Music/Aphex Twin/Drukqs/02 Vordhosbn.flac");
    card_add("/Music/Aphex Twin/Drukqs/03 Kladfvgbung.flac");
    card_add("/Music/Aphex Twin/SAW II/01 Untitled 1.wav");
    card_add("/Music/Aphex Twin/SAW II/02 Untitled 2.wav");
    card_add("/Music/Boards of Canada/Geogaddi/01 Ready Lets Go.mp3");
    card_add("/Music/Boards of Canada/Geogaddi/02 Music Is Math.mp3");
}

/* ===================================================================== */

TEST(starts_at_the_artist_list)
{
    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_ARTIST);
    CHECK_EQ(browse_row_count(&g_br), 2);
    CHECK_EQ(browse_selected(&g_br), 0);
    CHECK_EQ(browse_scroll_top(&g_br), 0);
    CHECK(strcmp(browse_header(&g_br), "Artists") == 0);

    libidx_close(&g_idx);
}

TEST(rows_show_the_artists)
{
    int n;

    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, 2);
    CHECK(strcmp(g_rows[0].text, "Aphex Twin") == 0);
    CHECK(strcmp(g_rows[1].text, "Boards of Canada") == 0);
    CHECK(g_rows[0].has_sub);
    CHECK(g_rows[1].has_sub);
    CHECK(!g_rows[0].is_current);

    libidx_close(&g_idx);
}

TEST(drilling_down_and_backing_out)
{
    int n;

    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_ALBUM);
    CHECK_EQ(browse_row_count(&g_br), 2);
    CHECK(strcmp(browse_header(&g_br), "Aphex Twin") == 0);

    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, 2);
    CHECK(strcmp(g_rows[0].text, "Drukqs") == 0);
    CHECK(strcmp(g_rows[1].text, "SAW II") == 0);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_TRACK);
    CHECK_EQ(browse_row_count(&g_br), 3);
    CHECK(strcmp(browse_header(&g_br), "Drukqs") == 0);

    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, 3);
    CHECK(strcmp(g_rows[0].text, "Jynweythek") == 0);
    CHECK(!g_rows[0].has_sub);      /* a track is a leaf */

    CHECK(browse_back(&g_br));
    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_ALBUM);
    CHECK(browse_back(&g_br));
    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_ARTIST);
    CHECK(!browse_back(&g_br));     /* nowhere further up */

    libidx_close(&g_idx);
}

TEST(selection_is_remembered_per_level)
{
    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    browse_move(&g_br, 1);                       /* Boards of Canada */
    CHECK_EQ(browse_selected(&g_br), 1);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_selected(&g_br), 0);         /* fresh album list */
    CHECK(strcmp(browse_header(&g_br), "Boards of Canada") == 0);

    CHECK(browse_back(&g_br));
    /* backing out returns to where we were, not to the top */
    CHECK_EQ(browse_selected(&g_br), 1);

    libidx_close(&g_idx);
}

TEST(activating_a_track_returns_it_without_playing_it)
{
    lib_track_t t;
    uint32_t gi = 999u;

    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    browse_move(&g_br, 1);

    CHECK_EQ(browse_activate(&g_br, &t, &gi), BROWSE_PLAY);
    CHECK(strcmp(t.title, "Vordhosbn") == 0);
    CHECK(strcmp(t.path, "/Music/Aphex Twin/Drukqs/02 Vordhosbn.flac") == 0);
    CHECK_EQ(t.track_no, 2u);
    CHECK_EQ(t.codec, LIB_CODEC_FLAC);
    CHECK_EQ(gi, 1u);

    /* level did not change — activating a track is a decision, not a move */
    CHECK_EQ(browse_level(&g_br), LIB_LEVEL_TRACK);

    libidx_close(&g_idx);
}

TEST(the_play_marker_propagates_up_the_levels)
{
    uint32_t playing;
    lib_track_t t;
    int n;

    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    /* play Geogaddi track 2, which is Boards of Canada */
    browse_move(&g_br, 1);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    browse_move(&g_br, 1);
    CHECK_EQ(browse_activate(&g_br, &t, &playing), BROWSE_PLAY);
    CHECK(strcmp(t.title, "Music Is Math") == 0);

    /* at track level only that row is marked */
    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS, playing);
    CHECK_EQ(n, 2);
    CHECK(!g_rows[0].is_current);
    CHECK(g_rows[1].is_current);

    /* at album level the containing album is marked */
    CHECK(browse_back(&g_br));
    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS, playing);
    CHECK_EQ(n, 1);
    CHECK(g_rows[0].is_current);

    /* at artist level the owning artist is marked, and only that one */
    CHECK(browse_back(&g_br));
    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS, playing);
    CHECK_EQ(n, 2);
    CHECK(!g_rows[0].is_current);
    CHECK(g_rows[1].is_current);

    libidx_close(&g_idx);
}

TEST(album_of_track_finds_the_right_album)
{
    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(libidx_album_of_track(&g_idx, 0u), 0u);
    CHECK_EQ(libidx_album_of_track(&g_idx, 2u), 0u);
    CHECK_EQ(libidx_album_of_track(&g_idx, 3u), 1u);
    CHECK_EQ(libidx_album_of_track(&g_idx, 4u), 1u);
    CHECK_EQ(libidx_album_of_track(&g_idx, 5u), 2u);
    CHECK_EQ(libidx_album_of_track(&g_idx, 99u), LIBIDX_NONE);
    CHECK_EQ(libidx_album_of_track(NULL, 0u), LIBIDX_NONE);

    libidx_close(&g_idx);
}

TEST(moving_clamps_at_both_ends)
{
    card_small();
    CHECK_EQ(build_and_browse(), LIB_OK);

    browse_move(&g_br, -5);
    CHECK_EQ(browse_selected(&g_br), 0);
    browse_move(&g_br, 99);
    CHECK_EQ(browse_selected(&g_br), 1);   /* 2 artists */
    browse_move(&g_br, 1);
    CHECK_EQ(browse_selected(&g_br), 1);

    libidx_close(&g_idx);
}

TEST(an_empty_library_is_safe_to_browse)
{
    lib_track_t t;

    card_reset();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_row_count(&g_br), 0);
    CHECK_EQ(browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                              BROWSE_NOTHING_PLAYING), 0);
    CHECK_EQ(browse_activate(&g_br, &t, NULL), BROWSE_NONE);
    CHECK(!browse_back(&g_br));
    browse_move(&g_br, 3);
    CHECK_EQ(browse_selected(&g_br), 0);

    libidx_close(&g_idx);
}

TEST(a_null_index_is_safe_to_browse)
{
    browse_t br;

    browse_init(&br, NULL);
    CHECK_EQ(browse_row_count(&br), 0);
    CHECK_EQ(browse_fill_rows(&br, g_rows, BROWSE_MAX_ROWS,
                              BROWSE_NOTHING_PLAYING), 0);
    CHECK_EQ(browse_activate(&br, NULL, NULL), BROWSE_NONE);
    CHECK(browse_header(&br) != NULL);
    browse_move(&br, 1);
    CHECK_EQ(browse_selected(&br), 0);
}

/* ===================================================================== */
/* the windowing claim                                                    */
/* ===================================================================== */

static char g_paths[400][96];

static void card_long_album(void)
{
    uint32_t i;
    card_reset();
    for (i = 0; i < 400u; i++) {
        sprintf(g_paths[i], "/Music/One Artist/Long Album/%03u Track %03u.flac",
                i + 1u, i + 1u);
        card_add(g_paths[i]);
    }
}

TEST(a_long_album_fills_only_the_visible_window)
{
    int n;

    card_long_album();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_row_count(&g_br), 400);

    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    /* 400 tracks, but only a screenful is ever built */
    CHECK(n <= LIB_VISIBLE);
    CHECK(n > 0);
    CHECK(strcmp(g_rows[0].text, "Track 001") == 0);

    libidx_close(&g_idx);
}

TEST(scrolling_a_long_album_stays_cheap)
{
    int i, reads_before, reads_after;

    card_long_album();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);

    /* one full screen redraw spans at most two pages */
    (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                           BROWSE_NOTHING_PLAYING);
    reads_before = g_reads;
    (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                           BROWSE_NOTHING_PLAYING);
    reads_after = g_reads;
    CHECK_EQ(reads_after - reads_before, 0);   /* same window, cache hit */

    /* stepping down one row at a time through all 400 tracks: with 8-track
     * pages this must stay near 400/8, not 400 */
    reads_before = g_reads;
    for (i = 0; i < 399; i++) {
        browse_move(&g_br, 1);
        (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                               BROWSE_NOTHING_PLAYING);
    }
    reads_after = g_reads;
    CHECK(reads_after - reads_before < 200);

    CHECK_EQ(browse_selected(&g_br), 399);

    libidx_close(&g_idx);
}

TEST(the_window_follows_the_selection)
{
    int n, top;

    card_long_album();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);

    browse_move(&g_br, 200);
    top = browse_scroll_top(&g_br);
    CHECK(top > 0);
    CHECK(browse_selected(&g_br) >= top);
    CHECK(browse_selected(&g_br) < top + LIB_VISIBLE);

    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    CHECK(n > 0);
    /* rows[0] is the row at scroll_top, which is what the windowed draw
     * call expects */
    {
        char expect[32];
        sprintf(expect, "Track %03d", top + 1);
        CHECK(strcmp(g_rows[0].text, expect) == 0);
    }

    /* scrolled to the very bottom, no blank rows */
    browse_move(&g_br, 999);
    top = browse_scroll_top(&g_br);
    CHECK_EQ(top, 400 - LIB_VISIBLE);
    n = browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                         BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, LIB_VISIBLE);

    libidx_close(&g_idx);
}

TEST(row_text_survives_scrolling)
{
    char first[BROWSE_TEXT_MAX];

    card_long_album();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);

    (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                           BROWSE_NOTHING_PLAYING);
    strcpy(first, g_rows[0].text);

    /* Reading a long way away evicts the page the first row came from. If
     * rows borrowed their text instead of owning it, this is where the
     * display would start showing someone else's track. */
    browse_move(&g_br, 300);
    (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                           BROWSE_NOTHING_PLAYING);
    browse_move(&g_br, -300);
    (void)browse_fill_rows(&g_br, g_rows, BROWSE_MAX_ROWS,
                           BROWSE_NOTHING_PLAYING);

    CHECK(strcmp(g_rows[0].text, first) == 0);
    CHECK(strcmp(g_rows[0].text, "Track 001") == 0);

    libidx_close(&g_idx);
}

TEST(fill_rows_respects_a_small_max)
{
    int n;

    card_long_album();
    CHECK_EQ(build_and_browse(), LIB_OK);

    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);
    CHECK_EQ(browse_activate(&g_br, NULL, NULL), BROWSE_DESCENDED);

    n = browse_fill_rows(&g_br, g_rows, 3, BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, 3);
    n = browse_fill_rows(&g_br, g_rows, 0, BROWSE_NOTHING_PLAYING);
    CHECK_EQ(n, 0);

    libidx_close(&g_idx);
}

int main(void)
{
    printf("browse\n");

    RUN(starts_at_the_artist_list);
    RUN(rows_show_the_artists);
    RUN(drilling_down_and_backing_out);
    RUN(selection_is_remembered_per_level);
    RUN(activating_a_track_returns_it_without_playing_it);
    RUN(the_play_marker_propagates_up_the_levels);
    RUN(album_of_track_finds_the_right_album);
    RUN(moving_clamps_at_both_ends);
    RUN(an_empty_library_is_safe_to_browse);
    RUN(a_null_index_is_safe_to_browse);

    RUN(a_long_album_fills_only_the_visible_window);
    RUN(scrolling_a_long_album_stays_cheap);
    RUN(the_window_follows_the_selection);
    RUN(row_text_survives_scrolling);
    RUN(fill_rows_respects_a_small_max);

    vfs_reset();
    return TEST_SUMMARY();
}
