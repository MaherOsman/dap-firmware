/*
 * audio — sample format conversion and volume, all fixed-point.
 *
 * Why fixed point: the H7 has a hardware FPU and floats would work, but the
 * I2S path runs in an interrupt, and keeping the ISR free of FPU context
 * saves both time and a class of "I forgot to enable lazy stacking" bugs.
 * Everything here is int32/int64 only.
 *
 * Internal working format is signed 32-bit, left-justified (Q1.31), which is
 * what the PCM5102 wants over I2S in 32-bit frames and what makes 24-bit
 * files pass through bit-perfect.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* Unpack `frames*channels` samples from a little-endian PCM byte stream into
 * left-justified int32. Returns number of samples written. */
size_t audio_unpack_s16(const uint8_t *src, int32_t *dst, size_t samples);
size_t audio_unpack_s24(const uint8_t *src, int32_t *dst, size_t samples);
size_t audio_unpack_s32(const uint8_t *src, int32_t *dst, size_t samples);
/* 8-bit WAV is unsigned with a 0x80 bias — a classic silent-bug source. */
size_t audio_unpack_u8(const uint8_t *src, int32_t *dst, size_t samples);

/* Duplicate mono into interleaved stereo, in place over a buffer sized 2*n. */
void audio_mono_to_stereo(int32_t *buf, size_t frames);

/*
 * Volume. 0..100 on the UI maps to a logarithmic (roughly -60 dB .. 0 dB)
 * Q16 gain, because linear volume controls feel wrong to the ear: half the
 * knob should be about a quarter of the power, not half the amplitude.
 */
uint32_t audio_volume_q16(uint8_t percent);
/* Apply gain with saturation. gain is Q16 (65536 == unity). */
void audio_apply_gain(int32_t *buf, size_t samples, uint32_t gain_q16);

/* Peak meter for the UI, returns the largest absolute sample. */
int32_t audio_peak(const int32_t *buf, size_t samples);

/* Saturating helpers, exposed for testing. */
int32_t audio_sat32(int64_t v);

#endif /* AUDIO_H */
