/*
 * MP3 decoder tests against a real file, mirroring test_decoder_flac.c.
 *
 * Two things here that FLAC did not need: the s16 -> Q1.31 shift, and
 * total_frames possibly being 0 when a VBR file carries no Xing header.
 */
#include "test.h"
#include "../src/core/decoder.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef MP3_TEST_FILE
#define MP3_TEST_FILE "C:/Users/maher/Downloads/05 - galore.mp3"
#endif

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

static FILE *open_fixture(void) { return fopen(MP3_TEST_FILE, "rb"); }

static void register_all(void)
{
    decoder_registry_clear();
    decoder_register(&decoder_wav_vt);
    decoder_register(&decoder_flac_vt);
    decoder_register(&decoder_mp3_vt);   /* last: weakest magic number */
}

TEST(mp3_probe_claims_a_real_file)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    uint8_t hdr[DECODER_MAX_HEADER];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    const decoder_vtable_t *vt = decoder_find(hdr, n);
    CHECK(vt == &decoder_mp3_vt);
    fclose(f);
}

TEST(mp3_probe_does_not_claim_wav_or_flac)
{
    /* Registered last for a reason: the MPEG sync pattern is two bytes and
     * could plausibly appear at the head of another format. */
    register_all();
    const uint8_t riff[] = "RIFF\x24\x00\x00\x00" "WAVEfmt ";
    const uint8_t flac[] = "fLaC\x00\x00\x00\x22" "____________";
    CHECK(decoder_find(riff, sizeof riff) != &decoder_mp3_vt);
    CHECK(decoder_find(flac, sizeof flac) != &decoder_mp3_vt);
}

TEST(mp3_opens_and_reports_stream_info)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, file_io(f), &info));
    CHECK(info.sample_rate > 0);
    CHECK(info.channels >= 1 && info.channels <= 2);
    CHECK(strcmp(decoder_name(&d), "mp3") == 0);

    printf("    [%s] %lu Hz, %u ch, %lu frames, arena peak %lu bytes\n",
           decoder_name(&d), (unsigned long)info.sample_rate,
           info.channels, (unsigned long)info.total_frames,
           (unsigned long)decoder_mp3_arena_peak(&d));

    if (info.total_frames == 0)
        printf("    note: no frame count (VBR without Xing header)\n");

    decoder_close(&d);
    fclose(f);
}

TEST(mp3_arena_is_big_enough)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));
    CHECK(decoder_mp3_arena_peak(&d) < DECODER_STATE_BYTES);
    decoder_close(&d);
    fclose(f);
}

TEST(mp3_decodes_frames)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, file_io(f), &info));

    static int32_t out[2048];
    /* Skip the intro; MP3 also carries encoder delay at the very start. */
    if (info.total_frames > 88200) CHECK(decoder_seek_frame(&d, 44100));

    size_t got = decoder_decode(&d, out, 1024);
    CHECK_EQ(got, 1024);

    int nonzero = 0;
    for (int i = 0; i < 2048; i++) if (out[i] != 0) nonzero++;
    CHECK(nonzero > 100);

    decoder_close(&d);
    fclose(f);
}

TEST(mp3_output_is_full_scale_q1_31)
{
    /* dr_mp3 gives s16; the adapter shifts left 16. If that shift were
     * missing the audio would be 65536x too quiet - silent in practice. */
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t out[2048];
    int32_t peak = 0;
    for (int block = 0; block < 200; block++) {
        size_t got = decoder_decode(&d, out, 1024);
        if (got == 0) break;
        for (size_t i = 0; i < got * 2; i++) {
            int32_t v = out[i] < 0 ? -out[i] : out[i];
            if (v > peak) peak = v;
        }
    }
    CHECK(peak > (1 << 24));
    printf("    peak sample: %ld (full scale %ld)\n",
           (long)peak, (long)0x7FFFFFFF);

    /* The bottom 16 bits must be zero - s16 shifted up, nothing below. */
    CHECK_EQ(peak & 0xFFFF, 0);

    decoder_close(&d);
    fclose(f);
}

TEST(mp3_decodes_continuously)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t out[2048];
    uint32_t total = 0;
    for (int i = 0; i < 200; i++) {
        size_t got = decoder_decode(&d, out, 1024);
        if (got == 0) break;
        total += (uint32_t)got;
    }
    CHECK(total > 100000);
    decoder_close(&d);
    fclose(f);
}

TEST(mp3_seek_returns_the_same_audio)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t first[2048], again[2048];
    CHECK(decoder_seek_frame(&d, 44100));
    CHECK_EQ(decoder_decode(&d, first, 1024), 1024);
    CHECK(decoder_seek_frame(&d, 44100));
    CHECK_EQ(decoder_decode(&d, again, 1024), 1024);

    int diffs = 0;
    for (int i = 0; i < 2048; i++) if (first[i] != again[i]) diffs++;
    CHECK_EQ(diffs, 0);

    decoder_close(&d);
    fclose(f);
}

TEST(mp3_reaches_end_of_stream)
{
    FILE *f = open_fixture();
    if (!f) return;
    register_all();

    decoder_t d;
    CHECK(decoder_open(&d, file_io(f), NULL));

    static int32_t out[2048];
    size_t got, total = 0;
    int guard = 0;
    while ((got = decoder_decode(&d, out, 1024)) != 0 && guard++ < 20000)
        total += got;

    CHECK(total > 0);
    CHECK(guard < 20000);
    CHECK_EQ(decoder_decode(&d, out, 1024), 0);
    decoder_close(&d);
    fclose(f);
}

int main(void)
{
    printf("decoder_mp3\n");
    FILE *probe = open_fixture();
    if (!probe) {
        printf("  SKIPPED - fixture not found:\n    %s\n", MP3_TEST_FILE);
        return 0;
    }
    fclose(probe);

    RUN(mp3_probe_claims_a_real_file);
    RUN(mp3_probe_does_not_claim_wav_or_flac);
    RUN(mp3_opens_and_reports_stream_info);
    RUN(mp3_arena_is_big_enough);
    RUN(mp3_decodes_frames);
    RUN(mp3_output_is_full_scale_q1_31);
    RUN(mp3_decodes_continuously);
    RUN(mp3_seek_returns_the_same_audio);
    RUN(mp3_reaches_end_of_stream);
    return TEST_SUMMARY();
}
