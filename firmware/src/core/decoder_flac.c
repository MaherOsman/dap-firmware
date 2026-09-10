/*
 * decoder_flac — FLAC behind the decoder seam, backed by dr_flac.
 *
 * Three things this adapter is responsible for, all of which are about
 * making a general-purpose desktop library behave on an MCU:
 *
 *   1. No heap. dr_flac allocates once on open; we hand it a bump allocator
 *      over the decoder's static arena. Nothing is ever freed mid-track, so
 *      a bump pointer is sufficient and cannot fragment.
 *   2. Position tracking. dr_flac wants read/seek/tell and supports relative
 *      seeks; decoder_io_t offers absolute seek only. We keep the cursor here
 *      and translate.
 *   3. Output shape. drflac_read_pcm_frames_s32 emits full-range int32, which
 *      is already our Q1.31 convention, so there is no conversion — but mono
 *      files still need widening to stereo like every other decoder.
 */
#include "decoder.h"
#include "audio.h"
#include <string.h>

#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#include "../../third_party/dr_flac.h"

typedef struct {
    decoder_t *owner;        /* back-pointer, so callbacks can reach io */
    uint32_t   cursor;       /* our own position, since io has no tell */
    uint32_t   file_size;
    drflac    *fl;

    /* Bump arena. dr_flac allocates once; peak is recorded for tuning. */
    size_t     arena_used;
    size_t     arena_peak;
    uint8_t    arena[DECODER_STATE_BYTES - 512];  /* rest is this struct */
} flac_state_t;

static flac_state_t *S(decoder_t *d) { return (flac_state_t *)d->state.bytes; }

/* ---- bump allocator ---------------------------------------------------- */

static void *arena_malloc(size_t sz, void *ud)
{
    flac_state_t *st = (flac_state_t *)ud;
    /* 8-byte align: dr_flac stores int32 and pointers in these blocks. */
    size_t aligned = (sz + 7u) & ~(size_t)7u;
    if (st->arena_used + aligned > sizeof st->arena) return NULL;
    void *p = st->arena + st->arena_used;
    st->arena_used += aligned;
    if (st->arena_used > st->arena_peak) st->arena_peak = st->arena_used;
    return p;
}

static void *arena_realloc(void *p, size_t sz, void *ud)
{
    /* dr_flac does not realloc during normal decoding. If it ever does,
     * a fresh block is correct-but-wasteful, which beats returning NULL. */
    void *n = arena_malloc(sz, ud);
    if (n && p) memcpy(n, p, sz);
    return n;
}

static void arena_free(void *p, void *ud)
{
    (void)p; (void)ud;   /* freed wholesale when the track closes */
}

/* ---- io bridge --------------------------------------------------------- */

static size_t flac_on_read(void *ud, void *dst, size_t n)
{
    flac_state_t *st = (flac_state_t *)ud;
    size_t got = st->owner->io.read(st->owner->io.ctx, dst, n);
    st->cursor += (uint32_t)got;
    return got;
}

static drflac_bool32 flac_on_seek(void *ud, int offset, drflac_seek_origin org)
{
    flac_state_t *st = (flac_state_t *)ud;
    int64_t target;

    switch (org) {
        case DRFLAC_SEEK_SET: target = offset; break;
        case DRFLAC_SEEK_CUR: target = (int64_t)st->cursor + offset; break;
        case DRFLAC_SEEK_END: target = (int64_t)st->file_size + offset; break;
        default: return DRFLAC_FALSE;
    }

    /* dr_flac deliberately seeks past EOF while hunting for frames and
     * expects a clean failure, not a clamp. */
    if (target < 0 || target > (int64_t)st->file_size) return DRFLAC_FALSE;

    if (!st->owner->io.seek(st->owner->io.ctx, (uint32_t)target))
        return DRFLAC_FALSE;
    st->cursor = (uint32_t)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_on_tell(void *ud, drflac_int64 *cursor)
{
    flac_state_t *st = (flac_state_t *)ud;
    *cursor = (drflac_int64)st->cursor;
    return DRFLAC_TRUE;
}

/* ---- vtable ------------------------------------------------------------ */

static bool flac_probe(const uint8_t *h, size_t len)
{
    /* Native FLAC starts with the fLaC marker. An ID3v2 tag may precede it
     * on files tagged by some editors, so accept that too and let dr_flac
     * skip the tag itself. */
    if (len >= 4 && memcmp(h, "fLaC", 4) == 0) return true;
    if (len >= 10 && memcmp(h, "ID3", 3) == 0) return true;
    return false;
}

static bool flac_open_impl(decoder_t *d, decoder_info_t *out)
{
    flac_state_t *st = S(d);
    memset(st, 0, sizeof *st);
    st->owner = d;
    st->cursor = 0;
    st->file_size = d->io.size ? d->io.size(d->io.ctx) : 0xFFFFFFFFu;

    drflac_allocation_callbacks cb;
    cb.pUserData = st;
    cb.onMalloc  = arena_malloc;
    cb.onRealloc = arena_realloc;
    cb.onFree    = arena_free;

    st->fl = drflac_open(flac_on_read, flac_on_seek, flac_on_tell, st, &cb);
    if (!st->fl) return false;

    if (st->fl->channels == 0 || st->fl->channels > 2) {
        drflac_close(st->fl);
        st->fl = NULL;
        return false;
    }

    out->sample_rate     = st->fl->sampleRate;
    out->channels        = (uint16_t)st->fl->channels;
    out->bits_per_sample = (uint16_t)st->fl->bitsPerSample;
    out->total_frames    = (uint32_t)st->fl->totalPCMFrameCount;
    return true;
}

static size_t flac_decode(decoder_t *d, int32_t *dst, size_t frames)
{
    flac_state_t *st = S(d);
    if (!st->fl) return 0;

    /* int32_t and drflac_int32 are both 32-bit but are spelled differently
         * by different toolchains (int vs long int on ARM), so C rejects the
         * pointer conversion even though the layout is identical. The assert
         * below is what makes the cast safe rather than hopeful. */
        _Static_assert(sizeof(drflac_int32) == sizeof(int32_t),
                       "drflac_int32 must match int32_t");
        drflac_uint64 got = drflac_read_pcm_frames_s32(st->fl,
                                                       (drflac_uint64)frames,
                                                       (drflac_int32 *)dst);
    if (got == 0) return 0;

    if (st->fl->channels == 1) audio_mono_to_stereo(dst, (size_t)got);
    return (size_t)got;
}

static bool flac_seek(decoder_t *d, uint32_t frame)
{
    flac_state_t *st = S(d);
    if (!st->fl) return false;
    return drflac_seek_to_pcm_frame(st->fl, frame) == DRFLAC_TRUE;
}

static void flac_close(decoder_t *d)
{
    flac_state_t *st = S(d);
    if (st->fl) { drflac_close(st->fl); st->fl = NULL; }
}

size_t decoder_flac_arena_peak(const decoder_t *d)
{
    return ((const flac_state_t *)d->state.bytes)->arena_peak;
}

const decoder_vtable_t decoder_flac_vt = {
    .name       = "flac",
    .probe      = flac_probe,
    .open       = flac_open_impl,
    .decode     = flac_decode,
    .seek_frame = flac_seek,
    .close      = flac_close,
};
