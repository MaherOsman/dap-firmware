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

TEST(a_gain_ramp_moves_smoothly_between_two_levels)
{
    int32_t buf[256];
    int i;

    for (i = 0; i < 256; i++) buf[i] = 1 << 24;

    /* half gain to unity across the block */
    audio_apply_gain_ramp(buf, 256, 32768u, 65536u);

    /* starts near the old level, ends near the new one */
    CHECK(buf[0] <= (1 << 24) / 2 + 1000);
    CHECK(buf[255] > (1 << 24) * 3 / 4);

    /* and never steps backwards — a non-monotonic ramp would be a click of
     * its own */
    for (i = 1; i < 256; i++) {
        CHECK(buf[i] >= buf[i - 1]);
    }
}

TEST(a_ramp_between_equal_levels_is_a_flat_gain)
{
    int32_t ramped[64], flat[64];
    int i;

    for (i = 0; i < 64; i++) { ramped[i] = flat[i] = 1 << 20; }

    audio_apply_gain_ramp(ramped, 64, 32768u, 32768u);
    audio_apply_gain(flat, 64, 32768u);

    for (i = 0; i < 64; i++) {
        CHECK_EQ(ramped[i], flat[i]);
    }
}

TEST(a_unity_to_unity_ramp_is_bit_perfect)
{
    int32_t buf[32];
    int i;

    for (i = 0; i < 32; i++) buf[i] = (int32_t)(i * 0x01234567);
    {
        int32_t before[32];
        memcpy(before, buf, sizeof(before));
        audio_apply_gain_ramp(buf, 32, 65536u, 65536u);
        CHECK_EQ(memcmp(before, buf, sizeof(before)), 0);
    }
}

TEST(a_ramp_down_to_silence_ends_silent)
{
    int32_t buf[128];
    int i;

    for (i = 0; i < 128; i++) buf[i] = 1 << 24;
    audio_apply_gain_ramp(buf, 128, 65536u, 0u);

    /* The last sample sits one ramp step short of the target rather than
     * exactly on it: the next block starts flat at the new level, so
     * landing early here would apply the change twice. */
    CHECK(buf[0] > buf[127]);
    CHECK(buf[127] < buf[0] / 64);
    for (i = 1; i < 128; i++) {
        CHECK(buf[i] <= buf[i - 1]);
    }
}

TEST(a_ramp_saturates_rather_than_wrapping)
{
    int32_t buf[16];
    int i;

    for (i = 0; i < 16; i++) buf[i] = INT32_MIN;
    audio_apply_gain_ramp(buf, 16, 65536u, 65536u);
    for (i = 0; i < 16; i++) CHECK(buf[i] <= 0);

    for (i = 0; i < 16; i++) buf[i] = INT32_MAX;
    audio_apply_gain_ramp(buf, 16, 65536u, 65536u);
    for (i = 0; i < 16; i++) CHECK(buf[i] > 0);
}

TEST(an_empty_ramp_does_nothing)
{
    int32_t buf[4] = { 1, 2, 3, 4 };
    audio_apply_gain_ramp(buf, 0, 0u, 65536u);
    CHECK_EQ(buf[0], 1);
    CHECK_EQ(buf[3], 4);
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
    RUN(a_gain_ramp_moves_smoothly_between_two_levels);
    RUN(a_ramp_between_equal_levels_is_a_flat_gain);
    RUN(a_unity_to_unity_ramp_is_bit_perfect);
    RUN(a_ramp_down_to_silence_ends_silent);
    RUN(a_ramp_saturates_rather_than_wrapping);
    RUN(an_empty_ramp_does_nothing);
    return TEST_SUMMARY();
}
