/*
 * FLAC decoder tests, run against a real file.
 *
 * Unlike the WAV tests there is no hand-built fixture here — a valid FLAC
 * stream cannot reasonably be synthesised in a test, and a synthetic one
 * would not exercise the encoder quirks that real files carry anyway.
 *
 * The path below is a local music file. When it is absent the suite reports
 * SKIPPED and passes, so this does not break builds on other machines.
 */
#include "test.h"
#include "../src/core/decoder.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef FLAC_TEST_FILE
#define FLAC_TEST_FILE "C:/Users/maher/Downloads/testmusic/Chon-Grow/12 But.flac"
#endif

/* ---- stdio-backed io: the host stand-in for the FatFs callbacks ---- */

static size_t file_read(void *ctx, void *dst, size_t n)
{
    return fread(dst, 1, n, (FILE *)ctx);
}

static bool file_seek(void *ctx, uint32_t off)
{
    return fseek((FILE *)ctx, (long)off, SEEK_SET) == 0;
}

static uint32_t file_size(void *ctx)
{
    FILE *f = (FILE *)ctx;
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return (uint32_t)end;
}

static decoder_io_t file_io(FILE *f)
{
    decoder_io_t io = { file_read, file_seek, file_size, f };
    return io;
}

static FILE *open_fixture(void)
{
    return fopen(FLAC_TEST_FILE, "rb");
}

static void register_all(void)
{
    decoder_registry_clear();
    decoder_register(&decoder_wav_vt);
    decoder_register(&decoder_flac_vt);
}

/* ---- tests ---- */

TEST(flac_probe_claims_a_real_file)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    uint8_t hdr[DECODER_MAX_HEADER];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    const decoder_vtable_t *vt = decoder_find(hdr, n);
    CHECK(vt == &decoder_flac_vt);
    fclose(f);
}

TEST(flac_opens_and_reports_stream_info)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, file_io(f), &info));
    CHECK_EQ(info.sample_rate, 44100);
    CHECK_EQ(info.channels, 2);
    CHECK(info.total_frames > 0);
    CHECK(strcmp(decoder_name(&d), "flac") == 0);

    printf("    [%s] %lu Hz, %u-bit, %u ch, %lu frames, arena peak %lu bytes\n",
           decoder_name(&d), (unsigned long)info.sample_rate,
           info.bits_per_sample, info.channels,
           (unsigned long)info.total_frames,
           (unsigned long)decoder_flac_arena_peak(&d));

    decoder_close(&d);
    fclose(f);
}

TEST(flac_arena_is_big_enough)
{
    /* The whole point of the bump allocator: if dr_flac ever needs more than
     * the arena holds, open fails outright rather than corrupting memory.
     * This test is what tells us DECODER_STATE_BYTES is correctly sized. */
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));
    size_t peak = decoder_flac_arena_peak(&d);
    CHECK(peak > 0);
    CHECK(peak < DECODER_STATE_BYTES);
    decoder_close(&d);
    fclose(f);
}

TEST(flac_decodes_frames)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    /* Track intros are often silent — a fade-in, a count-off gap, or just
     * leading digital black. Sample from a second in, where any real music
     * will have content, so this tests the decoder rather than the mix. */
    CHECK(decoder_seek_frame(&d, 44100));

    static int32_t out[2048];
    size_t got = decoder_decode(&d, out, 1024);
    CHECK_EQ(got, 1024);

    /* If every sample is zero the decoder ran but produced nothing — a
     * failure that would sound exactly like a wiring fault on hardware. */
    int nonzero = 0;
    for (int i = 0; i < 2048; i++) if (out[i] != 0) nonzero++;
    CHECK(nonzero > 100);

    decoder_close(&d);
    fclose(f);
}

TEST(flac_decodes_continuously)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, file_io(f), &info));

    static int32_t out[2048];
    uint32_t total = 0;
    for (int i = 0; i < 200; i++) {
        size_t got = decoder_decode(&d, out, 1024);
        if (got == 0) break;
        total += (uint32_t)got;
    }
    /* 200 x 1024 frames is ~4.6 s of audio; the file is far longer. */
    CHECK_EQ(total, 200u * 1024u);
    decoder_close(&d);
    fclose(f);
}

TEST(flac_seek_returns_the_same_audio)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t first[2048], again[2048];

    CHECK(decoder_seek_frame(&d, 100000));
    CHECK_EQ(decoder_decode(&d, first, 1024), 1024);

    CHECK(decoder_seek_frame(&d, 100000));
    CHECK_EQ(decoder_decode(&d, again, 1024), 1024);

    int diffs = 0;
    for (int i = 0; i < 2048; i++) if (first[i] != again[i]) diffs++;
    CHECK_EQ(diffs, 0);

    decoder_close(&d);
    fclose(f);
}

TEST(flac_reaches_end_of_stream)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, file_io(f), &info));

    /* Seek near the end and drain, proving EOF is reported as 0 frames
     * rather than hanging or returning garbage forever. */
    uint32_t near_end = info.total_frames > 2048 ? info.total_frames - 2048 : 0;
    CHECK(decoder_seek_frame(&d, near_end));

    static int32_t out[2048];
    size_t got, total = 0;
    int guard = 0;
    while ((got = decoder_decode(&d, out, 1024)) != 0 && guard++ < 100)
        total += got;

    CHECK(total > 0);
    CHECK(guard < 100);                       /* did not spin forever */
    CHECK_EQ(decoder_decode(&d, out, 1024), 0);
    decoder_close(&d);
    fclose(f);
}

TEST(flac_output_is_full_scale_q1_31)
{
    /* dr_flac's s32 output is left-justified to the full int32 range, which
     * is our Q1.31 convention. If it were not, 24-bit source material would
     * come out 256x too quiet — audible, but easy to misdiagnose as a volume
     * or amplifier problem on hardware. */
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t out[2048];
    int32_t peak = 0;
    for (int block = 0; block < 100; block++) {
        size_t got = decoder_decode(&d, out, 1024);
        if (got == 0) break;
        for (size_t i = 0; i < got * 2; i++) {
            int32_t v = out[i] < 0 ? -out[i] : out[i];
            if (v > peak) peak = v;
        }
    }
    /* Mastered music should peak well above 1/256 of full scale. */
    CHECK(peak > (1 << 23));
    printf("    peak sample: %ld (full scale %ld)\n",
           (long)peak, (long)0x7FFFFFFF);

    decoder_close(&d);
    fclose(f);
}

TEST(flac_arena_across_the_library)
{
    /* One file told us 42000 bytes. That is a sample, not a bound — dr_flac
     * allocates against the max block size in each file's STREAMINFO, and a
     * differently-encoded album can ask for more. This walks a folder so
     * DECODER_STATE_BYTES is set from a measured worst case. */
    static const char *dir = "C:/Users/maher/Downloads/testmusic/Chon-Grow";
    static const char *names[] = {
        "01 Drift.flac", "02 Story.flac", "03 Fall.flac", "04 Book.flac",
        "05 Can't Wait.flac", "06 Suda.flac", "07 Knot.flac", "08 Moon.flac",
        "09 Splash.flac", "10 Perfect Pillow.flac", "11 Echo.flac",
        "12 But.flac",
    };

    register_all();
    size_t worst = 0;
    int opened = 0;

    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;

        decoder_t d;
        decoder_info_t info;
        if (decoder_open(&d, file_io(f), &info)) {
            size_t peak = decoder_flac_arena_peak(&d);
            if (peak > worst) worst = peak;
            opened++;
            printf("    %-26s %6lu bytes  %2u-bit\n",
                   names[i], (unsigned long)peak, info.bits_per_sample);
            decoder_close(&d);
        } else {
            printf("    %-26s FAILED TO OPEN\n", names[i]);
            CHECK(false);
        }
        fclose(f);
    }

    if (opened == 0) return;    /* library not on this machine */
    printf("    worst arena peak: %lu of %u bytes (%lu%% headroom)\n",
           (unsigned long)worst, DECODER_STATE_BYTES,
           (unsigned long)(100u - (worst * 100u / DECODER_STATE_BYTES)));
    CHECK(worst < DECODER_STATE_BYTES);
}

int main(void)
{
    printf("decoder_flac\n");
    FILE *probe = open_fixture();
    if (!probe) {
        printf("  SKIPPED - fixture not found:\n    %s\n", FLAC_TEST_FILE);
        return 0;
    }
    fclose(probe);

    RUN(flac_probe_claims_a_real_file);
    RUN(flac_opens_and_reports_stream_info);
    RUN(flac_arena_is_big_enough);
    RUN(flac_decodes_frames);
    RUN(flac_decodes_continuously);
    RUN(flac_seek_returns_the_same_audio);
    RUN(flac_reaches_end_of_stream);
    RUN(flac_output_is_full_scale_q1_31);
    RUN(flac_arena_across_the_library);
    return TEST_SUMMARY();
}
