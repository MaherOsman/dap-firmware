/* library_index.c — reader for the on-card binary index. No HAL, no stdio. */

#include "library_index.h"

#include <string.h>

#define PAGE_BYTES (LIB_PAGE_TRACKS * LIB_TRACK_REC_SIZE)
#define ALIGN4(x)  (((x) + 3u) & ~3u)
#define NO_PAGE    0xFFFFFFFFu

/* ------------------------------------------------------------------ bytes */

uint16_t lib_rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t lib_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void lib_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

void lib_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Copy a fixed-width, NUL-padded on-card string field into a NUL-terminated
 * C string of cap+1 bytes. */
static void field_to_cstr(char *dst, const uint8_t *src, uint32_t cap)
{
    uint32_t i;
    for (i = 0; i < cap && src[i] != 0; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

/* Copy a C string into a fixed-width, NUL-padded on-card string field.
 * Truncates silently at cap bytes; the builder is responsible for shortening
 * names before they get here if truncation would be visible. */
static void cstr_to_field(uint8_t *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (src) {
        while (i < cap && src[i] != '\0') {
            dst[i] = (uint8_t)src[i];
            i++;
        }
    }
    while (i < cap) {
        dst[i++] = 0;
    }
}

/* ------------------------------------------------------------- records */

void lib_decode_track(const uint8_t *rec, lib_track_t *out)
{
    out->album_idx   = lib_rd_u32(rec + 0);
    out->duration_ms = lib_rd_u32(rec + 4);
    out->file_size   = lib_rd_u32(rec + 8);
    out->track_no    = lib_rd_u16(rec + 12);
    out->codec       = rec[14];
    out->flags       = rec[15];
    field_to_cstr(out->title, rec + 32,  LIB_TITLE_MAX);
    field_to_cstr(out->path,  rec + 128, LIB_PATH_MAX);
}

void lib_encode_track(uint8_t *rec, const lib_track_t *in)
{
    memset(rec, 0, LIB_TRACK_REC_SIZE);
    lib_wr_u32(rec + 0,  in->album_idx);
    lib_wr_u32(rec + 4,  in->duration_ms);
    lib_wr_u32(rec + 8,  in->file_size);
    lib_wr_u16(rec + 12, in->track_no);
    rec[14] = in->codec;
    rec[15] = in->flags;
    cstr_to_field(rec + 32,  in->title, LIB_TITLE_MAX);
    cstr_to_field(rec + 128, in->path,  LIB_PATH_MAX);
}

void lib_encode_header(uint8_t *buf, const lib_header_t *in)
{
    memset(buf, 0, LIB_HDR_SIZE);
    memcpy(buf, LIB_IDX_MAGIC, LIB_IDX_MAGIC_LEN);
    lib_wr_u32(buf + 8,  in->version);
    lib_wr_u32(buf + 12, in->artist_count);
    lib_wr_u32(buf + 16, in->album_count);
    lib_wr_u32(buf + 20, in->track_count);
    lib_wr_u32(buf + 24, in->artist_off);
    lib_wr_u32(buf + 28, in->album_off);
    lib_wr_u32(buf + 32, in->strpool_off);
    lib_wr_u32(buf + 36, in->strpool_len);
    lib_wr_u32(buf + 40, in->track_off);
    lib_wr_u32(buf + 44, in->track_rec_size);
    lib_wr_u32(buf + 48, in->build_id);
    lib_wr_u32(buf + 52, in->total_size);
}

int lib_decode_header(const uint8_t *buf, lib_header_t *out)
{
    uint32_t end;

    if (memcmp(buf, LIB_IDX_MAGIC, LIB_IDX_MAGIC_LEN) != 0) {
        return LIB_E_FORMAT;
    }

    out->version        = lib_rd_u32(buf + 8);
    out->artist_count   = lib_rd_u32(buf + 12);
    out->album_count    = lib_rd_u32(buf + 16);
    out->track_count    = lib_rd_u32(buf + 20);
    out->artist_off     = lib_rd_u32(buf + 24);
    out->album_off      = lib_rd_u32(buf + 28);
    out->strpool_off    = lib_rd_u32(buf + 32);
    out->strpool_len    = lib_rd_u32(buf + 36);
    out->track_off      = lib_rd_u32(buf + 40);
    out->track_rec_size = lib_rd_u32(buf + 44);
    out->build_id       = lib_rd_u32(buf + 48);
    out->total_size     = lib_rd_u32(buf + 52);

    if (out->version != LIB_IDX_VERSION) {
        return LIB_E_FORMAT;
    }
    if (out->track_rec_size != LIB_TRACK_REC_SIZE) {
        return LIB_E_FORMAT;
    }
    /* An empty library is legal, but a pool with no terminating NUL is not. */
    if (out->strpool_len == 0u && (out->artist_count || out->album_count)) {
        return LIB_E_FORMAT;
    }
    /* Overflow-safe section bounds against total_size. */
    if (out->artist_off < LIB_HDR_SIZE ||
        out->album_off  < LIB_HDR_SIZE ||
        out->strpool_off < LIB_HDR_SIZE ||
        out->track_off  < LIB_HDR_SIZE) {
        return LIB_E_FORMAT;
    }
    if (out->artist_count > 0xFFFFFFu || out->album_count > 0xFFFFFFu ||
        out->track_count > 0xFFFFFFu  || out->strpool_len > 0xFFFFFFu) {
        return LIB_E_FORMAT;
    }

    end = out->artist_off + out->artist_count * LIB_ARTIST_REC_SIZE;
    if (end > out->total_size) return LIB_E_FORMAT;
    end = out->album_off + out->album_count * LIB_ALBUM_REC_SIZE;
    if (end > out->total_size) return LIB_E_FORMAT;
    end = out->strpool_off + out->strpool_len;
    if (end > out->total_size) return LIB_E_FORMAT;
    end = out->track_off + out->track_count * LIB_TRACK_REC_SIZE;
    if (end > out->total_size) return LIB_E_FORMAT;

    return LIB_OK;
}

/* ---------------------------------------------------------------- arena */

uint32_t libidx_arena_bytes(const lib_header_t *h)
{
    return ALIGN4(h->artist_count * (uint32_t)sizeof(lib_artist_t))
         + ALIGN4(h->album_count  * (uint32_t)sizeof(lib_album_t))
         + ALIGN4(h->strpool_len)
         + PAGE_BYTES;
}

/* ------------------------------------------------------------------ io */

static int read_exact(libidx_t *lib, uint32_t off, void *dst, uint32_t len)
{
    uint32_t got = 0;
    int rc;

    if (len == 0u) return LIB_OK;

    rc = lib->io->seek(lib->io->ctx, lib->fh, off);
    if (rc != 0) return LIB_E_IO;

    rc = lib->io->read(lib->io->ctx, lib->fh, dst, len, &got);
    if (rc != 0 || got != len) return LIB_E_IO;

    return LIB_OK;
}

/* Loads a table by streaming disk records through the page buffer and
 * decoding them, so no second scratch allocation is needed. */
static int load_table(libidx_t *lib, uint32_t off, uint32_t count,
                      uint32_t rec_size, int is_artist)
{
    uint32_t per_batch = PAGE_BYTES / rec_size;
    uint32_t done = 0;

    while (done < count) {
        uint32_t n = count - done;
        uint32_t i;
        if (n > per_batch) n = per_batch;

        if (read_exact(lib, off + done * rec_size, lib->page, n * rec_size) != LIB_OK) {
            return LIB_E_IO;
        }

        for (i = 0; i < n; i++) {
            const uint8_t *p = lib->page + i * rec_size;
            if (is_artist) {
                lib_artist_t *a = &lib->artists[done + i];
                a->name_off    = lib_rd_u32(p + 0);
                a->album_first = lib_rd_u32(p + 4);
                a->album_count = lib_rd_u32(p + 8);
                if (a->name_off >= lib->hdr.strpool_len) return LIB_E_FORMAT;
                if (a->album_count > lib->hdr.album_count) return LIB_E_FORMAT;
                if (a->album_first + a->album_count > lib->hdr.album_count) {
                    return LIB_E_FORMAT;
                }
            } else {
                lib_album_t *b = &lib->albums[done + i];
                b->name_off    = lib_rd_u32(p + 0);
                b->artist_idx  = lib_rd_u32(p + 4);
                b->track_first = lib_rd_u32(p + 8);
                b->track_count = lib_rd_u32(p + 12);
                if (b->name_off >= lib->hdr.strpool_len) return LIB_E_FORMAT;
                if (b->artist_idx >= lib->hdr.artist_count) return LIB_E_FORMAT;
                if (b->track_count > lib->hdr.track_count) return LIB_E_FORMAT;
                if (b->track_first + b->track_count > lib->hdr.track_count) {
                    return LIB_E_FORMAT;
                }
            }
        }
        done += n;
    }
    return LIB_OK;
}

int libidx_open(libidx_t *lib, const lib_io_t *io, const char *path,
                 void *arena, uint32_t arena_len)
{
    uint8_t hdr_buf[LIB_HDR_SIZE];
    uint8_t *cur;
    uint32_t need;
    int rc;

    if (!lib || !io || !io->open || !io->read || !io->seek || !io->close ||
        !path || !arena) {
        return LIB_E_ARG;
    }

    memset(lib, 0, sizeof(*lib));
    lib->io = io;
    lib->page_first = NO_PAGE;

    if (io->open(io->ctx, path, LIB_IO_READ, &lib->fh) != 0) {
        lib->fh = NULL;
        return LIB_E_IO;
    }

    rc = read_exact(lib, 0, hdr_buf, LIB_HDR_SIZE);
    if (rc != LIB_OK) goto fail;

    rc = lib_decode_header(hdr_buf, &lib->hdr);
    if (rc != LIB_OK) goto fail;

    need = libidx_arena_bytes(&lib->hdr);
    if (need > arena_len) {
        lib->required_bytes = need;
        rc = LIB_E_NOMEM;
        goto fail;
    }

    /* Carve the arena: artists, albums, pool, page cache. */
    cur = (uint8_t *)arena;
    lib->artists = (lib_artist_t *)(void *)cur;
    cur += ALIGN4(lib->hdr.artist_count * (uint32_t)sizeof(lib_artist_t));
    lib->albums = (lib_album_t *)(void *)cur;
    cur += ALIGN4(lib->hdr.album_count * (uint32_t)sizeof(lib_album_t));
    lib->strpool = (char *)cur;
    cur += ALIGN4(lib->hdr.strpool_len);
    lib->page = cur;

    if (lib->hdr.strpool_len > 0u) {
        rc = read_exact(lib, lib->hdr.strpool_off, lib->strpool,
                        lib->hdr.strpool_len);
        if (rc != LIB_OK) goto fail;
        /* Every name lookup relies on this: the pool must end in a NUL, so a
         * corrupt name_off can never walk off the end of the buffer. */
        if (lib->strpool[lib->hdr.strpool_len - 1u] != '\0') {
            rc = LIB_E_FORMAT;
            goto fail;
        }
    }

    rc = load_table(lib, lib->hdr.artist_off, lib->hdr.artist_count,
                    LIB_ARTIST_REC_SIZE, 1);
    if (rc != LIB_OK) goto fail;

    rc = load_table(lib, lib->hdr.album_off, lib->hdr.album_count,
                    LIB_ALBUM_REC_SIZE, 0);
    if (rc != LIB_OK) goto fail;

    lib->is_open = 1;
    return LIB_OK;

fail:
    if (lib->fh) {
        io->close(io->ctx, lib->fh);
        lib->fh = NULL;
    }
    lib->is_open = 0;
    return rc;
}

void libidx_close(libidx_t *lib)
{
    if (lib && lib->fh && lib->io) {
        lib->io->close(lib->io->ctx, lib->fh);
    }
    if (lib) {
        lib->fh = NULL;
        lib->is_open = 0;
        lib->page_first = NO_PAGE;
        lib->page_n = 0;
    }
}

/* ----------------------------------------------------------- accessors */

uint32_t libidx_artist_count(const libidx_t *lib)
{
    return (lib && lib->is_open) ? lib->hdr.artist_count : 0u;
}

uint32_t libidx_album_count(const libidx_t *lib)
{
    return (lib && lib->is_open) ? lib->hdr.album_count : 0u;
}

uint32_t libidx_track_count(const libidx_t *lib)
{
    return (lib && lib->is_open) ? lib->hdr.track_count : 0u;
}

const char *libidx_artist_name(const libidx_t *lib, uint32_t artist)
{
    if (!lib || !lib->is_open || artist >= lib->hdr.artist_count) return "";
    return lib->strpool + lib->artists[artist].name_off;
}

uint32_t libidx_artist_album_count(const libidx_t *lib, uint32_t artist)
{
    if (!lib || !lib->is_open || artist >= lib->hdr.artist_count) return 0u;
    return lib->artists[artist].album_count;
}

uint32_t libidx_artist_album(const libidx_t *lib, uint32_t artist, uint32_t n)
{
    if (!lib || !lib->is_open || artist >= lib->hdr.artist_count) {
        return 0xFFFFFFFFu;
    }
    if (n >= lib->artists[artist].album_count) return 0xFFFFFFFFu;
    return lib->artists[artist].album_first + n;
}

const char *libidx_album_name(const libidx_t *lib, uint32_t album)
{
    if (!lib || !lib->is_open || album >= lib->hdr.album_count) return "";
    return lib->strpool + lib->albums[album].name_off;
}

uint32_t libidx_album_artist(const libidx_t *lib, uint32_t album)
{
    if (!lib || !lib->is_open || album >= lib->hdr.album_count) {
        return 0xFFFFFFFFu;
    }
    return lib->albums[album].artist_idx;
}

uint32_t libidx_album_track_count(const libidx_t *lib, uint32_t album)
{
    if (!lib || !lib->is_open || album >= lib->hdr.album_count) return 0u;
    return lib->albums[album].track_count;
}

/* ----------------------------------------------------------- paged read */

int libidx_track_global(libidx_t *lib, uint32_t track, lib_track_t *out)
{
    uint32_t first, want, avail, bytes;

    if (!lib || !lib->is_open || !out) return LIB_E_ARG;
    if (track >= lib->hdr.track_count) return LIB_E_RANGE;

    if (lib->page_first != NO_PAGE &&
        track >= lib->page_first &&
        track <  lib->page_first + lib->page_n) {
        lib_decode_track(lib->page + (track - lib->page_first) * LIB_TRACK_REC_SIZE,
                         out);
        return LIB_OK;
    }

    first = (track / LIB_PAGE_TRACKS) * LIB_PAGE_TRACKS;
    avail = lib->hdr.track_count - first;
    want  = (avail < LIB_PAGE_TRACKS) ? avail : LIB_PAGE_TRACKS;
    bytes = want * LIB_TRACK_REC_SIZE;

    /* Invalidate before the read so a failed read can't leave a half-filled
     * page looking valid. */
    lib->page_first = NO_PAGE;
    lib->page_n = 0;

    if (read_exact(lib, lib->hdr.track_off + first * LIB_TRACK_REC_SIZE,
                   lib->page, bytes) != LIB_OK) {
        return LIB_E_IO;
    }

    lib->page_first = first;
    lib->page_n = want;

    lib_decode_track(lib->page + (track - first) * LIB_TRACK_REC_SIZE, out);
    return LIB_OK;
}

int libidx_album_track(libidx_t *lib, uint32_t album, uint32_t n,
                        lib_track_t *out)
{
    if (!lib || !lib->is_open || !out) return LIB_E_ARG;
    if (album >= lib->hdr.album_count) return LIB_E_RANGE;
    if (n >= lib->albums[album].track_count) return LIB_E_RANGE;
    return libidx_track_global(lib, lib->albums[album].track_first + n, out);
}
