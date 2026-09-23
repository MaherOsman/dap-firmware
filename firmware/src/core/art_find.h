/*
 * art_find — work out where a track's album art is, without decoding it.
 *
 * Two places art lives in a real library, both handled:
 *
 *   Folder images   cover / folder / front / album / albumart, as .jpg,
 *                   .jpeg or .png, in the track's folder. If the track sits
 *                   in a "Disc 1" / "CD2" style subfolder, the album folder
 *                   above it is checked too — that is where rippers put it.
 *
 *   Embedded art    a FLAC PICTURE block, or an ID3v2 APIC/PIC frame in an
 *                   MP3 (ID3 v2.2, v2.3 and v2.4). A front-cover picture is
 *                   preferred when a file carries several.
 *
 * Every candidate is identified by its first bytes, not its file name — a
 * "cover.jpg" that is secretly a PNG gets called a PNG. JPEGs are checked
 * one step further, to baseline vs progressive, because the decoder only
 * does baseline.
 *
 * The answer is a reference: a file, and a byte span inside it. Folder
 * images are the whole file; embedded art is the span of picture data
 * inside the audio file. art_open() turns a reference into the byte source
 * art_decode_jpeg() reads from.
 *
 * Through lib_io_t like the index, so all of it is tested on a PC.
 */
#ifndef ART_FIND_H
#define ART_FIND_H

#include <stdbool.h>
#include <stdint.h>

#include "art.h"
#include "lib_io.h"

#define ART_PATH_MAX 240

typedef enum {
    ART_FMT_NONE = 0,         /* nothing found */
    ART_FMT_JPEG,             /* baseline JPEG: decodable */
    ART_FMT_JPEG_PROGRESSIVE, /* JPEG the decoder cannot do */
    ART_FMT_JPEG_OTHER,       /* lossless / arithmetic / 12-bit JPEG */
    ART_FMT_PNG,
    ART_FMT_GIF,
    ART_FMT_BMP,
    ART_FMT_WEBP,
    ART_FMT_UNKNOWN           /* something, but not an image we recognise */
} art_fmt_t;

typedef enum {
    ART_FROM_NONE = 0,
    ART_FROM_FOLDER,
    ART_FROM_FLAC,
    ART_FROM_ID3
} art_from_t;

typedef struct {
    art_fmt_t  fmt;
    art_from_t from;
    char       path[ART_PATH_MAX + 1];
    uint32_t   offset;        /* where the image starts in `path` */
    uint32_t   length;        /* its size in bytes; 0 = to end of file */
} art_ref_t;

/*
 * Find art for the track at `track_path`.
 *
 * Returns true with `out` set to something the decoder can do. Preference:
 * a folder image first (one per album, usually the largest), then art
 * embedded in the track.
 *
 * Returns false when nothing decodable exists. `out` then describes the
 * best thing that was found anyway (a PNG, a progressive JPEG) so the log
 * can say why this album shows the placeholder — or has fmt ART_FMT_NONE
 * if there was no art at all.
 */
bool art_find(const lib_io_t *io, const char *track_path, art_ref_t *out);

/* Pieces of art_find, exposed for the tests. */
bool art_find_embedded(const lib_io_t *io, const char *path, art_ref_t *out);
art_fmt_t art_probe(const lib_io_t *io, void *fh, uint32_t offset,
                    uint32_t length);

/* --- reading a reference ------------------------------------------- */

/* The state behind an art_src_t that reads one reference. */
typedef struct {
    const lib_io_t *io;
    void     *fh;
    uint32_t  pos;            /* absolute file offset of the next byte */
    uint32_t  end;            /* one past the last byte; 0 = end of file */
    void    (*yield)(void *ctx);
    void     *yield_ctx;
} art_reader_t;

/* Opens `ref` and fills `src` to read exactly its bytes. `yield` (may be
 * NULL) is passed through to the decoder's yield hook. Returns false if
 * the file will not open. Always pair with art_close(). */
bool art_open(const lib_io_t *io, const art_ref_t *ref, art_reader_t *rd,
              art_src_t *src, void (*yield)(void *ctx), void *yield_ctx);
void art_close(art_reader_t *rd);

const char *art_fmt_name(art_fmt_t f);
const char *art_from_name(art_from_t f);

#endif /* ART_FIND_H */
