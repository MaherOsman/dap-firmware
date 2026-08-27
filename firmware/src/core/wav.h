/*
 * wav — streaming RIFF/WAVE parser.
 *
 * Deliberately written to be fed a header block, not a FILE*, so it can be
 * unit-tested on a PC and later driven by FatFs f_read() on the STM32 with
 * zero changes.
 *
 * Supports: PCM 8/16/24/32-bit integer, IEEE float 32-bit (detected and
 * reported, decoding left to the sample converter), WAVE_FORMAT_EXTENSIBLE,
 * and files with LIST/INFO or other junk chunks before `data`.
 */
#ifndef WAV_H
#define WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WAV_OK = 0,
    WAV_ERR_TOO_SHORT,      /* not enough bytes to decide anything */
    WAV_ERR_NOT_RIFF,       /* missing 'RIFF' magic */
    WAV_ERR_NOT_WAVE,       /* missing 'WAVE' magic */
    WAV_ERR_NO_FMT,         /* ran out of header without a fmt chunk */
    WAV_ERR_NO_DATA,        /* ran out of header without a data chunk */
    WAV_ERR_BAD_FMT,        /* fmt chunk malformed or nonsensical */
    WAV_ERR_UNSUPPORTED     /* codec we cannot play (e.g. ADPCM) */
} wav_err_t;

typedef enum {
    WAV_CODEC_PCM   = 1,
    WAV_CODEC_FLOAT = 3
} wav_codec_t;

typedef struct {
    wav_codec_t codec;
    uint16_t    channels;
    uint32_t    sample_rate;
    uint16_t    bits_per_sample;
    uint16_t    block_align;   /* bytes per frame (all channels) */
    uint32_t    data_offset;   /* byte offset of first sample in the file */
    uint32_t    data_bytes;    /* length of the data chunk */
    uint32_t    total_frames;  /* data_bytes / block_align */
} wav_info_t;

/*
 * Parse `len` bytes from the start of a WAV file. `len` only needs to cover
 * the header up to and including the `data` chunk header (typically < 1 KiB;
 * 4 KiB is a safe read for anything with embedded art or INFO tags).
 */
wav_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *out);

const char *wav_err_str(wav_err_t e);

/* Duration helpers — the UI needs these for the progress bar. */
uint32_t wav_duration_ms(const wav_info_t *i);
/* Byte offset to seek to for a given millisecond position, frame-aligned. */
uint32_t wav_ms_to_offset(const wav_info_t *i, uint32_t ms);

#endif /* WAV_H */
