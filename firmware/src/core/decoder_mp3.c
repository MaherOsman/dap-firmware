/*
 * decoder_mp3 — MP3 behind the decoder seam, backed by dr_mp3.
 *
 * Differs from decoder_flac in two ways: drmp3 is a caller-owned struct
 * rather than a returned pointer, and dr_mp3 has no s32 output, so s16 is
 * decoded into scratch and shifted up to Q1.31 here.
 */
#include "decoder.h"
#include "audio.h"
#include <string.h>

#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD
#define DRMP3_DATA_CHUNK_SIZE 16384   /* default is 65536 — desktop sizing */
#include "../../third_party/dr_mp3.h"
#define MP3_SCRATCH_FRAMES 1152

typedef struct {
    decoder_t *owner;
    uint32_t   cursor;
    uint32_t   file_size;
    bool       started;
    drmp3      mp3;
    int16_t    scratch[MP3_SCRATCH_FRAMES * 2];
    size_t     arena_used;
    size_t     arena_peak;
    void      *last_block;
    uint8_t    arena[24576];
} mp3_state_t;

_Static_assert(sizeof(mp3_state_t) <= DECODER_STATE_BYTES,
               "mp3_state_t must fit in the decoder arena");

static mp3_state_t *S(decoder_t *d) { return (mp3_state_t *)d->state.bytes; }

static void *arena_malloc(size_t sz, void *ud)
{
    mp3_state_t *st = (mp3_state_t *)ud;
    size_t aligned = (sz + 7u) & ~(size_t)7u;
    if (st->arena_used + aligned > sizeof st->arena) return NULL;
    void *p = st->arena + st->arena_used;
    st->arena_used += aligned;
    if (st->arena_used > st->arena_peak) st->arena_peak = st->arena_used;
        st->last_block = p;
    return p;
}

static void *arena_realloc(void *p, size_t sz, void *ud)
{
    mp3_state_t *st = (mp3_state_t *)ud;
    size_t aligned = (sz + 7u) & ~(size_t)7u;

    /* dr_mp3 grows its read buffer as it goes, and the block being grown
     * is always the most recent allocation. Extending in place costs
     * nothing and avoids stranding the old one — with a bump allocator
     * that never frees, copying instead exhausts the arena partway
     * through a track and playback stops. */
    if (p != NULL && st->last_block == p) {
        size_t base = (size_t)((uint8_t *)p - st->arena);
        if (base + aligned > sizeof st->arena) return NULL;
        st->arena_used = base + aligned;
        if (st->arena_used > st->arena_peak) st->arena_peak = st->arena_used;
        return p;
    }

    void *n = arena_malloc(sz, ud);
    if (n && p) memcpy(n, p, sz);
    return n;
}

static void arena_free(void *p, void *ud) { (void)p; (void)ud; }

static size_t mp3_on_read(void *ud, void *dst, size_t n)
{
    mp3_state_t *st = (mp3_state_t *)ud;
    size_t got = st->owner->io.read(st->owner->io.ctx, dst, n);
    st->cursor += (uint32_t)got;
    return got;
}

static drmp3_bool32 mp3_on_seek(void *ud, int offset, drmp3_seek_origin org)
{
    mp3_state_t *st = (mp3_state_t *)ud;
    int64_t target;
    switch (org) {
        case DRMP3_SEEK_SET: target = offset; break;
        case DRMP3_SEEK_CUR: target = (int64_t)st->cursor + offset; break;
        case DRMP3_SEEK_END: target = (int64_t)st->file_size + offset; break;
        default: return DRMP3_FALSE;
    }
    if (target < 0 || target > (int64_t)st->file_size) return DRMP3_FALSE;
    if (!st->owner->io.seek(st->owner->io.ctx, (uint32_t)target))
        return DRMP3_FALSE;
    st->cursor = (uint32_t)target;
    return DRMP3_TRUE;
}

static drmp3_bool32 mp3_on_tell(void *ud, drmp3_int64 *cursor)
{
    *cursor = (drmp3_int64)((mp3_state_t *)ud)->cursor;
    return DRMP3_TRUE;
}

static bool mp3_probe(const uint8_t *h, size_t len)
{
    /* ID3v2 tag, or a raw MPEG audio sync word (11 set bits) with a layer
     * field that is not the reserved value. Registered last, because this
     * pattern is far weaker than the fLaC or RIFF magic numbers. */
    if (len >= 3 && memcmp(h, "ID3", 3) == 0) return true;
    if (len >= 2 && h[0] == 0xFFu && (h[1] & 0xE0u) == 0xE0u
                 && (h[1] & 0x06u) != 0x00u) return true;
    return false;
}

static bool mp3_open_impl(decoder_t *d, decoder_info_t *out)
{
    mp3_state_t *st = S(d);
    memset(st, 0, sizeof *st);
    st->owner = d;
    st->file_size = d->io.size ? d->io.size(d->io.ctx) : 0xFFFFFFFFu;

    drmp3_allocation_callbacks cb;
    cb.pUserData = st;
    cb.onMalloc  = arena_malloc;
    cb.onRealloc = arena_realloc;
    cb.onFree    = arena_free;

    if (!drmp3_init(&st->mp3, mp3_on_read, mp3_on_seek, mp3_on_tell,
                    NULL, st, &cb))
        return false;
    st->started = true;

    if (st->mp3.channels == 0 || st->mp3.channels > 2) {
        drmp3_uninit(&st->mp3);
        st->started = false;
        return false;
    }

    out->sample_rate     = st->mp3.sampleRate;
    out->channels        = (uint16_t)st->mp3.channels;
    out->bits_per_sample = 16;
    out->total_frames    = (st->mp3.totalPCMFrameCount == DRMP3_UINT64_MAX)
                           ? 0u : (uint32_t)st->mp3.totalPCMFrameCount;
    return true;
}

static size_t mp3_decode(decoder_t *d, int32_t *dst, size_t frames)
{
    mp3_state_t *st = S(d);
    if (!st->started) return 0;
    if (frames > MP3_SCRATCH_FRAMES) frames = MP3_SCRATCH_FRAMES;

    drmp3_uint64 got = drmp3_read_pcm_frames_s16(&st->mp3,
                                                 (drmp3_uint64)frames,
                                                 st->scratch);
    if (got == 0) return 0;

    /* s16 -> Q1.31 left-justified, matching every other decoder's output. */
    size_t samples = (size_t)got * st->mp3.channels;
    for (size_t i = 0; i < samples; i++)
        dst[i] = (int32_t)((uint32_t)st->scratch[i] << 16);

    if (st->mp3.channels == 1) audio_mono_to_stereo(dst, (size_t)got);
    return (size_t)got;
}

static bool mp3_seek(decoder_t *d, uint32_t frame)
{
    mp3_state_t *st = S(d);
    if (!st->started) return false;
    return drmp3_seek_to_pcm_frame(&st->mp3, frame) == DRMP3_TRUE;
}

static void mp3_close(decoder_t *d)
{
    mp3_state_t *st = S(d);
    if (st->started) { drmp3_uninit(&st->mp3); st->started = false; }
}

size_t decoder_mp3_arena_peak(const decoder_t *d)
{
    return ((const mp3_state_t *)d->state.bytes)->arena_peak;
}

const decoder_vtable_t decoder_mp3_vt = {
    .name       = "mp3",
    .probe      = mp3_probe,
    .open       = mp3_open_impl,
    .decode     = mp3_decode,
    .seek_frame = mp3_seek,
    .close      = mp3_close,
};
