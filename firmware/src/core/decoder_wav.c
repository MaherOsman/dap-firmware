/*
 * decoder_wav — WAV behind the decoder seam.
 *
 * Almost pure delegation: wav_parse() does the header work and the
 * audio_unpack_*() family does the sample conversion, both already tested.
 * What lives here is the streaming state — where we are in the data chunk,
 * and how many source bytes a request for N frames needs.
 *
 * WAV also serves as the control case for the whole seam: it costs no CPU to
 * decode, so if something sounds wrong while playing a WAV, the fault is in
 * the buffering or the hardware, never in the decoder.
 */
#include "decoder.h"
#include "wav.h"
#include "audio.h"
#include <string.h>

/* Carved out of decoder_t.state — must fit in DECODER_STATE_BYTES. */
#define WAV_SCRATCH 4096

typedef struct {
    wav_info_t info;
    uint32_t   pos;          /* byte offset within the data chunk */
    uint8_t    scratch[WAV_SCRATCH];
} wav_state_t;

static wav_state_t *S(decoder_t *d) { return (wav_state_t *)d->state.bytes; }

static bool wav_probe(const uint8_t *h, size_t len)
{
    /* 'RIFF' at 0 and 'WAVE' at 8 — enough to claim the file without
     * parsing it. A RIFF container holding something else (AVI) fails at 8. */
    if (len < 12) return false;
    return memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0;
}

static bool wav_open_impl(decoder_t *d, decoder_info_t *out)
{
    wav_state_t *st = S(d);
    memset(st, 0, sizeof *st);

    /* 4 KiB covers any sane header, including embedded art or INFO tags. */
    size_t n = d->io.read(d->io.ctx, st->scratch, WAV_SCRATCH);
    if (n < 12) return false;

    if (wav_parse(st->scratch, n, &st->info) != WAV_OK) return false;

    /* Only integer PCM at depths audio.h can unpack. Float and ADPCM are
     * reported by wav_parse but there is no unpacker for them. */
    if (st->info.codec != WAV_CODEC_PCM) return false;
    if (st->info.channels == 0 || st->info.channels > 2) return false;
    switch (st->info.bits_per_sample) {
        case 8: case 16: case 24: case 32: break;
        default: return false;
    }

    if (!d->io.seek(d->io.ctx, st->info.data_offset)) return false;
    st->pos = 0;

    out->sample_rate     = st->info.sample_rate;
    out->channels        = st->info.channels;
    out->bits_per_sample = st->info.bits_per_sample;
    out->total_frames    = st->info.total_frames;
    return true;
}

static size_t wav_decode(decoder_t *d, int32_t *dst, size_t frames)
{
    wav_state_t *st = S(d);
    const uint32_t bytes_per_frame = st->info.block_align;
    if (bytes_per_frame == 0) return 0;

    uint32_t remaining = st->info.data_bytes - st->pos;
    if (remaining == 0) return 0;

    /* Clamp the request to what the scratch holds and what the file has. */
    size_t max_frames = WAV_SCRATCH / bytes_per_frame;
    if (frames > max_frames) frames = max_frames;

    uint32_t want = (uint32_t)frames * bytes_per_frame;
    if (want > remaining) want = remaining - (remaining % bytes_per_frame);
    if (want == 0) return 0;

    size_t got = d->io.read(d->io.ctx, st->scratch, want);
    got -= got % bytes_per_frame;          /* never emit a partial frame */
    if (got == 0) return 0;
    st->pos += (uint32_t)got;

    size_t samples = got / (st->info.bits_per_sample / 8u);
    switch (st->info.bits_per_sample) {
        case 8:  audio_unpack_u8 (st->scratch, dst, samples); break;
        case 16: audio_unpack_s16(st->scratch, dst, samples); break;
        case 24: audio_unpack_s24(st->scratch, dst, samples); break;
        case 32: audio_unpack_s32(st->scratch, dst, samples); break;
        default: return 0;
    }

    size_t out_frames = samples / st->info.channels;

    /* The audio path is always stereo; mono files are widened here so
     * nothing downstream has to care about channel count. */
    if (st->info.channels == 1) audio_mono_to_stereo(dst, out_frames);

    return out_frames;
}

static bool wav_seek(decoder_t *d, uint32_t frame)
{
    wav_state_t *st = S(d);
    if (st->info.block_align == 0) return false;
    if (frame > st->info.total_frames) return false;

    uint32_t byte_off = frame * st->info.block_align;
    if (!d->io.seek(d->io.ctx, st->info.data_offset + byte_off)) return false;
    st->pos = byte_off;
    return true;
}

static void wav_close(decoder_t *d) { (void)d; }

const decoder_vtable_t decoder_wav_vt = {
    .name       = "wav",
    .probe      = wav_probe,
    .open       = wav_open_impl,
    .decode     = wav_decode,
    .seek_frame = wav_seek,
    .close      = wav_close,
};
