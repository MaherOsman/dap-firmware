/* test_playqueue.c — playback policy.
 *
 * Uses a hand-built index rather than a scan: the queue only ever asks the
 * index about album spans, so a two-album fixture is enough and the test
 * stays independent of the scanner.
 */

#include "test.h"

#include <stdlib.h>

#include "../src/core/playqueue.h"

/* ---- minimal in-RAM index file ---- */

typedef struct { uint8_t *d; uint32_t len, pos; } ramf_t;
static ramf_t g_f;

static int r_open(void *c, const char *p, int m, void **fh)
{ (void)c; (void)p; (void)m; g_f.pos = 0; *fh = &g_f; return 0; }
static int r_read(void *c, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    ramf_t *f = (ramf_t *)fh; uint32_t n;
    (void)c;
    if (f->pos >= f->len) { *got = 0; return 0; }
    n = f->len - f->pos; if (n > len) n = len;
    memcpy(dst, f->d + f->pos, n); f->pos += n; *got = n; return 0;
}
static int r_seek(void *c, void *fh, uint32_t off)
{ ramf_t *f = (ramf_t *)fh; (void)c; if (off > f->len) return -1; f->pos = off; return 0; }
static int r_close(void *c, void *fh) { (void)c; (void)fh; return 0; }

/* Two albums under one artist: album 0 has 3 tracks, album 1 has 2. */
static void build_index(void)
{
    const char *pool = "The Artist\0Album One\0Album Two\0";
    uint32_t pool_len = 32u;
    uint32_t artist_off = LIB_HDR_SIZE;
    uint32_t album_off = artist_off + LIB_ARTIST_REC_SIZE;
    uint32_t strpool_off = album_off + 2u * LIB_ALBUM_REC_SIZE;
    uint32_t track_off = strpool_off + pool_len;
    uint32_t total = track_off + 5u * LIB_TRACK_REC_SIZE;
    uint8_t *m = (uint8_t *)calloc(1, total);
    uint32_t i;

    memcpy(m, LIB_IDX_MAGIC, LIB_IDX_MAGIC_LEN);
    lib_wr_u32(m + 8, LIB_IDX_VERSION);
    lib_wr_u32(m + 12, 1u);              /* artists */
    lib_wr_u32(m + 16, 2u);              /* albums  */
    lib_wr_u32(m + 20, 5u);              /* tracks  */
    lib_wr_u32(m + 24, artist_off);
    lib_wr_u32(m + 28, album_off);
    lib_wr_u32(m + 32, strpool_off);
    lib_wr_u32(m + 36, pool_len);
    lib_wr_u32(m + 40, track_off);
    lib_wr_u32(m + 44, LIB_TRACK_REC_SIZE);
    lib_wr_u32(m + 48, 1u);
    lib_wr_u32(m + 52, total);

    lib_wr_u32(m + artist_off + 0, 0u);   /* name_off    */
    lib_wr_u32(m + artist_off + 4, 0u);   /* album_first */
    lib_wr_u32(m + artist_off + 8, 2u);   /* album_count */

    lib_wr_u32(m + album_off + 0, 11u);   /* "Album One" */
    lib_wr_u32(m + album_off + 4, 0u);
    lib_wr_u32(m + album_off + 8, 0u);    /* track_first */
    lib_wr_u32(m + album_off + 12, 3u);   /* track_count */

    lib_wr_u32(m + album_off + LIB_ALBUM_REC_SIZE + 0, 21u);  /* "Album Two" */
    lib_wr_u32(m + album_off + LIB_ALBUM_REC_SIZE + 4, 0u);
    lib_wr_u32(m + album_off + LIB_ALBUM_REC_SIZE + 8, 3u);
    lib_wr_u32(m + album_off + LIB_ALBUM_REC_SIZE + 12, 2u);

    memcpy(m + strpool_off, pool, 31u);

    for (i = 0; i < 5u; i++) {
        uint8_t *r = m + track_off + i * LIB_TRACK_REC_SIZE;
        lib_wr_u32(r + 0, (i < 3u) ? 0u : 1u);
        lib_wr_u16(r + 12, (uint16_t)((i < 3u) ? i + 1u : i - 2u));
        r[14] = LIB_CODEC_FLAC;
        r[32] = (uint8_t)('A' + i);
    }

    g_f.d = m;
    g_f.len = total;
}

static lib_io_t   g_io;
static libidx_t   g_idx;
static playqueue_t g_pq;
static uint8_t    g_arena[32u * 1024u];

static void setup(void)
{
    if (g_f.d) { free(g_f.d); g_f.d = NULL; }
    build_index();
    memset(&g_io, 0, sizeof(g_io));
    g_io.open = r_open; g_io.read = r_read; g_io.seek = r_seek; g_io.close = r_close;
    CHECK_EQ(libidx_open(&g_idx, &g_io, "/dap.idx", g_arena, sizeof(g_arena)), LIB_OK);
    pq_init(&g_pq, &g_idx);
}

static void teardown(void) { libidx_close(&g_idx); }

/* ===================================================================== */

TEST(starts_stopped)
{
    setup();
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    CHECK_EQ(pq_current(&g_pq), PQ_NO_TRACK);
    CHECK(!pq_is_active(&g_pq));
    CHECK_EQ(pq_repeat(&g_pq), PQ_REPEAT_OFF);
    teardown();
}

TEST(starting_a_track_makes_it_current)
{
    setup();
    CHECK(pq_start(&g_pq, 1u));
    CHECK_EQ(pq_current(&g_pq), 1u);
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);
    CHECK(pq_is_active(&g_pq));
    teardown();
}

TEST(an_out_of_range_track_is_refused_and_changes_nothing)
{
    setup();
    CHECK(pq_start(&g_pq, 2u));
    CHECK(!pq_start(&g_pq, 99u));
    CHECK_EQ(pq_current(&g_pq), 2u);      /* unchanged */
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);
    teardown();
}

TEST(pause_toggles_without_losing_the_track)
{
    setup();
    CHECK(pq_start(&g_pq, 1u));

    CHECK(pq_toggle_pause(&g_pq));
    CHECK_EQ(pq_state(&g_pq), PQ_PAUSED);
    CHECK_EQ(pq_current(&g_pq), 1u);
    CHECK(pq_is_active(&g_pq));           /* paused is still active */

    CHECK(!pq_toggle_pause(&g_pq));
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);
    CHECK_EQ(pq_current(&g_pq), 1u);
    teardown();
}

TEST(pausing_while_stopped_does_nothing)
{
    setup();
    CHECK(!pq_toggle_pause(&g_pq));
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    teardown();
}

TEST(a_finished_track_advances_within_the_album)
{
    uint32_t nxt = 99u;

    setup();
    CHECK(pq_start(&g_pq, 0u));

    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 1u);
    CHECK_EQ(pq_current(&g_pq), 1u);
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);

    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 2u);
    teardown();
}

TEST(the_album_ends_rather_than_running_into_the_next_one)
{
    uint32_t nxt = 99u;

    setup();
    CHECK(pq_start(&g_pq, 2u));           /* last track of album 0 */

    CHECK(!pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    CHECK_EQ(pq_current(&g_pq), PQ_NO_TRACK);
    CHECK_EQ(nxt, 99u);                   /* left alone on failure */
    teardown();
}

TEST(the_second_album_plays_through_on_its_own_bounds)
{
    uint32_t nxt = 0;

    setup();
    CHECK(pq_start(&g_pq, 3u));           /* first track of album 1 */
    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 4u);
    CHECK(!pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    teardown();
}

TEST(repeat_one_replays_the_same_track_only_when_it_ends)
{
    uint32_t nxt = 0;

    setup();
    pq_set_repeat(&g_pq, PQ_REPEAT_ONE);
    CHECK(pq_start(&g_pq, 1u));

    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 1u);
    CHECK_EQ(pq_current(&g_pq), 1u);

    /* a deliberate skip still moves on */
    CHECK(pq_next(&g_pq, &nxt));
    CHECK_EQ(nxt, 2u);
    teardown();
}

TEST(repeat_all_wraps_to_the_start_of_the_album)
{
    uint32_t nxt = 0;

    setup();
    pq_set_repeat(&g_pq, PQ_REPEAT_ALL);
    CHECK(pq_start(&g_pq, 2u));           /* last of album 0 */

    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 0u);
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);

    /* album 1 wraps to its own first track, not to track 0 */
    CHECK(pq_start(&g_pq, 4u));
    CHECK(pq_track_finished(&g_pq, &nxt));
    CHECK_EQ(nxt, 3u);
    teardown();
}

TEST(skipping_back_stays_inside_the_album)
{
    uint32_t p = 0;

    setup();
    CHECK(pq_start(&g_pq, 2u));
    CHECK(pq_prev(&g_pq, &p));
    CHECK_EQ(p, 1u);
    CHECK(pq_prev(&g_pq, &p));
    CHECK_EQ(p, 0u);

    /* at the first track, restart rather than stop */
    CHECK(pq_prev(&g_pq, &p));
    CHECK_EQ(p, 0u);
    CHECK_EQ(pq_state(&g_pq), PQ_PLAYING);

    /* and never falls off the front of album 1 into album 0 */
    CHECK(pq_start(&g_pq, 3u));
    CHECK(pq_prev(&g_pq, &p));
    CHECK_EQ(p, 3u);
    teardown();
}

TEST(skipping_forward_off_the_end_stops)
{
    uint32_t n = 0;

    setup();
    CHECK(pq_start(&g_pq, 2u));
    CHECK(!pq_next(&g_pq, &n));
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    teardown();
}

TEST(advancing_while_stopped_is_safe)
{
    uint32_t n = 0;

    setup();
    CHECK(!pq_next(&g_pq, &n));
    CHECK(!pq_prev(&g_pq, &n));
    CHECK(!pq_track_finished(&g_pq, &n));
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    teardown();
}

TEST(stop_clears_everything)
{
    setup();
    CHECK(pq_start(&g_pq, 1u));
    pq_stop(&g_pq);
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    CHECK_EQ(pq_current(&g_pq), PQ_NO_TRACK);
    CHECK(!pq_is_active(&g_pq));
    teardown();
}

TEST(a_null_index_is_safe)
{
    playqueue_t pq;
    uint32_t n = 0;

    pq_init(&pq, NULL);
    CHECK(!pq_start(&pq, 0u));
    CHECK(!pq_next(&pq, &n));
    CHECK(!pq_prev(&pq, &n));
    CHECK(!pq_track_finished(&pq, &n));
    CHECK(!pq_toggle_pause(&pq));
    CHECK_EQ(pq_current(&pq), PQ_NO_TRACK);
    pq_stop(&pq);
}

TEST(null_out_pointers_are_allowed)
{
    setup();
    CHECK(pq_start(&g_pq, 0u));
    CHECK(pq_next(&g_pq, NULL));
    CHECK_EQ(pq_current(&g_pq), 1u);
    CHECK(pq_prev(&g_pq, NULL));
    CHECK_EQ(pq_current(&g_pq), 0u);
    CHECK(pq_track_finished(&g_pq, NULL));
    CHECK_EQ(pq_current(&g_pq), 1u);
    teardown();
}

TEST(playing_an_album_end_to_end)
{
    uint32_t n;
    int plays = 0;

    setup();
    CHECK(pq_start(&g_pq, 0u));
    plays++;
    while (pq_track_finished(&g_pq, &n)) {
        plays++;
        CHECK(plays < 10);        /* an infinite loop here is the real risk */
    }
    CHECK_EQ(plays, 3);
    CHECK_EQ(pq_state(&g_pq), PQ_STOPPED);
    teardown();
}

int main(void)
{
    printf("playqueue\n");

    RUN(starts_stopped);
    RUN(starting_a_track_makes_it_current);
    RUN(an_out_of_range_track_is_refused_and_changes_nothing);
    RUN(pause_toggles_without_losing_the_track);
    RUN(pausing_while_stopped_does_nothing);
    RUN(a_finished_track_advances_within_the_album);
    RUN(the_album_ends_rather_than_running_into_the_next_one);
    RUN(the_second_album_plays_through_on_its_own_bounds);
    RUN(repeat_one_replays_the_same_track_only_when_it_ends);
    RUN(repeat_all_wraps_to_the_start_of_the_album);
    RUN(skipping_back_stays_inside_the_album);
    RUN(skipping_forward_off_the_end_stops);
    RUN(advancing_while_stopped_is_safe);
    RUN(stop_clears_everything);
    RUN(a_null_index_is_safe);
    RUN(null_out_pointers_are_allowed);
    RUN(playing_an_album_end_to_end);

    if (g_f.d) free(g_f.d);
    return TEST_SUMMARY();
}
