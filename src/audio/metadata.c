#include "metadata.h"

#include <taglib/tag_c.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */

/* ─────────────────────────────────────────────
   Parse bit depth directly from a FLAC file's
   STREAMINFO metadata block.

   FLAC binary layout (all big-endian):
     [0 –  3]  "fLaC" marker
     [4 –  7]  block header: is_last(1b) type(7b) length(24b)
     [8 – 41]  STREAMINFO (34 bytes):
                 min_block_size  16b
                 max_block_size  16b
                 min_frame_size  24b
                 max_frame_size  24b
                 sample_rate     20b
                 channels – 1    3b
                 bps – 1         5b   ← we want this
                 total_samples   36b
                 MD5            128b
───────────────────────────────────────────── */
static int read_flac_bit_depth(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    unsigned char buf[42];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (n < 42) return 0;
    if (buf[0]!='f'||buf[1]!='L'||buf[2]!='a'||buf[3]!='C') return 0;
    if ((buf[4] & 0x7F) != 0) return 0;   /* first block must be STREAMINFO */

    /* STREAMINFO starts at byte 8.
       bits_per_sample – 1 spans 1 bit in si[12] and 4 bits in si[13]. */
    const unsigned char *si = buf + 8;
    int bps_raw = ((si[12] & 0x01) << 4) | ((si[13] >> 4) & 0x0F);
    return bps_raw + 1;
}

/* Infer format string from file extension */
static void format_from_ext(const char *path, char *out, int out_sz)
{
    const char *ext = strrchr(path, '.');
    if (!ext) { snprintf(out, out_sz, "Unknown"); return; }
    ext++;   /* skip the dot */

    if      (strcasecmp(ext, "flac") == 0) snprintf(out, out_sz, "FLAC");
    else if (strcasecmp(ext, "mp3")  == 0) snprintf(out, out_sz, "MP3");
    else if (strcasecmp(ext, "wav")  == 0) snprintf(out, out_sz, "WAV");
    else if (strcasecmp(ext, "aiff") == 0) snprintf(out, out_sz, "AIFF");
    else if (strcasecmp(ext, "ogg")  == 0) snprintf(out, out_sz, "OGG");
    else snprintf(out, out_sz, "%s", ext);
}

/* ─────────────────────────────────────────────
   Public API
───────────────────────────────────────────── */
int metadata_read(const char *path, TrackMetadata *out)
{
    memset(out, 0, sizeof(*out));

    TagLib_File *file = taglib_file_new(path);
    if (!file || !taglib_file_is_valid(file)) {
        if (file) taglib_file_free(file);
        return 0;
    }

    TagLib_Tag *tag = taglib_file_tag(file);
    if (tag) {
        const char *s;

        s = taglib_tag_title(tag);
        snprintf(out->title,  sizeof(out->title),  "%s", s ? s : "");

        s = taglib_tag_artist(tag);
        snprintf(out->artist, sizeof(out->artist), "%s", s ? s : "");

        s = taglib_tag_album(tag);
        snprintf(out->album,  sizeof(out->album),  "%s", s ? s : "");

        unsigned int yr = taglib_tag_year(tag);
        if (yr > 0) snprintf(out->year, sizeof(out->year), "%u", yr);
    }

    const TagLib_AudioProperties *props = taglib_file_audioproperties(file);
    if (props) {
        out->duration_sec  = taglib_audioproperties_length(props);
        out->bitrate_kbps  = taglib_audioproperties_bitrate(props);
        out->sample_rate_hz = taglib_audioproperties_samplerate(props);
    }

    taglib_tag_free_strings();
    taglib_file_free(file);

    format_from_ext(path, out->file_format, sizeof(out->file_format));

    /* Bit depth: direct FLAC header parse for FLAC files */
    if (strcasecmp(out->file_format, "FLAC") == 0)
        out->bit_depth = read_flac_bit_depth(path);

    return 1;
}
