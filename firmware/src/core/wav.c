#include "wav.h"
#include <string.h>

/* WAV is little-endian regardless of host, so read explicitly. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool tag_is(const uint8_t *p, const char *t)
{
    return memcmp(p, t, 4) == 0;
}

const char *wav_err_str(wav_err_t e)
{
    switch (e) {
    case WAV_OK:              return "ok";
    case WAV_ERR_TOO_SHORT:   return "file too short";
    case WAV_ERR_NOT_RIFF:    return "not a RIFF file";
    case WAV_ERR_NOT_WAVE:    return "RIFF but not WAVE";
    case WAV_ERR_NO_FMT:      return "no fmt chunk";
    case WAV_ERR_NO_DATA:     return "no data chunk";
    case WAV_ERR_BAD_FMT:     return "malformed fmt chunk";
    case WAV_ERR_UNSUPPORTED: return "unsupported codec";
    }
    return "unknown";
}

wav_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *out)
{
    if (!buf || !out || len < 12) {
        return WAV_ERR_TOO_SHORT;
    }
    if (!tag_is(buf, "RIFF")) {
        return WAV_ERR_NOT_RIFF;
    }
    if (!tag_is(buf + 8, "WAVE")) {
        return WAV_ERR_NOT_WAVE;
    }

    memset(out, 0, sizeof(*out));
    bool have_fmt = false;

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t sz = rd32(buf + pos + 4);
        size_t body = pos + 8;

        if (tag_is(id, "fmt ")) {
            if (sz < 16 || body + 16 > len) {
                return WAV_ERR_BAD_FMT;
            }
            uint16_t fmt = rd16(buf + body);
            out->channels        = rd16(buf + body + 2);
            out->sample_rate     = rd32(buf + body + 4);
            out->block_align     = rd16(buf + body + 12);
            out->bits_per_sample = rd16(buf + body + 14);

            /* WAVE_FORMAT_EXTENSIBLE: the real codec is in the GUID's
             * first two bytes, at cbSize+SubFormat. */
            if (fmt == 0xFFFE) {
                if (sz < 40 || body + 26 > len) {
                    return WAV_ERR_BAD_FMT;
                }
                fmt = rd16(buf + body + 24);
            }
            if (fmt != WAV_CODEC_PCM && fmt != WAV_CODEC_FLOAT) {
                return WAV_ERR_UNSUPPORTED;
            }
            out->codec = (wav_codec_t)fmt;

            if (out->channels == 0 || out->channels > 2 ||
                out->sample_rate == 0 || out->bits_per_sample == 0 ||
                (out->bits_per_sample & 7) != 0) {
                return WAV_ERR_BAD_FMT;
            }
            /* Some encoders write a bogus block_align; recompute if needed. */
            uint16_t expect =
                (uint16_t)(out->channels * (out->bits_per_sample / 8));
            if (out->block_align != expect) {
                out->block_align = expect;
            }
            have_fmt = true;
        } else if (tag_is(id, "data")) {
            if (!have_fmt) {
                return WAV_ERR_NO_FMT;
            }
            out->data_offset = (uint32_t)body;
            out->data_bytes  = sz;
            out->total_frames =
                out->block_align ? sz / out->block_align : 0;
            return WAV_OK;
        }

        /* Chunks are word-aligned: an odd size is followed by a pad byte. */
        pos = body + sz + (sz & 1u);
        if (sz > len) {
            break; /* size field points past our buffer — stop walking */
        }
    }
    return have_fmt ? WAV_ERR_NO_DATA : WAV_ERR_NO_FMT;
}

uint32_t wav_duration_ms(const wav_info_t *i)
{
    if (!i || i->sample_rate == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)i->total_frames * 1000u) / i->sample_rate);
}

uint32_t wav_ms_to_offset(const wav_info_t *i, uint32_t ms)
{
    if (!i || i->sample_rate == 0 || i->block_align == 0) {
        return 0;
    }
    uint64_t frame = ((uint64_t)ms * i->sample_rate) / 1000u;
    if (frame > i->total_frames) {
        frame = i->total_frames;
    }
    return i->data_offset + (uint32_t)(frame * i->block_align);
}
