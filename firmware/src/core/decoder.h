/*
 * decoder — the format-agnostic seam between a byte source and the audio path.
 *
 * Above this line (platform_audio, player, ringbuf) nothing knows what a WAV
 * or a FLAC is; below it (decoder_wav, decoder_flac, decoder_mp3) nothing
 * knows what FatFs is.
 *
 * That second property is what makes FLAC testable: on the host the io
 * callbacks are backed by an in-memory array, on the STM32 by f_read/f_lseek,
 * and the decoder body is identical in both.
 */
#ifndef DECODER_H
#define DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECODER_MAX_HEADER  64      /* bytes probe() gets to identify a format */
#define DECODER_STATE_BYTES 65536   /* per-instance arena; sized for FLAC, measured not guessed */

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;   /* source depth; output is always Q1.31 */
    uint32_t total_frames;      /* 0 when the format does not say */
} decoder_info_t;

/* Byte source. `ctx` is opaque to the decoder. */
typedef struct {
    size_t   (*read)(void *ctx, void *dst, size_t n);  /* short read == EOF */
    bool     (*seek)(void *ctx, uint32_t offset);      /* absolute from start */
    uint32_t (*size)(void *ctx);                       /* 0 if unknown */
    void     *ctx;
} decoder_io_t;

typedef struct decoder decoder_t;

typedef struct {
    const char *name;
    bool   (*probe)(const uint8_t *header, size_t len);
    bool   (*open)(decoder_t *d, decoder_info_t *out);
    size_t (*decode)(decoder_t *d, int32_t *dst, size_t frames);
    bool   (*seek_frame)(decoder_t *d, uint32_t frame);
    void   (*close)(decoder_t *d);
} decoder_vtable_t;

struct decoder {
    const decoder_vtable_t *vt;
    decoder_io_t io;
    decoder_info_t info;
    bool is_open;
    union {
        uint8_t  bytes[DECODER_STATE_BYTES];
        uint64_t align;
    } state;
};

void decoder_register(const decoder_vtable_t *vt);
void decoder_registry_clear(void);
size_t decoder_registry_count(void);
const decoder_vtable_t *decoder_find(const uint8_t *header, size_t len);

bool   decoder_open(decoder_t *d, decoder_io_t io, decoder_info_t *out);
size_t decoder_decode(decoder_t *d, int32_t *dst, size_t frames);
bool   decoder_seek_frame(decoder_t *d, uint32_t frame);
void   decoder_close(decoder_t *d);
const char *decoder_name(const decoder_t *d);

extern const decoder_vtable_t decoder_wav_vt;
extern const decoder_vtable_t decoder_flac_vt;
extern const decoder_vtable_t decoder_mp3_vt;
size_t decoder_mp3_arena_peak(const decoder_t *d);

/* Peak bytes dr_flac took from the arena — for sizing DECODER_STATE_BYTES. */
size_t decoder_flac_arena_peak(const decoder_t *d);

#endif /* DECODER_H */
