/* art_find.c — locate album art. See art_find.h. No HAL, no stdio. */

#include "art_find.h"

#include <string.h>

/* Folder image names, in order of preference, tried with each extension.
 * FatFs matches long names case-insensitively, so "Cover.JPG" is found by
 * "cover.jpg". */
static const char *const FOLDER_NAMES[] = {
    "cover", "folder", "front", "album", "albumart"
};
static const char *const FOLDER_EXTS[] = { ".jpg", ".jpeg", ".png" };

#define N_NAMES (sizeof(FOLDER_NAMES) / sizeof(FOLDER_NAMES[0]))
#define N_EXTS  (sizeof(FOLDER_EXTS) / sizeof(FOLDER_EXTS[0]))

/* Sanity limits, so a corrupt header can never send a loop off into the
 * weeds: nobody's metadata has 256 blocks or a 64 MB picture. */
#define MAX_META_BLOCKS  256
#define MAX_ID3_FRAMES   512
#define MAX_PICTURE      (64u * 1024u * 1024u)
#define MAX_JPEG_SEGS    64

#define PIC_FRONT_COVER  3u

/* ------------------------------------------------------------ helpers */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14) |
           ((uint32_t)(p[2] & 0x7Fu) << 7) | (uint32_t)(p[3] & 0x7Fu);
}

/* Reads exactly `len` bytes at `off`. False on any error or short read. */
static bool read_at(const lib_io_t *io, void *fh, uint32_t off, void *buf,
                    uint32_t len)
{
    uint32_t got = 0;
    if (io->seek(io->ctx, fh, off) != 0) return false;
    if (io->read(io->ctx, fh, buf, len, &got) != 0) return false;
    return got == len;
}

/* Like read_at, but a short read is fine; returns the count, or 0. */
static uint32_t read_some(const lib_io_t *io, void *fh, uint32_t off,
                          void *buf, uint32_t len)
{
    uint32_t got = 0;
    if (io->seek(io->ctx, fh, off) != 0) return 0;
    if (io->read(io->ctx, fh, buf, len, &got) != 0) return 0;
    return got;
}

static void copy_path(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > ART_PATH_MAX) n = ART_PATH_MAX;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Is `f` more useful to report than `g`? Decodable beats everything, then
 * any real image beats unknown bytes, which beats nothing. */
static int fmt_rank(art_fmt_t f)
{
    switch (f) {
    case ART_FMT_JPEG:    return 3;
    case ART_FMT_NONE:    return 0;
    case ART_FMT_UNKNOWN: return 1;
    default:              return 2;
    }
}

/* -------------------------------------------------------------- probe */

/* A JPEG's SOF marker says how it was coded. Walk the segments from the
 * start until one turns up. EXIF blocks can be tens of KB, so segments are
 * skipped by their length rather than read. */
static art_fmt_t probe_jpeg(const lib_io_t *io, void *fh, uint32_t pos,
                            uint32_t end)
{
    int segs;
    uint8_t h[4];

    pos += 2;                           /* past FF D8 */
    for (segs = 0; segs < MAX_JPEG_SEGS; segs++) {
        uint8_t m;
        uint32_t len;

        if (end != 0u && pos + 4u > end) break;
        if (!read_at(io, fh, pos, h, 4)) break;
        if (h[0] != 0xFFu) break;
        m = h[1];
        if (m == 0xFFu) { pos += 1u; continue; }            /* fill byte */
        if ((m >= 0xD0u && m <= 0xD7u) || m == 0x01u) {     /* no length */
            pos += 2u;
            continue;
        }
        if (m == 0xC0u) return ART_FMT_JPEG;                 /* baseline */
        if (m == 0xC2u) return ART_FMT_JPEG_PROGRESSIVE;
        if ((m >= 0xC1u && m <= 0xCFu) && m != 0xC4u && m != 0xC8u &&
            m != 0xCCu) {
            return ART_FMT_JPEG_OTHER;                       /* other SOFn */
        }
        if (m == 0xDAu || m == 0xD9u) break;   /* scan or end, no frame */
        len = ((uint32_t)h[2] << 8) | h[3];
        if (len < 2u) break;
        pos += 2u + len;
    }
    return ART_FMT_UNKNOWN;
}

art_fmt_t art_probe(const lib_io_t *io, void *fh, uint32_t offset,
                    uint32_t length)
{
    static const uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A,
                                        0x1A, 0x0A };
    uint8_t b[12];
    uint32_t end = (length != 0u) ? offset + length : 0u;
    uint32_t got;

    if (io == NULL || fh == NULL) return ART_FMT_NONE;
    got = read_some(io, fh, offset, b, sizeof(b));
    if (length != 0u && got > length) got = length;
    if (got < 3u) return ART_FMT_NONE;

    if (b[0] == 0xFFu && b[1] == 0xD8u && b[2] == 0xFFu) {
        return probe_jpeg(io, fh, offset, end);
    }
    if (got >= 8u && memcmp(b, PNG_SIG, 8) == 0)       return ART_FMT_PNG;
    if (got >= 4u && memcmp(b, "GIF8", 4) == 0)        return ART_FMT_GIF;
    if (got >= 2u && b[0] == 'B' && b[1] == 'M')       return ART_FMT_BMP;
    if (got >= 12u && memcmp(b, "RIFF", 4) == 0 &&
        memcmp(b + 8, "WEBP", 4) == 0)                 return ART_FMT_WEBP;
    return ART_FMT_UNKNOWN;
}

/* ---------------------------------------------------------- embedded */

/* One picture a container offered, as found. */
typedef struct {
    bool     found;
    uint32_t type;            /* ID3/FLAC picture type; 3 = front cover */
    uint32_t offset, length;
} pic_t;

/* Keep the front cover if there is one, otherwise the first picture. */
static void offer(pic_t *best, uint32_t type, uint32_t off, uint32_t len)
{
    if (len == 0u || len > MAX_PICTURE) return;
    if (!best->found ||
        (type == PIC_FRONT_COVER && best->type != PIC_FRONT_COVER)) {
        best->found = true;
        best->type = type;
        best->offset = off;
        best->length = len;
    }
}

/* Total size of an ID3v2 tag starting at 0, or 0 if there is none. */
static uint32_t id3_total_size(const uint8_t *h)
{
    uint32_t size;
    if (memcmp(h, "ID3", 3) != 0) return 0u;
    size = 10u + syncsafe32(h + 6);
    if (h[3] >= 4u && (h[5] & 0x10u) != 0u) size += 10u;   /* footer */
    return size;
}

/* Walks a FLAC stream's metadata blocks for PICTURE (type 6). */
static void scan_flac(const lib_io_t *io, void *fh, uint32_t pos, pic_t *best)
{
    uint8_t h[4];
    int n;

    if (!read_at(io, fh, pos, h, 4) || memcmp(h, "fLaC", 4) != 0) return;
    pos += 4u;

    for (n = 0; n < MAX_META_BLOCKS; n++) {
        bool last;
        uint32_t type, len;

        if (!read_at(io, fh, pos, h, 4)) return;
        last = (h[0] & 0x80u) != 0u;
        type = h[0] & 0x7Fu;
        len = be24(h + 1);
        pos += 4u;

        if (type == 6u && len >= 32u) {
            /* PICTURE: type, MIME, description, 4 x u32 dims, data. */
            uint8_t w[4];
            uint32_t p = pos, pic_type, mime_len, desc_len, data_len;
            uint32_t block_end = pos + len;

            if (!read_at(io, fh, p, w, 4)) return;
            pic_type = be32(w);            p += 4u;
            if (!read_at(io, fh, p, w, 4)) return;
            mime_len = be32(w);            p += 4u;
            if (mime_len > len) goto next;
            p += mime_len;
            if (!read_at(io, fh, p, w, 4)) return;
            desc_len = be32(w);            p += 4u;
            if (desc_len > len) goto next;
            p += desc_len + 16u;           /* width, height, depth, colours */
            if (!read_at(io, fh, p, w, 4)) return;
            data_len = be32(w);            p += 4u;
            if (p + data_len <= block_end) offer(best, pic_type, p, data_len);
        }
    next:
        pos += len;
        if (last) return;
    }
}

/* Parses the head of an APIC (v2.3/2.4) or PIC (v2.2) frame body and offers
 * the picture data that follows the header fields. */
static void id3_picture(const lib_io_t *io, void *fh, uint32_t body,
                        uint32_t body_len, bool v22, pic_t *best)
{
    uint8_t b[256];
    uint32_t got, i, enc, pic_type;

    got = read_some(io, fh, body, b, sizeof(b));
    if (got > body_len) got = body_len;
    if (got < 4u) return;

    enc = b[0];
    if (v22) {
        i = 4u;                            /* encoding + 3-char format */
    } else {
        i = 1u;
        while (i < got && b[i] != 0u) i++; /* MIME, Latin-1, NUL-ended */
        i++;
    }
    if (i >= got) return;
    pic_type = b[i++];

    /* Description: one NUL in Latin-1/UTF-8, two (aligned) in UTF-16. */
    if (enc == 1u || enc == 2u) {
        while (i + 1u < got && !(b[i] == 0u && b[i + 1u] == 0u)) i += 2u;
        i += 2u;
    } else {
        while (i < got && b[i] != 0u) i++;
        i++;
    }
    if (i > got || i >= body_len) return;  /* description ran past our view */

    offer(best, pic_type, body + i, body_len - i);
}

/* Walks an ID3v2 tag at the start of the file for pictures. */
static void scan_id3(const lib_io_t *io, void *fh, pic_t *best)
{
    uint8_t h[10];
    uint32_t pos, end, major, flags;
    int n;

    if (!read_at(io, fh, 0, h, 10) || memcmp(h, "ID3", 3) != 0) return;
    major = h[3];
    flags = h[5];
    end = 10u + syncsafe32(h + 6);
    pos = 10u;

    if (major < 2u || major > 4u) return;
    /* Whole-tag unsynchronisation rewrites every FF xx in the picture; it
     * could be undone, but it is rare enough in modern files to skip. */
    if ((flags & 0x80u) != 0u && major < 4u) return;

    if ((flags & 0x40u) != 0u && major >= 3u) {          /* extended header */
        uint8_t e[4];
        if (!read_at(io, fh, pos, e, 4)) return;
        pos += (major == 4u) ? syncsafe32(e) : 4u + be32(e);
    }

    for (n = 0; n < MAX_ID3_FRAMES && pos < end; n++) {
        uint8_t f[10];
        uint32_t size, hdr = (major == 2u) ? 6u : 10u;

        if (pos + hdr > end || !read_at(io, fh, pos, f, hdr)) return;
        if (f[0] == 0u) return;                          /* padding */

        if (major == 2u) {
            size = be24(f + 3);
            if (memcmp(f, "PIC", 3) == 0) {
                id3_picture(io, fh, pos + hdr, size, true, best);
            }
        } else {
            size = (major == 4u) ? syncsafe32(f + 4) : be32(f + 4);
            if (memcmp(f, "APIC", 4) == 0) {
                uint32_t body = pos + hdr, len = size;
                bool skip;
                if (major == 4u) {
                    /* compressed, encrypted, unsynchronised: skip; a data
                     * length indicator is 4 bytes we step over. */
                    skip = (f[9] & 0x0Eu) != 0u;
                    if (!skip && (f[9] & 0x01u) != 0u && len >= 4u) {
                        body += 4u;
                        len -= 4u;
                    }
                } else {
                    skip = (f[9] & 0xC0u) != 0u; /* compressed, encrypted */
                }
                if (!skip) id3_picture(io, fh, body, len, false, best);
            }
        }
        if (size == 0u || size > end) return;
        pos += hdr + size;
    }
}

bool art_find_embedded(const lib_io_t *io, const char *path, art_ref_t *out)
{
    void *fh = NULL;
    uint8_t h[10];
    pic_t best;
    uint32_t id3;
    art_from_t from = ART_FROM_NONE;

    if (out != NULL) memset(out, 0, sizeof(*out));
    if (io == NULL || path == NULL || out == NULL) return false;
    if (io->open(io->ctx, path, LIB_IO_READ, &fh) != 0) return false;

    memset(&best, 0, sizeof(best));
    if (read_at(io, fh, 0, h, 10)) {
        id3 = id3_total_size(h);
        if (id3 != 0u) {
            scan_id3(io, fh, &best);
            if (best.found) from = ART_FROM_ID3;
        }
        /* FLAC, possibly behind an ID3 tag some taggers prepend. */
        if (!best.found) {
            scan_flac(io, fh, id3, &best);
            if (best.found) from = ART_FROM_FLAC;
        }
    }

    if (best.found) {
        out->fmt = art_probe(io, fh, best.offset, best.length);
        out->from = from;
        copy_path(out->path, path);
        out->offset = best.offset;
        out->length = best.length;
    }
    io->close(io->ctx, fh);
    return best.found;
}

/* ------------------------------------------------------------ folders */

/* Length of the directory part of `path`, including its trailing '/'. */
static size_t dir_len(const char *path, size_t upto)
{
    while (upto > 0u && path[upto - 1u] != '/') upto--;
    return upto;
}

/* Does the last component of path[0..len) look like "Disc 1", "CD2",
 * "disk_03"? Then the album folder is the one above. */
static bool is_disc_dir(const char *path, size_t len)
{
    size_t start, n;
    const char *s;
    char c0, c1, c2, c3;

    if (len < 2u) return false;
    n = len - 1u;                              /* drop the trailing '/' */
    start = dir_len(path, n);
    s = path + start;
    n -= start;
    if (n < 2u) return false;

    c0 = (char)(s[0] | 0x20);
    c1 = (char)(s[1] | 0x20);
    if (c0 == 'c' && c1 == 'd') return true;
    if (n < 4u) return false;
    c2 = (char)(s[2] | 0x20);
    c3 = (char)(s[3] | 0x20);
    return c0 == 'd' && c1 == 'i' && (c2 == 's') && (c3 == 'c' || c3 == 'k');
}

/* Tries every folder image name in the directory path[0..len). Stops at
 * the first decodable one; remembers the best undecodable one in `alt`. */
static bool scan_folder(const lib_io_t *io, const char *path, size_t len,
                        art_ref_t *out, art_ref_t *alt)
{
    char buf[ART_PATH_MAX + 1];
    size_t i, j;

    if (len + 16u > ART_PATH_MAX) return false;
    memcpy(buf, path, len);

    for (i = 0; i < N_NAMES; i++) {
        for (j = 0; j < N_EXTS; j++) {
            size_t a = strlen(FOLDER_NAMES[i]), b = strlen(FOLDER_EXTS[j]);
            void *fh = NULL;
            art_fmt_t fmt;

            memcpy(buf + len, FOLDER_NAMES[i], a);
            memcpy(buf + len + a, FOLDER_EXTS[j], b);
            buf[len + a + b] = '\0';

            if (io->open(io->ctx, buf, LIB_IO_READ, &fh) != 0) continue;
            fmt = art_probe(io, fh, 0, 0);
            io->close(io->ctx, fh);

            if (fmt == ART_FMT_JPEG) {
                out->fmt = fmt;
                out->from = ART_FROM_FOLDER;
                copy_path(out->path, buf);
                out->offset = 0;
                out->length = 0;
                return true;
            }
            if (fmt_rank(fmt) > fmt_rank(alt->fmt)) {
                alt->fmt = fmt;
                alt->from = ART_FROM_FOLDER;
                copy_path(alt->path, buf);
                alt->offset = 0;
                alt->length = 0;
            }
        }
    }
    return false;
}

bool art_find(const lib_io_t *io, const char *track_path, art_ref_t *out)
{
    art_ref_t alt, emb;
    size_t d;

    if (out != NULL) memset(out, 0, sizeof(*out));
    if (io == NULL || track_path == NULL || out == NULL ||
        io->open == NULL || io->read == NULL || io->seek == NULL ||
        io->close == NULL) {
        return false;
    }
    memset(&alt, 0, sizeof(alt));

    d = dir_len(track_path, strlen(track_path));
    if (scan_folder(io, track_path, d, out, &alt)) return true;
    if (is_disc_dir(track_path, d)) {
        size_t up = dir_len(track_path, d - 1u);
        if (scan_folder(io, track_path, up, out, &alt)) return true;
    }

    if (art_find_embedded(io, track_path, &emb)) {
        if (emb.fmt == ART_FMT_JPEG) {
            *out = emb;
            return true;
        }
        if (fmt_rank(emb.fmt) > fmt_rank(alt.fmt)) alt = emb;
    }

    *out = alt;
    return false;
}

/* ------------------------------------------------------------ reading */

static size_t reader_read(void *ctx, uint8_t *buf, size_t len)
{
    art_reader_t *rd = (art_reader_t *)ctx;
    uint32_t want, got = 0;

    if (rd->end != 0u) {
        uint32_t left = (rd->pos < rd->end) ? rd->end - rd->pos : 0u;
        if (len > left) len = left;
    }
    want = (uint32_t)len;
    if (want == 0u) return 0;

    if (buf == NULL) {              /* skip: just move, the next read seeks */
        rd->pos += want;
        return want;
    }
    if (rd->io->seek(rd->io->ctx, rd->fh, rd->pos) != 0) return 0;
    if (rd->io->read(rd->io->ctx, rd->fh, buf, want, &got) != 0) return 0;
    rd->pos += got;
    return got;
}

static void reader_yield(void *ctx)
{
    art_reader_t *rd = (art_reader_t *)ctx;
    if (rd->yield != NULL) rd->yield(rd->yield_ctx);
}

bool art_open(const lib_io_t *io, const art_ref_t *ref, art_reader_t *rd,
              art_src_t *src, void (*yield)(void *ctx), void *yield_ctx)
{
    if (rd != NULL) memset(rd, 0, sizeof(*rd));
    if (io == NULL || ref == NULL || rd == NULL || src == NULL ||
        ref->fmt == ART_FMT_NONE) {
        return false;
    }
    if (io->open(io->ctx, ref->path, LIB_IO_READ, &rd->fh) != 0) {
        rd->fh = NULL;
        return false;
    }
    rd->io = io;
    rd->pos = ref->offset;
    rd->end = (ref->length != 0u) ? ref->offset + ref->length : 0u;
    rd->yield = yield;
    rd->yield_ctx = yield_ctx;

    src->read = reader_read;
    src->yield = reader_yield;
    src->ctx = rd;
    return true;
}

void art_close(art_reader_t *rd)
{
    if (rd != NULL && rd->io != NULL && rd->fh != NULL) {
        rd->io->close(rd->io->ctx, rd->fh);
    }
    if (rd != NULL) rd->fh = NULL;
}

const char *art_fmt_name(art_fmt_t f)
{
    switch (f) {
    case ART_FMT_NONE:             return "none";
    case ART_FMT_JPEG:             return "JPEG";
    case ART_FMT_JPEG_PROGRESSIVE: return "progressive JPEG";
    case ART_FMT_JPEG_OTHER:       return "unusual JPEG";
    case ART_FMT_PNG:              return "PNG";
    case ART_FMT_GIF:              return "GIF";
    case ART_FMT_BMP:              return "BMP";
    case ART_FMT_WEBP:             return "WebP";
    case ART_FMT_UNKNOWN:          return "unrecognised data";
    default:                       return "?";
    }
}

const char *art_from_name(art_from_t f)
{
    switch (f) {
    case ART_FROM_NONE:   return "none";
    case ART_FROM_FOLDER: return "folder image";
    case ART_FROM_FLAC:   return "embedded (FLAC)";
    case ART_FROM_ID3:    return "embedded (ID3)";
    default:              return "?";
    }
}
