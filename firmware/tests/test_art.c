/* test_art — JPEG cover decoding, fed from memory. */

#include "test.h"

#include <stdlib.h>

#include "../src/core/art.h"
#include "art_fixtures.h"

#define SENTINEL 0xDEADu

static art_work_t g_work;
static uint16_t   g_out[ART_MAX_SIZE * ART_MAX_SIZE];

/* A byte source over a buffer, optionally cut short. */
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    int yields;
} mem_t;

static size_t mem_read(void *ctx, uint8_t *buf, size_t len)
{
    mem_t *m = (mem_t *)ctx;
    size_t left = m->len - m->pos;
    if (len > left) len = left;
    if (buf != NULL) memcpy(buf, m->data + m->pos, len);
    m->pos += len;
    return len;
}

static void mem_yield(void *ctx) { ((mem_t *)ctx)->yields++; }
static bool mem_rewind(void *ctx) { ((mem_t *)ctx)->pos = 0; return true; }

static art_result_t decode(const uint8_t *data, size_t len, int size,
                           art_info_t *info, int *yields)
{
    mem_t m = { data, len, 0, 0 };
    art_src_t src = { mem_read, mem_yield, &m, NULL };
    art_result_t r;
    int i;

    for (i = 0; i < ART_MAX_SIZE * ART_MAX_SIZE; i++) g_out[i] = SENTINEL;
    r = art_decode_jpeg(&src, g_out, size, &g_work, info);
    if (yields != NULL) *yields = m.yields;
    return r;
}

#define DECODE(fix, size, info) decode((fix), sizeof(fix), (size), (info), NULL)

/* Is output pixel (x, y) within `tol` of an 8-bit colour, per channel? */
static int near(int size, int x, int y, int r, int g, int b, int tol)
{
    uint16_t p = g_out[y * size + x];
    int pr = ((p >> 11) & 0x1F) * 255 / 31;
    int pg = ((p >> 5) & 0x3F) * 255 / 63;
    int pb = (p & 0x1F) * 255 / 31;
    return abs(pr - r) <= tol && abs(pg - g) <= tol && abs(pb - b) <= tol;
}

static int untouched(int size)
{
    int i, n = 0;
    for (i = 0; i < size * size; i++) n += (g_out[i] == SENTINEL);
    return n;
}

static void check_quadrants(int size)
{
    int q = size / 4;
    CHECK(near(size, q,     q,     220, 30, 30, 24));    /* red */
    CHECK(near(size, 3 * q, q,     30, 200, 60, 24));    /* green */
    CHECK(near(size, q,     3 * q, 40, 60, 210, 24));    /* blue */
    CHECK(near(size, 3 * q, 3 * q, 240, 240, 240, 24));  /* white */
    CHECK_EQ(untouched(size), 0);
}

/* ------------------------------------------------------------------ */

TEST(a_cover_shrinks_to_both_art_sizes)
{
    art_info_t info;

    CHECK_EQ(DECODE(FIX_QUAD400, 176, &info), ART_OK);
    CHECK_EQ(info.src_w, 400);
    CHECK_EQ(info.src_h, 400);
    CHECK_EQ(info.scale, 1);          /* 400/2 = 200 is still >= 176 */
    check_quadrants(176);

    CHECK_EQ(DECODE(FIX_QUAD400, 216, &info), ART_OK);
    CHECK_EQ(info.scale, 0);          /* 400/2 = 200 would be too small */
    check_quadrants(216);
}

TEST(big_covers_use_the_deeper_descales)
{
    art_info_t info;

    /* A 400 px cover at 96 px behaves like a 1600 px one at 384: it takes
     * the 1/4 path. At 40 px, the 1/8 path — DC-only decoding. */
    CHECK_EQ(DECODE(FIX_QUAD400, 96, &info), ART_OK);
    CHECK_EQ(info.scale, 2);
    check_quadrants(96);

    CHECK_EQ(DECODE(FIX_QUAD400, 40, &info), ART_OK);
    CHECK_EQ(info.scale, 3);
    check_quadrants(40);
}

TEST(the_quadrant_edges_stay_where_they_belong)
{
    /* Averaging must not smear one quadrant into the next beyond a pixel
     * or so either side of the seam. */
    int s = 216, y;
    CHECK_EQ(DECODE(FIX_QUAD400, s, NULL), ART_OK);
    for (y = 10; y < s / 2 - 10; y += 7) {
        CHECK(near(s, s / 2 - 4, y, 220, 30, 30, 32));
        CHECK(near(s, s / 2 + 3, y, 30, 200, 60, 32));
    }
}

TEST(a_small_cover_is_enlarged_with_every_pixel_written)
{
    art_info_t info;
    CHECK_EQ(DECODE(FIX_QUAD64, 176, &info), ART_OK);
    CHECK_EQ(info.scale, 0);
    check_quadrants(176);

    CHECK_EQ(DECODE(FIX_QUAD64, 216, NULL), ART_OK);
    check_quadrants(216);
}

TEST(a_wide_cover_is_cropped_to_its_centre)
{
    /* 600x300, thirds at 200 and 400. The centre square is x 150..449: a
     * sliver of red, all of the green, a sliver of blue. */
    int s = 176;
    CHECK_EQ(DECODE(FIX_WIDE, s, NULL), ART_OK);
    CHECK(near(s, s / 2, s / 2, 30, 200, 60, 24));
    CHECK(near(s, 4, s / 2, 220, 30, 30, 32));
    CHECK(near(s, s - 5, s / 2, 40, 60, 210, 32));
    CHECK_EQ(untouched(s), 0);
}

TEST(greyscale_covers_decode)
{
    int s = 176;
    CHECK_EQ(DECODE(FIX_GRAY, s, NULL), ART_OK);
    CHECK(near(s, s / 4, s / 2, 40, 40, 40, 16));
    CHECK(near(s, 3 * s / 4, s / 2, 200, 200, 200, 16));
    CHECK_EQ(untouched(s), 0);
}

TEST(progressive_jpegs_are_turned_away_by_the_baseline_path)
{
    CHECK_EQ(DECODE(FIX_PROG, 176, NULL), ART_ERR_UNSUPPORTED);
}

/* ---------------------------------------------------- progressive */

static art_result_t decode_prog(const uint8_t *data, size_t len, int size,
                                art_info_t *info, size_t *used)
{
    mem_t m = { data, len, 0, 0 };
    art_src_t src = { mem_read, mem_yield, &m, mem_rewind };
    art_result_t r;
    int i;

    for (i = 0; i < ART_MAX_SIZE * ART_MAX_SIZE; i++) g_out[i] = SENTINEL;
    r = art_decode_jpeg_progressive(&src, g_out, size, &g_work, info);
    if (used != NULL) *used = m.pos;
    return r;
}

#define DECODE_PROG(fix, size, info) \
    decode_prog((fix), sizeof(fix), (size), (info), NULL)

TEST(a_small_progressive_cover_decodes_at_full_detail)
{
    art_info_t info;
    /* 200 px at 176: every coefficient is needed, full 8x8 blocks. */
    CHECK_EQ(DECODE_PROG(FIX_PROG, 176, &info), ART_OK);
    CHECK_EQ(info.src_w, 200);
    CHECK_EQ(info.scale, 0);
    check_quadrants(176);
    CHECK_EQ(DECODE_PROG(FIX_PROG, 216, NULL), ART_OK);
    check_quadrants(216);
}

TEST(a_big_progressive_cover_shrinks_and_needs_only_its_first_pass)
{
    /* The fixture stops right after the first scan. At 176 px a 1600 px
     * cover needs only that (1/8 is already sharp) — decoding succeeds,
     * proof nothing past it is read. At 216 px the next passes would add
     * detail; the file ending early still gives the whole picture. */
    size_t used = 0;
    CHECK_EQ(decode_prog(FIX_PROG1600, sizeof(FIX_PROG1600), 176, NULL, &used),
             ART_OK);
    check_quadrants(176);
    CHECK(used <= sizeof(FIX_PROG1600));
    CHECK_EQ(DECODE_PROG(FIX_PROG1600, 216, NULL), ART_OK);
    check_quadrants(216);
}

TEST(progressive_greyscale_and_444_and_restarts_decode)
{
    int s = 176;
    CHECK_EQ(DECODE_PROG(FIX_PROGGRAY, s, NULL), ART_OK);
    CHECK(near(s, s / 4, s / 2, 40, 40, 40, 16));
    CHECK(near(s, 3 * s / 4, s / 2, 200, 200, 200, 16));
    CHECK_EQ(untouched(s), 0);

    CHECK_EQ(DECODE_PROG(FIX_PROGWIDE, s, NULL), ART_OK);
    CHECK(near(s, s / 2, s / 2, 30, 200, 60, 24));
    CHECK(near(s, 3, s / 2, 220, 30, 30, 40));
    CHECK(near(s, s - 4, s / 2, 40, 60, 210, 40));
    CHECK_EQ(untouched(s), 0);

    CHECK_EQ(DECODE_PROG(FIX_PROGRST, s, NULL), ART_OK);
    check_quadrants(s);
}

/* Mean absolute difference in green (the 6-bit, luma-heavy channel)
 * between the output and a saved copy. */
static uint16_t g_ref[ART_MAX_SIZE * ART_MAX_SIZE];

static int mean_green_diff(int size)
{
    int i, total = 0;
    for (i = 0; i < size * size; i++) {
        total += abs((int)((g_out[i] >> 5) & 0x3F) - (int)((g_ref[i] >> 5) & 0x3F));
    }
    return total * 255 / 63 / (size * size);
}

TEST(a_progressive_cover_is_as_sharp_as_the_same_cover_saved_baseline)
{
    art_info_t info;
    int d;

    /* Same image, saved both ways. The progressive decode must land close
     * to the baseline one — not the soft first-pass-only picture, which
     * misses most of the fine lines. */
    CHECK_EQ(DECODE(FIX_DETBASE, 96, NULL), ART_OK);
    memcpy(g_ref, g_out, sizeof(g_ref));
    CHECK_EQ(DECODE_PROG(FIX_DETPROG, 96, &info), ART_OK);
    CHECK_EQ(info.scale, 1);                  /* 4x4 per block */
    CHECK_EQ(untouched(96), 0);
    d = mean_green_diff(96);
    CHECK(d <= 10);                           /* measured: 6 */

    /* What the first pass alone gives (1/8 = 32 px, enlarged 3x): the
     * blurry picture this path exists to avoid. It must be far worse. */
    {
        static uint16_t small[32 * 32];
        int x, y;
        CHECK_EQ(DECODE_PROG(FIX_DETPROG, 32, &info), ART_OK);
        CHECK_EQ(info.scale, 3);
        memcpy(small, g_out, sizeof(small));
        for (y = 0; y < 96; y++) {
            for (x = 0; x < 96; x++) g_out[y * 96 + x] = small[(y / 3) * 32 + x / 3];
        }
        CHECK(mean_green_diff(96) >= 3 * d);  /* measured: 28 vs 6 */
    }
}

TEST(the_progressive_path_turns_away_what_it_cannot_do)
{
    static const uint8_t junk[64] = { 0x12, 0x34 };
    art_result_t r;

    /* a baseline file is the other decoder's job */
    CHECK_EQ(DECODE_PROG(FIX_QUAD400, 176, NULL), ART_ERR_UNSUPPORTED);
    CHECK(DECODE_PROG(junk, 176, NULL) != ART_OK);

    /* cut off inside the first scan: an error, not garbage */
    r = decode_prog(FIX_PROG1600, sizeof(FIX_PROG1600) / 2, 176, NULL, NULL);
    CHECK(r != ART_OK);
    r = decode_prog(FIX_PROG1600, 40, 176, NULL, NULL);
    CHECK(r != ART_OK);
}

TEST(damaged_input_fails_cleanly)
{
    static const uint8_t junk[64] = { 0x12, 0x34, 0x56 };
    art_result_t r;

    /* TJpgDec hunts for the start-of-image marker, so junk reads as the
     * stream ending before a JPEG was found. Either way: not ART_OK. */
    r = DECODE(junk, 176, NULL);
    CHECK(r == ART_ERR_READ || r == ART_ERR_FORMAT);

    /* Cut off mid-stream: some error, never ART_OK, never a crash. */
    r = decode(FIX_QUAD400, sizeof(FIX_QUAD400) / 2, 176, NULL, NULL);
    CHECK(r != ART_OK);

    r = decode(FIX_QUAD400, 10, 176, NULL, NULL);
    CHECK(r != ART_OK);

    r = decode(FIX_QUAD400, 0, 176, NULL, NULL);
    CHECK(r != ART_OK);
}

TEST(the_decode_yields_to_the_audio_regularly)
{
    int yields = 0;
    CHECK_EQ(decode(FIX_QUAD400, sizeof(FIX_QUAD400), 216, NULL, &yields),
             ART_OK);
    /* One per block TJpgDec produces: 25 x 25 MCUs for a 400 px 4:2:0
     * image. The point is "often", not an exact count. */
    CHECK(yields >= 100);
}

TEST(a_null_yield_is_fine)
{
    mem_t m = { FIX_QUAD400, sizeof(FIX_QUAD400), 0, 0 };
    art_src_t src = { mem_read, NULL, &m, NULL };
    CHECK_EQ(art_decode_jpeg(&src, g_out, 176, &g_work, NULL), ART_OK);
}

TEST(bad_arguments_are_rejected)
{
    mem_t m = { FIX_QUAD400, sizeof(FIX_QUAD400), 0, 0 };
    art_src_t src = { mem_read, NULL, &m, NULL };
    art_src_t no_read = { NULL, NULL, &m, NULL };

    CHECK_EQ(art_decode_jpeg(NULL, g_out, 176, &g_work, NULL), ART_ERR_ARG);
    CHECK_EQ(art_decode_jpeg(&no_read, g_out, 176, &g_work, NULL), ART_ERR_ARG);
    CHECK_EQ(art_decode_jpeg(&src, NULL, 176, &g_work, NULL), ART_ERR_ARG);
    CHECK_EQ(art_decode_jpeg(&src, g_out, 176, NULL, NULL), ART_ERR_ARG);
    CHECK_EQ(art_decode_jpeg(&src, g_out, 0, &g_work, NULL), ART_ERR_ARG);
    CHECK_EQ(art_decode_jpeg(&src, g_out, ART_MAX_SIZE + 1, &g_work, NULL),
             ART_ERR_ARG);
}

TEST(every_result_has_a_name)
{
    int r;
    for (r = ART_OK; r <= ART_ERR_MEM; r++) {
        CHECK(art_result_name((art_result_t)r)[0] != '?');
    }
    CHECK(art_result_name((art_result_t)99)[0] == '?');
}

int main(void)
{
    printf("art\n");
    RUN(a_cover_shrinks_to_both_art_sizes);
    RUN(big_covers_use_the_deeper_descales);
    RUN(the_quadrant_edges_stay_where_they_belong);
    RUN(a_small_cover_is_enlarged_with_every_pixel_written);
    RUN(a_wide_cover_is_cropped_to_its_centre);
    RUN(greyscale_covers_decode);
    RUN(progressive_jpegs_are_turned_away_by_the_baseline_path);
    RUN(a_small_progressive_cover_decodes_at_full_detail);
    RUN(a_big_progressive_cover_shrinks_and_needs_only_its_first_pass);
    RUN(progressive_greyscale_and_444_and_restarts_decode);
    RUN(a_progressive_cover_is_as_sharp_as_the_same_cover_saved_baseline);
    RUN(the_progressive_path_turns_away_what_it_cannot_do);
    RUN(damaged_input_fails_cleanly);
    RUN(the_decode_yields_to_the_audio_regularly);
    RUN(a_null_yield_is_fine);
    RUN(bad_arguments_are_rejected);
    RUN(every_result_has_a_name);
    return TEST_SUMMARY();
}
