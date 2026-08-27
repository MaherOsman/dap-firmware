#include "test.h"
#include "../src/core/wav.h"

/* --- tiny WAV builder so tests read like the files they describe --- */

static uint8_t buf[512];
static size_t  len;

static void put(const void *p, size_t n)
{
    memcpy(buf + len, p, n);
    len += n;
}
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

static void build_wav(uint16_t fmt, uint16_t ch, uint32_t rate, uint16_t bits,
                      uint32_t data_bytes, bool with_junk)
{
    len = 0;
    put_tag("RIFF");
    put32(0); /* size — parser must not depend on it */
    put_tag("WAVE");

    if (with_junk) {
        put_tag("LIST");
        put32(10);
        put("INFOxxxxxx", 10);
    }

    put_tag("fmt ");
    put32(16);
    put16(fmt);
    put16(ch);
    put32(rate);
    put32(rate * ch * (bits / 8));  /* byte rate */
    put16((uint16_t)(ch * (bits / 8)));
    put16(bits);

    put_tag("data");
    put32(data_bytes);
}

TEST(wav_parses_cd_quality_stereo)
{
    build_wav(1, 2, 44100, 16, 44100 * 4, false);
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);
    CHECK_EQ(i.codec, WAV_CODEC_PCM);
    CHECK_EQ(i.channels, 2);
    CHECK_EQ(i.sample_rate, 44100);
    CHECK_EQ(i.bits_per_sample, 16);
    CHECK_EQ(i.block_align, 4);
    CHECK_EQ(i.total_frames, 44100);
    CHECK_EQ(wav_duration_ms(&i), 1000);
}

TEST(wav_parses_24bit_hires)
{
    build_wav(1, 2, 96000, 24, 96000 * 6 * 3, false);
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);
    CHECK_EQ(i.bits_per_sample, 24);
    CHECK_EQ(i.block_align, 6);
    CHECK_EQ(i.sample_rate, 96000);
    CHECK_EQ(wav_duration_ms(&i), 3000);
}

TEST(wav_skips_junk_chunks_before_data)
{
    build_wav(1, 2, 48000, 16, 4800, true);
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);
    CHECK_EQ(i.sample_rate, 48000);
    /* data_offset must point past the junk, not at a fixed 44. */
    CHECK(i.data_offset > 44);
}

TEST(wav_handles_odd_sized_chunk_padding)
{
    len = 0;
    put_tag("RIFF"); put32(0); put_tag("WAVE");
    put_tag("junk"); put32(3); put("abc", 3); put("\0", 1); /* pad byte */
    put_tag("fmt "); put32(16);
    put16(1); put16(2); put32(44100); put32(176400); put16(4); put16(16);
    put_tag("data"); put32(1000);

    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);
    CHECK_EQ(i.sample_rate, 44100);
}

TEST(wav_handles_format_extensible)
{
    len = 0;
    put_tag("RIFF"); put32(0); put_tag("WAVE");
    put_tag("fmt "); put32(40);
    put16(0xFFFE); put16(2); put32(44100); put32(264600); put16(6); put16(24);
    put16(22);       /* cbSize */
    put16(24);       /* valid bits */
    put32(3);        /* channel mask */
    put16(1);        /* SubFormat GUID: first 2 bytes == PCM */
    {
        uint8_t guid_rest[14] = {0};
        put(guid_rest, sizeof(guid_rest)); /* rest of the 16-byte GUID */
    }
    put_tag("data"); put32(600);

    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);
    CHECK_EQ(i.codec, WAV_CODEC_PCM);
    CHECK_EQ(i.bits_per_sample, 24);
}

TEST(wav_rejects_garbage)
{
    wav_info_t i;
    CHECK_EQ(wav_parse((const uint8_t *)"nope", 4, &i), WAV_ERR_TOO_SHORT);

    len = 0;
    put_tag("XXXX"); put32(0); put_tag("WAVE");
    CHECK_EQ(wav_parse(buf, len, &i), WAV_ERR_NOT_RIFF);

    len = 0;
    put_tag("RIFF"); put32(0); put_tag("AVI ");
    CHECK_EQ(wav_parse(buf, len, &i), WAV_ERR_NOT_WAVE);
}

TEST(wav_rejects_unsupported_codec)
{
    build_wav(0x0011, 2, 44100, 4, 1000, false); /* IMA ADPCM */
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_ERR_UNSUPPORTED);
}

TEST(wav_reports_missing_data_chunk)
{
    len = 0;
    put_tag("RIFF"); put32(0); put_tag("WAVE");
    put_tag("fmt "); put32(16);
    put16(1); put16(2); put32(44100); put32(176400); put16(4); put16(16);
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_ERR_NO_DATA);
}

TEST(wav_seek_offsets_are_frame_aligned)
{
    build_wav(1, 2, 44100, 16, 44100 * 4 * 10, false);
    wav_info_t i;
    CHECK_EQ(wav_parse(buf, len, &i), WAV_OK);

    uint32_t off = wav_ms_to_offset(&i, 5000);
    CHECK_EQ((off - i.data_offset) % i.block_align, 0);
    CHECK_EQ(off - i.data_offset, 44100u * 4u * 5u);

    /* Seeking past the end clamps to the end of the data chunk. */
    uint32_t end = wav_ms_to_offset(&i, 999999);
    CHECK_EQ(end, i.data_offset + i.data_bytes);
}

int main(void)
{
    printf("wav\n");
    RUN(wav_parses_cd_quality_stereo);
    RUN(wav_parses_24bit_hires);
    RUN(wav_skips_junk_chunks_before_data);
    RUN(wav_handles_odd_sized_chunk_padding);
    RUN(wav_handles_format_extensible);
    RUN(wav_rejects_garbage);
    RUN(wav_rejects_unsupported_codec);
    RUN(wav_reports_missing_data_chunk);
    RUN(wav_seek_offsets_are_frame_aligned);
    return TEST_SUMMARY();
}
