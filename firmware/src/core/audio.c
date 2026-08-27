#include "audio.h"

int32_t audio_sat32(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

size_t audio_unpack_u8(const uint8_t *src, int32_t *dst, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        /* 8-bit WAV is unsigned, midpoint 128. */
        int32_t s = (int32_t)src[i] - 128;
        /* Shift in the unsigned domain: left-shifting a negative int is
         * undefined behaviour in C, and it really does bite on some ARM
         * optimisation levels. */
        dst[i] = (int32_t)((uint32_t)s << 24);
    }
    return samples;
}

size_t audio_unpack_s16(const uint8_t *src, int32_t *dst, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        int16_t s = (int16_t)((uint16_t)src[2 * i] |
                              ((uint16_t)src[2 * i + 1] << 8));
        dst[i] = (int32_t)((uint32_t)(int32_t)s << 16);
    }
    return samples;
}

size_t audio_unpack_s24(const uint8_t *src, int32_t *dst, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        const uint8_t *p = src + 3 * i;
        /* Build left-justified directly; the low byte lands in bits 8..15,
         * which sign-extends for free because the MSB is in bit 31. */
        uint32_t u = ((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 24);
        dst[i] = (int32_t)u;
    }
    return samples;
}

size_t audio_unpack_s32(const uint8_t *src, int32_t *dst, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        const uint8_t *p = src + 4 * i;
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        dst[i] = (int32_t)u;
    }
    return samples;
}

void audio_mono_to_stereo(int32_t *buf, size_t frames)
{
    /* Expand backwards so the in-place copy never overwrites unread input. */
    for (size_t i = frames; i-- > 0;) {
        int32_t s = buf[i];
        buf[2 * i]     = s;
        buf[2 * i + 1] = s;
    }
}

/*
 * Q16 gains for 0..100%, following a -60 dB .. 0 dB logarithmic taper
 * (dB = -60 + 0.6 * percent), with 0 forced to hard mute.
 * Precomputed so the audio ISR does no math beyond a multiply.
 */
static const uint32_t k_volume_q16[101] = {
    0, 70, 75, 81, 86, 93, 99, 106,
    114, 122, 131, 140, 150, 161, 172, 185,
    198, 212, 227, 243, 261, 280, 300, 321,
    344, 369, 395, 423, 453, 486, 521, 558,
    598, 640, 686, 735, 788, 844, 905, 969,
    1039, 1113, 1193, 1278, 1369, 1467, 1572, 1685,
    1805, 1934, 2072, 2221, 2379, 2550, 2732, 2927,
    3137, 3361, 3601, 3859, 4135, 4431, 4748, 5087,
    5451, 5841, 6259, 6706, 7186, 7700, 8250, 8841,
    9473, 10150, 10876, 11654, 12488, 13381, 14338, 15363,
    16462, 17639, 18901, 20253, 21701, 23253, 24916, 26698,
    28608, 30653, 32846, 35195, 37712, 40409, 43299, 46396,
    49714, 53270, 57079, 61162, 65536,
};

uint32_t audio_volume_q16(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return k_volume_q16[percent];
}

void audio_apply_gain(int32_t *buf, size_t samples, uint32_t gain_q16)
{
    if (gain_q16 == 65536u) {
        return; /* unity: bit-perfect passthrough, do not touch the data */
    }
    for (size_t i = 0; i < samples; i++) {
        int64_t v = ((int64_t)buf[i] * (int64_t)gain_q16) >> 16;
        buf[i] = audio_sat32(v);
    }
}

int32_t audio_peak(const int32_t *buf, size_t samples)
{
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t s = buf[i];
        /* Negate carefully: -INT32_MIN overflows. */
        int32_t a = (s < 0) ? ((s == INT32_MIN) ? INT32_MAX : -s) : s;
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}
