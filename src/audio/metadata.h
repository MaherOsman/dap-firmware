#ifndef METADATA_H
#define METADATA_H

/* ─────────────────────────────────────────────
   Metadata read from an audio file via TagLib.
   Bit depth is extracted by parsing the raw
   FLAC STREAMINFO block (TagLib C API does not
   expose it directly).
───────────────────────────────────────────── */
typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char year[16];
    char file_format[16];  /* "FLAC", "MP3", "WAV", … */

    int  bit_depth;        /* 16, 24, 32  (0 = unknown) */
    int  sample_rate_hz;   /* e.g. 44100, 96000          */
    int  bitrate_kbps;
    int  duration_sec;
} TrackMetadata;

/** Read tags and audio properties from path into *out.
 *  Returns 1 on success, 0 on failure.               */
int metadata_read(const char *path, TrackMetadata *out);

#endif /* METADATA_H */
