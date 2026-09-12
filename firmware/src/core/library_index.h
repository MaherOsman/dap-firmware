/* library_index.h — on-card binary music index.
 *
 * Layout goal: 100 GB / ~3000 tracks with no boot lag.
 *   resident  : header + artist table + album table + name string pool
 *               (~450 names => ~20 KB for a 3000-track card)
 *   paged     : track records, read LIB_PAGE_TRACKS at a time into a
 *               2.5 KB cache inside the caller's arena
 *
 * FILE FORMAT (all integers little-endian, written/read byte-by-byte so the
 * format does not depend on host endianness or struct padding).
 *
 * Header, 64 bytes at offset 0:
 *   off size field
 *    0    8  magic "DAPIDX10"
 *    8    4  version                 (LIB_IDX_VERSION)
 *   12    4  artist_count
 *   16    4  album_count
 *   20    4  track_count
 *   24    4  artist_off              byte offset of artist table
 *   28    4  album_off
 *   32    4  strpool_off
 *   36    4  strpool_len             bytes, last byte must be 0
 *   40    4  track_off
 *   44    4  track_rec_size          must equal LIB_TRACK_REC_SIZE
 *   48    4  build_id                bumped by the builder; used to detect
 *                                    a stale index without rescanning
 *   52    4  total_size              expected file size in bytes
 *   56    8  reserved (zero)
 *
 * Artist record, 16 bytes:
 *    0    4  name_off   offset into string pool
 *    4    4  album_first index of first album belonging to this artist
 *    8    4  album_count
 *   12    4  reserved
 *   Albums are stored grouped by artist, so [album_first, +album_count) is
 *   contiguous. Artists are sorted by name.
 *
 * Album record, 20 bytes:
 *    0    4  name_off
 *    4    4  artist_idx
 *    8    4  track_first index of first track record for this album
 *   12    4  track_count
 *   16    4  reserved
 *   Tracks are stored grouped by album, sorted by (track_no, title).
 *
 * Track record, 320 bytes (fixed size => O(1) seek to any track):
 *    0    4  album_idx
 *    4    4  duration_ms   0 if unknown
 *    8    4  file_size
 *   12    2  track_no      0 if unknown
 *   14    1  codec         LIB_CODEC_*
 *   15    1  flags
 *   16   16  reserved (zero)
 *   32   96  title, NUL-padded, not necessarily NUL-terminated when full
 *  128  192  path,  NUL-padded, absolute path on the card
 */
#ifndef LIBRARY_INDEX_H
#define LIBRARY_INDEX_H

#include <stdint.h>
#include "lib_io.h"

#define LIB_IDX_MAGIC        "DAPIDX10"   /* 8 bytes, no NUL stored */
#define LIB_IDX_MAGIC_LEN    8u
#define LIB_IDX_VERSION      1u

#define LIB_HDR_SIZE         64u
#define LIB_ARTIST_REC_SIZE  16u
#define LIB_ALBUM_REC_SIZE   20u
#define LIB_TRACK_REC_SIZE   320u

#define LIB_TITLE_MAX        96u   /* bytes stored on card */
#define LIB_PATH_MAX         192u

#define LIB_PAGE_TRACKS      8u    /* track records per cached page */

#define LIB_INDEX_PATH       "/dap.idx"

enum {
    LIB_CODEC_UNKNOWN = 0,
    LIB_CODEC_WAV     = 1,
    LIB_CODEC_FLAC    = 2,
    LIB_CODEC_MP3     = 3
};

enum {
    LIB_OK        =  0,
    LIB_E_IO      = -1,   /* backend reported failure or short read */
    LIB_E_FORMAT  = -2,   /* magic/version/size/bounds check failed */
    LIB_E_NOMEM   = -3,   /* arena too small; see libidx_t.required_bytes */
    LIB_E_RANGE   = -4,   /* index out of range */
    LIB_E_ARG     = -5
};

typedef struct {
    uint32_t version;
    uint32_t artist_count;
    uint32_t album_count;
    uint32_t track_count;
    uint32_t artist_off;
    uint32_t album_off;
    uint32_t strpool_off;
    uint32_t strpool_len;
    uint32_t track_off;
    uint32_t track_rec_size;
    uint32_t build_id;
    uint32_t total_size;
} lib_header_t;

typedef struct {
    uint32_t name_off;
    uint32_t album_first;
    uint32_t album_count;
} lib_artist_t;

typedef struct {
    uint32_t name_off;
    uint32_t artist_idx;
    uint32_t track_first;
    uint32_t track_count;
} lib_album_t;

/* Decoded track record. Strings are always NUL-terminated here even when the
 * on-card field is full, hence the +1. */
typedef struct {
    uint32_t album_idx;
    uint32_t duration_ms;
    uint32_t file_size;
    uint16_t track_no;
    uint8_t  codec;
    uint8_t  flags;
    char     title[LIB_TITLE_MAX + 1];
    char     path[LIB_PATH_MAX + 1];
} lib_track_t;

typedef struct {
    const lib_io_t *io;
    void           *fh;
    lib_header_t    hdr;

    lib_artist_t   *artists;   /* into arena */
    lib_album_t    *albums;    /* into arena */
    char           *strpool;   /* into arena */
    uint8_t        *page;      /* into arena, LIB_PAGE_TRACKS * 320 bytes */

    uint32_t        page_first; /* global track index of page[0] */
    uint32_t        page_n;     /* valid records in page, 0 = empty */

    uint32_t        required_bytes; /* set on LIB_E_NOMEM */
    int             is_open;
} libidx_t;

/* --- little-endian byte helpers (shared with the builder and the tests) --- */
uint16_t lib_rd_u16(const uint8_t *p);
uint32_t lib_rd_u32(const uint8_t *p);
void     lib_wr_u16(uint8_t *p, uint16_t v);
void     lib_wr_u32(uint8_t *p, uint32_t v);

/* Decode/encode a 320-byte track record. `rec` must be LIB_TRACK_REC_SIZE. */
void lib_decode_track(const uint8_t *rec, lib_track_t *out);
void lib_encode_track(uint8_t *rec, const lib_track_t *in);

/* Decode a 64-byte header buffer. Returns LIB_OK or LIB_E_FORMAT. */
int  lib_decode_header(const uint8_t *buf, lib_header_t *out);
void lib_encode_header(uint8_t *buf, const lib_header_t *in);

/* Arena bytes needed to open an index with this header. */
uint32_t libidx_arena_bytes(const lib_header_t *h);

/* Opens `path`, validates the header, loads the resident tables into `arena`.
 * `arena` must be 4-byte aligned. On LIB_E_NOMEM, lib->required_bytes says how
 * much is needed and the file is left closed. */
int  libidx_open(libidx_t *lib, const lib_io_t *io, const char *path,
                  void *arena, uint32_t arena_len);
void libidx_close(libidx_t *lib);

uint32_t    libidx_artist_count(const libidx_t *lib);
const char *libidx_artist_name(const libidx_t *lib, uint32_t artist);
uint32_t    libidx_album_count(const libidx_t *lib);                    /* total */
uint32_t    libidx_artist_album_count(const libidx_t *lib, uint32_t artist);
/* Album n of an artist -> global album index, or UINT32_MAX if out of range. */
uint32_t    libidx_artist_album(const libidx_t *lib, uint32_t artist, uint32_t n);
const char *libidx_album_name(const libidx_t *lib, uint32_t album);
uint32_t    libidx_album_artist(const libidx_t *lib, uint32_t album);
uint32_t    libidx_album_track_count(const libidx_t *lib, uint32_t album);
uint32_t    libidx_track_count(const libidx_t *lib);                    /* total */

/* Paged reads. Both hit the page cache; a cache hit does no I/O. */
int libidx_track_global(libidx_t *lib, uint32_t track, lib_track_t *out);
int libidx_album_track(libidx_t *lib, uint32_t album, uint32_t n, lib_track_t *out);

#endif /* LIBRARY_INDEX_H */
