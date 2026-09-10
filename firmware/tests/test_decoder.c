#include "test.h"
#include "../src/core/decoder.h"
#include "../src/core/audio.h"
#include <stdlib.h>

/* --- memory-backed io, so the same fixtures test_wav.c uses work here --- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int    reads;        /* call count, to prove streaming actually streams */
} memio_t;

static size_t mem_read(void *ctx, void *dst, size_t n)
{
    memio_t *m = (memio_t *)ctx;
    m->reads++;
    size_t avail = m->len - m->pos;
    if (n > avail) n = avail;
    memcpy(dst, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static bool mem_seek(void *ctx, uint32_t off)
{
    memio_t *m = (memio_t *)ctx;
    if (off > m->len) return false;
    m->pos = off;
    return true;
}

static uint32_t mem_size(void *ctx) { return (uint32_t)((memio_t *)ctx)->len; }

static decoder_io_t mem_io(memio_t *m)
{
    decoder_io_t io = { mem_read, mem_seek, mem_size, m };
    return io;
}

/* --- WAV builder, same shape as test_wav.c --- */

static uint8_t  buf[65536];
static size_t   len;

static void put(const void *p, size_t n) { memcpy(buf + len, p, n); len += n; }
static void put_tag(const char *t) { put(t, 4); }
static void put32(uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
    put(b, 4);
}
static void put16(uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, 2);
}

/* Builds a WAV whose samples are a known ramp, so decoded output can be
 * checked against a value we can compute rather than just "not zero". */
static uint32_t build_wav_ramp(uint16_t ch, uint32_t rate, uint16_t bits,
                               uint32_t frames)
{
    uint16_t block = (uint16_t)(ch * (bits / 8u));
    uint32_t data_bytes = frames * block;

    len = 0;
    put_tag("RIFF"); put32(0); put_tag("WAVE");
    put_tag("fmt "); put32(16);
    put16(1); put16(ch); put32(rate);
    put32(rate * block); put16(block); put16(bits);
    put_tag("data"); put32(data_bytes);

    for (uint32_t i = 0; i < frames * ch; i++) {
        uint32_t v = i & 0x7Fu;               /* small ramp, no sign games */
        for (uint16_t b = 0; b < bits / 8u; b++) {
            uint8_t byte = (b == (bits / 8u) - 1u) ? (uint8_t)v : 0u;
            put(&byte, 1);
        }
    }
    return data_bytes;
}

static void register_wav_only(void)
{
    decoder_registry_clear();
    decoder_register(&decoder_wav_vt);
}

/* --- registry ---------------------------------------------------------- */

TEST(registry_starts_empty_and_registers)
{
    decoder_registry_clear();
    CHECK_EQ(decoder_registry_count(), 0);
    decoder_register(&decoder_wav_vt);
    CHECK_EQ(decoder_registry_count(), 1);
}

TEST(registry_ignores_null)
{
    decoder_registry_clear();
    decoder_register(NULL);
    CHECK_EQ(decoder_registry_count(), 0);
}

TEST(find_matches_wav_header)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 4);
    const decoder_vtable_t *vt = decoder_find(buf, len);
    CHECK(vt != NULL);
    CHECK(vt == &decoder_wav_vt);
}

TEST(find_rejects_non_wav)
{
    register_wav_only();
    const uint8_t flac[] = "fLaC\0\0\0\x22____________";
    CHECK(decoder_find(flac, sizeof flac) == NULL);
}

TEST(find_rejects_riff_that_is_not_wave)
{
    register_wav_only();
    /* A RIFF container holding AVI must not be claimed by the wav decoder. */
    const uint8_t avi[] = "RIFF\x10\x00\x00\x00" "AVI LIST";
    CHECK(decoder_find(avi, sizeof avi) == NULL);
}

TEST(find_rejects_truncated_header)
{
    register_wav_only();
    const uint8_t tiny[] = "RIFF";
    CHECK(decoder_find(tiny, sizeof tiny) == NULL);
}

/* --- open -------------------------------------------------------------- */

TEST(open_reports_format)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 24, 100);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    decoder_info_t info;

    CHECK(decoder_open(&d, mem_io(&m), &info));
    CHECK_EQ(info.sample_rate, 44100);
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.bits_per_sample, 24);
    CHECK_EQ(info.total_frames, 100);
    CHECK(strcmp(decoder_name(&d), "wav") == 0);
    decoder_close(&d);
}

TEST(open_rewinds_after_probe)
{
    /* decoder_open reads a header to probe, then must seek back to 0 so the
     * decoder sees the file from the start. If it did not, wav_parse would
     * be handed bytes from the middle of the header and fail. */
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 10);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));
    decoder_close(&d);
}

TEST(open_fails_on_unknown_format)
{
    register_wav_only();
    const uint8_t junk[64] = { 0 };
    memio_t m = { junk, sizeof junk, 0, 0 };
    decoder_t d;
    CHECK(!decoder_open(&d, mem_io(&m), NULL));
}

TEST(open_fails_when_no_decoder_registered)
{
    decoder_registry_clear();
    build_wav_ramp(2, 44100, 16, 10);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(!decoder_open(&d, mem_io(&m), NULL));
}

/* --- decode ------------------------------------------------------------ */

TEST(decode_returns_frames_and_stops_at_eof)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 50);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    int32_t out[256];
    size_t total = 0;
    for (int i = 0; i < 20; i++) {
        size_t got = decoder_decode(&d, out, 16);
        if (got == 0) break;
        total += got;
    }
    CHECK_EQ(total, 50);
    CHECK_EQ(decoder_decode(&d, out, 16), 0);   /* stays at EOF */
    decoder_close(&d);
}

TEST(decode_matches_direct_unpack)
{
    /* The seam must be transparent: bytes through the decoder must equal
     * bytes through audio_unpack_s24() called directly on the same data. */
    register_wav_only();
    build_wav_ramp(2, 44100, 24, 32);
    uint32_t data_off = (uint32_t)(len - (32u * 6u));

    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    int32_t via_decoder[64];
    CHECK_EQ(decoder_decode(&d, via_decoder, 32), 32);

    int32_t direct[64];
    audio_unpack_s24(buf + data_off, direct, 64);

    for (int i = 0; i < 64; i++) CHECK_EQ(via_decoder[i], direct[i]);
    decoder_close(&d);
}

TEST(decode_never_emits_a_partial_frame)
{
    /* Truncate the file mid-frame. The decoder must drop the ragged tail
     * rather than hand a half-frame to the audio path, which would swap the
     * channels for the rest of the track. */
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 20);
    memio_t m = { buf, len - 3, 0, 0 };     /* 3 bytes short of a frame */
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    int32_t out[128];
    size_t total = 0, got;
    while ((got = decoder_decode(&d, out, 8)) != 0) total += got;
    CHECK_EQ(total, 19);                    /* the ragged 20th is dropped */
    decoder_close(&d);
}

TEST(decode_widens_mono_to_stereo)
{
    register_wav_only();
    build_wav_ramp(1, 44100, 16, 16);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    decoder_info_t info;
    CHECK(decoder_open(&d, mem_io(&m), &info));
    CHECK_EQ(info.channels, 1);

    int32_t out[64];
    CHECK_EQ(decoder_decode(&d, out, 16), 16);
    /* Every frame must have identical left and right. */
    for (int i = 0; i < 16; i++) CHECK_EQ(out[i * 2], out[i * 2 + 1]);
    decoder_close(&d);
}

TEST(decode_clamps_oversized_requests)
{
    /* Asking for more frames than the scratch buffer holds must return a
     * short read, not overflow. */
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 4000);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    static int32_t big[8192];
    size_t got = decoder_decode(&d, big, 4000);
    CHECK(got > 0);
    CHECK(got <= 1024);          /* 4096 scratch / 4 bytes per frame */
    decoder_close(&d);
}

TEST(decode_on_closed_decoder_returns_zero)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 10);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));
    decoder_close(&d);

    int32_t out[16];
    CHECK_EQ(decoder_decode(&d, out, 4), 0);
}

/* --- seek -------------------------------------------------------------- */

TEST(seek_repositions_the_stream)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 200);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    int32_t first[32], again[32];
    CHECK_EQ(decoder_decode(&d, first, 16), 16);

    CHECK(decoder_seek_frame(&d, 0));
    CHECK_EQ(decoder_decode(&d, again, 16), 16);
    for (int i = 0; i < 32; i++) CHECK_EQ(first[i], again[i]);
    decoder_close(&d);
}

TEST(seek_past_end_is_rejected)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 50);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));
    CHECK(!decoder_seek_frame(&d, 999999));
    decoder_close(&d);
}

TEST(seek_to_end_yields_no_frames)
{
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 50);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));
    CHECK(decoder_seek_frame(&d, 50));
    int32_t out[16];
    CHECK_EQ(decoder_decode(&d, out, 8), 0);
    decoder_close(&d);
}

/* --- streaming behaviour ----------------------------------------------- */

TEST(decode_streams_rather_than_slurping)
{
    /* A decoder that read the whole file on open would defeat the ring
     * buffer entirely — on hardware the file is 40 MB and RAM is 1 MB. */
    register_wav_only();
    build_wav_ramp(2, 44100, 16, 4000);
    memio_t m = { buf, len, 0, 0 };
    decoder_t d;
    CHECK(decoder_open(&d, mem_io(&m), NULL));

    int open_reads = m.reads;
    CHECK(m.pos < 8192);        /* open touched only the header region */

    int32_t out[512];
    decoder_decode(&d, out, 256);
    CHECK(m.reads > open_reads);
    decoder_close(&d);
}

int main(void)
{
    printf("decoder\n");
    RUN(registry_starts_empty_and_registers);
    RUN(registry_ignores_null);
    RUN(find_matches_wav_header);
    RUN(find_rejects_non_wav);
    RUN(find_rejects_riff_that_is_not_wave);
    RUN(find_rejects_truncated_header);
    RUN(open_reports_format);
    RUN(open_rewinds_after_probe);
    RUN(open_fails_on_unknown_format);
    RUN(open_fails_when_no_decoder_registered);
    RUN(decode_returns_frames_and_stops_at_eof);
    RUN(decode_matches_direct_unpack);
    RUN(decode_never_emits_a_partial_frame);
    RUN(decode_widens_mono_to_stereo);
    RUN(decode_clamps_oversized_requests);
    RUN(decode_on_closed_decoder_returns_zero);
    RUN(seek_repositions_the_stream);
    RUN(seek_past_end_is_rejected);
    RUN(seek_to_end_yields_no_frames);
    RUN(decode_streams_rather_than_slurping);
    return TEST_SUMMARY();
}
