#include "test.h"
#include "../src/core/audio.h"

TEST(unpack_s16_left_justifies)
{
    /* 0x7FFF (full scale positive) and 0x8000 (full scale negative), LE. */
    const uint8_t src[] = {0xFF, 0x7F, 0x00, 0x80, 0x00, 0x00};
    int32_t dst[3];
    CHECK_EQ(audio_unpack_s16(src, dst, 3), 3);
    CHECK_EQ(dst[0], 0x7FFF0000);
    CHECK_EQ(dst[1], (int32_t)0x80000000);
    CHECK_EQ(dst[2], 0);
}

TEST(unpack_s24_sign_extends)
{
    /* 24-bit LE: 0x7FFFFF, 0x800000, 0xFFFFFF (== -1) */
    const uint8_t src[] = {0xFF, 0xFF, 0x7F,
                           0x00, 0x00, 0x80,
                           0xFF, 0xFF, 0xFF};
    int32_t dst[3];
    audio_unpack_s24(src, dst, 3);
    CHECK_EQ(dst[0], 0x7FFFFF00);
    CHECK_EQ(dst[1], (int32_t)0x80000000);
    CHECK_EQ(dst[2], (int32_t)0xFFFFFF00); /* negative, near zero */
    CHECK(dst[2] < 0);
}

TEST(unpack_u8_removes_the_128_bias)
{
    const uint8_t src[] = {128, 255, 0};
    int32_t dst[3];
    audio_unpack_u8(src, dst, 3);
    CHECK_EQ(dst[0], 0);          /* midpoint is silence, not full scale */
    CHECK(dst[1] > 0);
    CHECK(dst[2] < 0);
}

TEST(unpack_s32_is_passthrough)
{
    const uint8_t src[] = {0x78, 0x56, 0x34, 0x12};
    int32_t dst[1];
    audio_unpack_s32(src, dst, 1);
    CHECK_EQ(dst[0], 0x12345678);
}

TEST(mono_to_stereo_duplicates_in_place)
{
    int32_t buf[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    audio_mono_to_stereo(buf, 4);
    CHECK_EQ(buf[0], 1); CHECK_EQ(buf[1], 1);
    CHECK_EQ(buf[2], 2); CHECK_EQ(buf[3], 2);
    CHECK_EQ(buf[4], 3); CHECK_EQ(buf[5], 3);
    CHECK_EQ(buf[6], 4); CHECK_EQ(buf[7], 4);
}

TEST(volume_curve_is_monotonic_and_bounded)
{
    CHECK_EQ(audio_volume_q16(0), 0);        /* hard mute */
    CHECK_EQ(audio_volume_q16(100), 65536);  /* unity */
    CHECK_EQ(audio_volume_q16(200), 65536);  /* clamped */
    for (int p = 1; p <= 100; p++) {
        CHECK(audio_volume_q16((uint8_t)p) > audio_volume_q16((uint8_t)(p - 1)));
    }
    /* Log taper: halfway on the knob is far below half amplitude. */
    CHECK(audio_volume_q16(50) < 65536 / 8);
}

TEST(unity_gain_is_bit_perfect)
{
    int32_t buf[4] = {12345, -67890, INT32_MAX, INT32_MIN};
    int32_t copy[4];
    memcpy(copy, buf, sizeof(buf));
    audio_apply_gain(buf, 4, 65536);
    CHECK_EQ(memcmp(buf, copy, sizeof(buf)), 0);
}

TEST(gain_halves_and_saturates)
{
    int32_t buf[2] = {1000, -1000};
    audio_apply_gain(buf, 2, 32768); /* -6 dB */
    CHECK_EQ(buf[0], 500);
    CHECK_EQ(buf[1], -500);

    int32_t loud[2] = {INT32_MAX, INT32_MIN};
    audio_apply_gain(loud, 2, 65536 * 4); /* +12 dB, must clip not wrap */
    CHECK_EQ(loud[0], INT32_MAX);
    CHECK_EQ(loud[1], INT32_MIN);
}

TEST(mute_produces_silence)
{
    int32_t buf[3] = {INT32_MAX, -5, 12345};
    audio_apply_gain(buf, 3, audio_volume_q16(0));
    CHECK_EQ(buf[0], 0); CHECK_EQ(buf[1], 0); CHECK_EQ(buf[2], 0);
}

TEST(peak_handles_int32_min)
{
    int32_t buf[3] = {INT32_MIN, 5, -7};
    /* Naive -INT32_MIN overflows and would report a negative peak. */
    CHECK(audio_peak(buf, 3) > 0);
    CHECK_EQ(audio_peak(buf, 3), INT32_MAX);
}

TEST(saturation_helper)
{
    CHECK_EQ(audio_sat32((int64_t)INT32_MAX + 1), INT32_MAX);
    CHECK_EQ(audio_sat32((int64_t)INT32_MIN - 1), INT32_MIN);
    CHECK_EQ(audio_sat32(0), 0);
}

int main(void)
{
    printf("audio\n");
    RUN(unpack_s16_left_justifies);
    RUN(unpack_s24_sign_extends);
    RUN(unpack_u8_removes_the_128_bias);
    RUN(unpack_s32_is_passthrough);
    RUN(mono_to_stereo_duplicates_in_place);
    RUN(volume_curve_is_monotonic_and_bounded);
    RUN(unity_gain_is_bit_perfect);
    RUN(gain_halves_and_saturates);
    RUN(mute_produces_silence);
    RUN(peak_handles_int32_min);
    RUN(saturation_helper);
    return TEST_SUMMARY();
}
