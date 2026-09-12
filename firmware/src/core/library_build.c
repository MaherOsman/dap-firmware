/* library_build.c — two-pass index builder. No HAL, no stdio, no malloc. */

#include "library_build.h"

#include <string.h>

#define ALIGN4(x) (((x) + 3u) & ~3u)

/* ===================================================================== */
/* small string helpers                                                  */
/* ===================================================================== */

static char lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

/* Copies at most cap-1 bytes plus a NUL. Returns 1 if it had to truncate. */
static int copy_bounded(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (cap == 0u) return 1;
    while (src[i] != '\0' && i + 1u < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return src[i] != '\0';
}

/* Case-insensitive first, exact as a tiebreak, so "abba" and "ABBA" sort
 * adjacently and deterministically instead of by discovery order. */
static int name_cmp(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        char ca = lower(a[i]);
        char cb = lower(b[i]);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        i++;
    }
    if (a[i] != b[i]) return (a[i] == '\0') ? -1 : 1;
    return strcmp(a, b);
}

static int str_eq_ci(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (lower(a[i]) != lower(b[i])) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* First four characters, lowered, packed MSB-first so a plain integer
 * comparison orders them lexicographically. */
static uint32_t title_key4(const char *s)
{
    uint32_t k = 0;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint8_t c = (s[i] != '\0') ? (uint8_t)lower(s[i]) : 0u;
        k = (k << 8) | c;
        if (s[i] == '\0') {
            k <<= (8u * (3u - i));
            break;
        }
    }
    return k;
}

/* ===================================================================== */
/* path -> codec and tags                                                */
/* ===================================================================== */

static const char *path_ext(const char *path)
{
    const char *dot = NULL;
    uint32_t i;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '.') dot = path + i;
        if (path[i] == '/') dot = NULL;
    }
    return dot;
}

uint8_t lib_codec_from_path(const char *path)
{
    const char *e = path ? path_ext(path) : NULL;
    if (!e) return LIB_CODEC_UNKNOWN;
    if (str_eq_ci(e, ".wav"))  return LIB_CODEC_WAV;
    if (str_eq_ci(e, ".flac")) return LIB_CODEC_FLAC;
    if (str_eq_ci(e, ".mp3"))  return LIB_CODEC_MP3;
    return LIB_CODEC_UNKNOWN;
}

/* Index of the character after the last '/', i.e. the basename start. */
static uint32_t basename_at(const char *p, uint32_t end)
{
    uint32_t i = end;
    while (i > 0u && p[i - 1u] != '/') i--;
    return i;
}

void lib_tags_from_path(const char *path, lib_tags_t *out)
{
    uint32_t len, base, i, n;
    uint32_t dir_end, dir_start, par_end, par_start;
    const char *ext;
    char work[LIB_TITLE_MAX + 1];

    memset(out, 0, sizeof(*out));
    copy_bounded(out->artist, "Unknown Artist", LIB_NAME_MAX);
    copy_bounded(out->album, "Unknown Album", LIB_NAME_MAX);
    out->title[0] = '\0';

    if (!path || path[0] == '\0') return;

    len = (uint32_t)strlen(path);
    base = basename_at(path, len);

    /* title = filename without extension */
    ext = path_ext(path);
    n = ext ? (uint32_t)(ext - (path + base)) : (len - base);
    if (n > LIB_TITLE_MAX) n = LIB_TITLE_MAX;
    memcpy(work, path + base, n);
    work[n] = '\0';

    /* leading "03 ", "03-", "03_", "03." becomes track_no */
    i = 0;
    while (work[i] >= '0' && work[i] <= '9' && i < 3u) i++;
    if (i > 0u && (work[i] == ' ' || work[i] == '-' || work[i] == '_' ||
                   work[i] == '.')) {
        uint32_t no = 0;
        uint32_t j;
        for (j = 0; j < i; j++) no = no * 10u + (uint32_t)(work[j] - '0');
        out->track_no = (uint16_t)no;
        i++;
        while (work[i] == ' ' || work[i] == '-' || work[i] == '_') i++;
        copy_bounded(out->title, work + i, LIB_TITLE_MAX + 1u);
    } else {
        copy_bounded(out->title, work, LIB_TITLE_MAX + 1u);
    }

    if (out->title[0] == '\0') {
        copy_bounded(out->title, "Untitled", LIB_TITLE_MAX + 1u);
    }

    /* album = parent directory, artist = grandparent */
    if (base == 0u) return;
    dir_end = base - 1u;                      /* strips the '/' */
    dir_start = basename_at(path, dir_end);
    if (dir_end > dir_start) {
        n = dir_end - dir_start;
        if (n > LIB_NAME_MAX - 1u) n = LIB_NAME_MAX - 1u;
        memcpy(out->album, path + dir_start, n);
        out->album[n] = '\0';
    }

    if (dir_start == 0u) return;
    par_end = dir_start - 1u;
    par_start = basename_at(path, par_end);
    if (par_end > par_start) {
        n = par_end - par_start;
        if (n > LIB_NAME_MAX - 1u) n = LIB_NAME_MAX - 1u;
        memcpy(out->artist, path + par_start, n);
        out->artist[n] = '\0';
    }
}

/* ===================================================================== */
/* build state                                                           */
/* ===================================================================== */

typedef struct {
    uint32_t album;     /* interned album id, patched to final at write time */
    uint32_t title4;
    uint32_t seq;       /* record index in the temp file */
    uint16_t track_no;
    uint16_t pad;
} bld_ent_t;

typedef struct {
    uint32_t name_off;
} bart_t;

typedef struct {
    uint32_t name_off;
    uint32_t artist;
} balb_t;

typedef struct {
    const lib_build_cfg_t *cfg;
    lib_build_stats_t     *st;

    bld_ent_t *ents;
    bart_t    *arts;
    balb_t    *albs;
    uint32_t  *art_order;
    uint32_t  *art_rank;
    uint32_t  *alb_order;
    uint32_t  *alb_rank;
    char      *pool;
    uint8_t   *rec;

    uint32_t n_ent, n_art, n_alb;
    uint32_t pool_used;

    void *temp_fh;
} bld_t;

uint32_t library_build_arena_bytes(const lib_build_cfg_t *cfg)
{
    if (!cfg) return 0u;
    return ALIGN4(cfg->max_tracks  * (uint32_t)sizeof(bld_ent_t))
         + ALIGN4(cfg->max_artists * (uint32_t)sizeof(bart_t))
         + ALIGN4(cfg->max_albums  * (uint32_t)sizeof(balb_t))
         + ALIGN4(cfg->max_artists * 4u) * 2u
         + ALIGN4(cfg->max_albums  * 4u) * 2u
         + ALIGN4(cfg->pool_bytes)
         + LIB_TRACK_REC_SIZE;
}

static void carve(bld_t *b, const lib_build_cfg_t *cfg)
{
    uint8_t *p = (uint8_t *)cfg->arena;

    b->ents = (bld_ent_t *)(void *)p;
    p += ALIGN4(cfg->max_tracks * (uint32_t)sizeof(bld_ent_t));
    b->arts = (bart_t *)(void *)p;
    p += ALIGN4(cfg->max_artists * (uint32_t)sizeof(bart_t));
    b->albs = (balb_t *)(void *)p;
    p += ALIGN4(cfg->max_albums * (uint32_t)sizeof(balb_t));
    b->art_order = (uint32_t *)(void *)p;
    p += ALIGN4(cfg->max_artists * 4u);
    b->art_rank = (uint32_t *)(void *)p;
    p += ALIGN4(cfg->max_artists * 4u);
    b->alb_order = (uint32_t *)(void *)p;
    p += ALIGN4(cfg->max_albums * 4u);
    b->alb_rank = (uint32_t *)(void *)p;
    p += ALIGN4(cfg->max_albums * 4u);
    b->pool = (char *)p;
    p += ALIGN4(cfg->pool_bytes);
    b->rec = p;
}

/* ---------------------------------------------------------- interning */

static int pool_add(bld_t *b, const char *s, uint32_t *off)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    if (b->pool_used + n > b->cfg->pool_bytes) return LIB_E_FULL;
    memcpy(b->pool + b->pool_used, s, n);
    *off = b->pool_used;
    b->pool_used += n;
    return LIB_OK;
}

static int intern_artist(bld_t *b, const char *name, uint32_t *id)
{
    uint32_t i, off;
    int rc;

    for (i = 0; i < b->n_art; i++) {
        if (str_eq_ci(b->pool + b->arts[i].name_off, name)) {
            *id = i;
            return LIB_OK;
        }
    }
    if (b->n_art >= b->cfg->max_artists) return LIB_E_FULL;
    rc = pool_add(b, name, &off);
    if (rc != LIB_OK) return rc;
    b->arts[b->n_art].name_off = off;
    *id = b->n_art++;
    return LIB_OK;
}

/* Albums are keyed by (artist, name) so two different artists can both have
 * "Greatest Hits" without collapsing into one album. */
static int intern_album(bld_t *b, uint32_t artist, const char *name,
                        uint32_t *id)
{
    uint32_t i, off;
    int rc;

    for (i = 0; i < b->n_alb; i++) {
        if (b->albs[i].artist == artist &&
            str_eq_ci(b->pool + b->albs[i].name_off, name)) {
            *id = i;
            return LIB_OK;
        }
    }
    if (b->n_alb >= b->cfg->max_albums) return LIB_E_FULL;
    rc = pool_add(b, name, &off);
    if (rc != LIB_OK) return rc;
    b->albs[b->n_alb].name_off = off;
    b->albs[b->n_alb].artist = artist;
    *id = b->n_alb++;
    return LIB_OK;
}

/* ===================================================================== */
/* sorting — shell sort, in place, no recursion, no scratch              */
/* ===================================================================== */

static const uint32_t GAPS[] = { 701u, 301u, 132u, 57u, 23u, 10u, 4u, 1u };
#define N_GAPS (sizeof(GAPS) / sizeof(GAPS[0]))

typedef int (*idx_cmp_fn)(const bld_t *b, uint32_t x, uint32_t y);

static void sort_idx(const bld_t *b, uint32_t *a, uint32_t n, idx_cmp_fn cmp)
{
    uint32_t g, i, j;
    for (g = 0; g < N_GAPS; g++) {
        uint32_t gap = GAPS[g];
        if (gap >= n) continue;
        for (i = gap; i < n; i++) {
            uint32_t v = a[i];
            j = i;
            while (j >= gap && cmp(b, a[j - gap], v) > 0) {
                a[j] = a[j - gap];
                j -= gap;
            }
            a[j] = v;
        }
    }
}

static int cmp_artist(const bld_t *b, uint32_t x, uint32_t y)
{
    return name_cmp(b->pool + b->arts[x].name_off,
                    b->pool + b->arts[y].name_off);
}

static int cmp_album(const bld_t *b, uint32_t x, uint32_t y)
{
    uint32_t ax = b->art_rank[b->albs[x].artist];
    uint32_t ay = b->art_rank[b->albs[y].artist];
    if (ax != ay) return (ax < ay) ? -1 : 1;
    return name_cmp(b->pool + b->albs[x].name_off,
                    b->pool + b->albs[y].name_off);
}

static int cmp_ent(const bld_t *b, const bld_ent_t *x, const bld_ent_t *y)
{
    uint32_t ax = b->alb_rank[x->album];
    uint32_t ay = b->alb_rank[y->album];
    if (ax != ay) return (ax < ay) ? -1 : 1;
    if (x->track_no != y->track_no) return (x->track_no < y->track_no) ? -1 : 1;
    if (x->title4 != y->title4) return (x->title4 < y->title4) ? -1 : 1;
    return (x->seq < y->seq) ? -1 : 1;
}

static void sort_ents(bld_t *b)
{
    uint32_t g, i, j;
    for (g = 0; g < N_GAPS; g++) {
        uint32_t gap = GAPS[g];
        if (gap >= b->n_ent) continue;
        for (i = gap; i < b->n_ent; i++) {
            bld_ent_t v = b->ents[i];
            j = i;
            while (j >= gap && cmp_ent(b, &b->ents[j - gap], &v) > 0) {
                b->ents[j] = b->ents[j - gap];
                j -= gap;
            }
            b->ents[j] = v;
        }
    }
}

/* ===================================================================== */
/* pass 1 — walk                                                         */
/* ===================================================================== */

static int add_track(bld_t *b, const char *path, uint32_t size)
{
    const lib_build_cfg_t *cfg = b->cfg;
    lib_tags_t tags;
    lib_track_t t;
    uint32_t artist_id, album_id;
    uint8_t codec;
    int rc;

    codec = lib_codec_from_path(path);
    if (codec == LIB_CODEC_UNKNOWN) {
        b->st->skipped_not_audio++;
        return LIB_OK;
    }
    if (strlen(path) > LIB_PATH_MAX) {
        /* Storing it would silently truncate and the path would never open
         * again, so refuse it and make it visible in the stats instead. */
        b->st->skipped_path_too_long++;
        return LIB_OK;
    }
    if (b->n_ent >= cfg->max_tracks) return LIB_E_FULL;

    lib_tags_from_path(path, &tags);
    if (cfg->tags) {
        (void)cfg->tags(cfg->tags_ctx, cfg->io, path, size, codec, &tags);
        tags.artist[LIB_NAME_MAX - 1u] = '\0';
        tags.album[LIB_NAME_MAX - 1u] = '\0';
        tags.title[LIB_TITLE_MAX] = '\0';
        if (tags.artist[0] == '\0') copy_bounded(tags.artist, "Unknown Artist", LIB_NAME_MAX);
        if (tags.album[0] == '\0')  copy_bounded(tags.album, "Unknown Album", LIB_NAME_MAX);
        if (tags.title[0] == '\0')  copy_bounded(tags.title, "Untitled", LIB_TITLE_MAX + 1u);
    }

    rc = intern_artist(b, tags.artist, &artist_id);
    if (rc != LIB_OK) return rc;
    rc = intern_album(b, artist_id, tags.album, &album_id);
    if (rc != LIB_OK) return rc;

    memset(&t, 0, sizeof(t));
    t.album_idx = album_id;           /* interned id; remapped in pass 2 */
    t.duration_ms = tags.duration_ms;
    t.file_size = size;
    t.track_no = tags.track_no;
    t.codec = codec;
    copy_bounded(t.title, tags.title, LIB_TITLE_MAX + 1u);
    copy_bounded(t.path, path, LIB_PATH_MAX + 1u);

    lib_encode_track(b->rec, &t);
    if (cfg->io->write(cfg->io->ctx, b->temp_fh, b->rec,
                       LIB_TRACK_REC_SIZE) != 0) {
        return LIB_E_IO;
    }

    b->ents[b->n_ent].album = album_id;
    b->ents[b->n_ent].title4 = title_key4(t.title);
    b->ents[b->n_ent].seq = b->n_ent;
    b->ents[b->n_ent].track_no = tags.track_no;
    b->ents[b->n_ent].pad = 0;
    b->n_ent++;

    b->st->tracks++;
    return LIB_OK;
}

static int skip_name(const char *n)
{
    if (n[0] == '\0') return 1;
    if (n[0] == '.') return 1;     /* ".", "..", and hidden entries */
    if (str_eq_ci(n, "System Volume Information")) return 1;
    return 0;
}

typedef struct {
    void    *dh;
    uint32_t parent_len;
} lvl_t;

static int walk(bld_t *b)
{
    const lib_build_cfg_t *cfg = b->cfg;
    lvl_t stack[LIB_BUILD_MAX_DEPTH];
    char path[LIB_PATH_MAX + 2];
    uint32_t depth = 0;
    uint32_t plen;
    int rc = LIB_OK;

    plen = (uint32_t)strlen(cfg->root);
    if (plen > LIB_PATH_MAX) return LIB_E_ARG;
    memcpy(path, cfg->root, plen + 1u);
    while (plen > 1u && path[plen - 1u] == '/') {
        plen--;              /* normalise "/Music/" -> "/Music" */
        path[plen] = '\0';
    }
    if (plen == 1u && path[0] == '/') {
        plen = 0u;           /* root becomes "" so children are "/Name" */
        path[0] = '\0';
    }

    if (cfg->dir->opendir(cfg->dir->ctx, plen ? path : "/", &stack[0].dh) != 0) {
        return LIB_E_IO;
    }
    stack[0].parent_len = 0u;
    depth = 1u;
    b->st->dirs_visited++;

    while (depth > 0u) {
        lib_dirent_t de;
        int done = 0;
        uint32_t nlen;

        if (cfg->dir->readdir(cfg->dir->ctx, stack[depth - 1u].dh, &de,
                              &done) != 0) {
            rc = LIB_E_IO;
            break;
        }
        if (done) {
            cfg->dir->closedir(cfg->dir->ctx, stack[depth - 1u].dh);
            plen = stack[depth - 1u].parent_len;
            path[plen] = '\0';
            depth--;
            continue;
        }

        de.name[LIB_FILENAME_MAX - 1u] = '\0';
        if (skip_name(de.name)) continue;

        nlen = (uint32_t)strlen(de.name);
        if (plen + 1u + nlen > LIB_PATH_MAX) {
            if (de.is_dir) b->st->skipped_too_deep++;
            else b->st->skipped_path_too_long++;
            continue;
        }

        path[plen] = '/';
        memcpy(path + plen + 1u, de.name, nlen + 1u);

        if (de.is_dir) {
            if (depth >= LIB_BUILD_MAX_DEPTH) {
                b->st->skipped_too_deep++;
                path[plen] = '\0';
                continue;
            }
            if (cfg->dir->opendir(cfg->dir->ctx, path,
                                  &stack[depth].dh) != 0) {
                /* An unreadable directory is not fatal — skip the subtree. */
                path[plen] = '\0';
                continue;
            }
            stack[depth].parent_len = plen;
            depth++;
            plen = plen + 1u + nlen;
            b->st->dirs_visited++;
            continue;
        }

        b->st->files_seen++;
        rc = add_track(b, path, de.size);
        path[plen] = '\0';
        if (rc != LIB_OK) break;

        if (cfg->progress && (b->st->tracks % 32u) == 0u) {
            cfg->progress(cfg->progress_ctx, LIB_PHASE_SCAN, b->st->tracks, 0u);
        }
    }

    while (depth > 0u) {
        cfg->dir->closedir(cfg->dir->ctx, stack[depth - 1u].dh);
        depth--;
    }
    return rc;
}

/* ===================================================================== */
/* pass 2 — sort and write                                               */
/* ===================================================================== */

static int write_at(const lib_io_t *io, void *fh, const void *src, uint32_t n)
{
    return (io->write(io->ctx, fh, src, n) != 0) ? LIB_E_IO : LIB_OK;
}

static int emit(bld_t *b, void *out_fh)
{
    const lib_build_cfg_t *cfg = b->cfg;
    const lib_io_t *io = cfg->io;
    lib_header_t hdr;
    uint8_t buf[LIB_HDR_SIZE];
    uint32_t i, run, pool_len;
    int rc;

    /* --- ranks --- */
    for (i = 0; i < b->n_art; i++) b->art_order[i] = i;
    sort_idx(b, b->art_order, b->n_art, cmp_artist);
    for (i = 0; i < b->n_art; i++) b->art_rank[b->art_order[i]] = i;

    for (i = 0; i < b->n_alb; i++) b->alb_order[i] = i;
    sort_idx(b, b->alb_order, b->n_alb, cmp_album);
    for (i = 0; i < b->n_alb; i++) b->alb_rank[b->alb_order[i]] = i;

    sort_ents(b);

    pool_len = b->pool_used;
    if (pool_len == 0u) {
        b->pool[0] = '\0';
        pool_len = 1u;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.version = LIB_IDX_VERSION;
    hdr.artist_count = b->n_art;
    hdr.album_count = b->n_alb;
    hdr.track_count = b->n_ent;
    hdr.artist_off = LIB_HDR_SIZE;
    hdr.album_off = hdr.artist_off + b->n_art * LIB_ARTIST_REC_SIZE;
    hdr.strpool_off = hdr.album_off + b->n_alb * LIB_ALBUM_REC_SIZE;
    hdr.strpool_len = pool_len;
    hdr.track_off = hdr.strpool_off + pool_len;
    hdr.track_rec_size = LIB_TRACK_REC_SIZE;
    hdr.build_id = cfg->build_id;
    hdr.total_size = hdr.track_off + b->n_ent * LIB_TRACK_REC_SIZE;

    lib_encode_header(buf, &hdr);
    rc = write_at(io, out_fh, buf, LIB_HDR_SIZE);
    if (rc != LIB_OK) return rc;

    /* --- artist table, in sorted order, with contiguous album runs --- */
    run = 0u;
    for (i = 0; i < b->n_art; i++) {
        uint32_t src = b->art_order[i];
        uint32_t count = 0;
        uint32_t j;
        uint8_t arec[LIB_ARTIST_REC_SIZE];

        for (j = 0; j < b->n_alb; j++) {
            if (b->albs[j].artist == src) count++;
        }
        memset(arec, 0, sizeof(arec));
        lib_wr_u32(arec + 0, b->arts[src].name_off);
        lib_wr_u32(arec + 4, run);
        lib_wr_u32(arec + 8, count);
        rc = write_at(io, out_fh, arec, LIB_ARTIST_REC_SIZE);
        if (rc != LIB_OK) return rc;
        run += count;
    }

    /* --- album table, in sorted order, with contiguous track runs --- */
    run = 0u;
    for (i = 0; i < b->n_alb; i++) {
        uint32_t src = b->alb_order[i];
        uint32_t count = 0;
        uint32_t j;
        uint8_t brec[LIB_ALBUM_REC_SIZE];

        for (j = 0; j < b->n_ent; j++) {
            if (b->ents[j].album == src) count++;
        }
        memset(brec, 0, sizeof(brec));
        lib_wr_u32(brec + 0, b->albs[src].name_off);
        lib_wr_u32(brec + 4, b->art_rank[b->albs[src].artist]);
        lib_wr_u32(brec + 8, run);
        lib_wr_u32(brec + 12, count);
        rc = write_at(io, out_fh, brec, LIB_ALBUM_REC_SIZE);
        if (rc != LIB_OK) return rc;
        run += count;
    }

    rc = write_at(io, out_fh, b->pool, pool_len);
    if (rc != LIB_OK) return rc;

    /* --- track records, read back from the temp file in sorted order --- */
    for (i = 0; i < b->n_ent; i++) {
        uint32_t got = 0;
        uint32_t off = b->ents[i].seq * LIB_TRACK_REC_SIZE;

        if (io->seek(io->ctx, b->temp_fh, off) != 0) return LIB_E_IO;
        if (io->read(io->ctx, b->temp_fh, b->rec, LIB_TRACK_REC_SIZE,
                     &got) != 0 || got != LIB_TRACK_REC_SIZE) {
            return LIB_E_IO;
        }
        /* patch the interned album id to its final sorted index */
        lib_wr_u32(b->rec + 0, b->alb_rank[b->ents[i].album]);

        rc = write_at(io, out_fh, b->rec, LIB_TRACK_REC_SIZE);
        if (rc != LIB_OK) return rc;

        if (cfg->progress && (i % 64u) == 0u) {
            cfg->progress(cfg->progress_ctx, LIB_PHASE_WRITE, i, b->n_ent);
        }
    }

    if (cfg->progress) {
        cfg->progress(cfg->progress_ctx, LIB_PHASE_WRITE, b->n_ent, b->n_ent);
    }
    return LIB_OK;
}

/* ===================================================================== */

int library_build(const lib_build_cfg_t *cfg, lib_build_stats_t *stats)
{
    bld_t b;
    lib_build_stats_t local;
    const char *index_path;
    const char *temp_path;
    void *out_fh = NULL;
    int rc;

    if (!cfg || !cfg->io || !cfg->dir || !cfg->arena || !cfg->root) {
        return LIB_E_ARG;
    }
    if (!cfg->io->open || !cfg->io->read || !cfg->io->write ||
        !cfg->io->seek || !cfg->io->close) {
        return LIB_E_ARG;
    }
    if (!cfg->dir->opendir || !cfg->dir->readdir || !cfg->dir->closedir) {
        return LIB_E_ARG;
    }
    if (cfg->arena_len < library_build_arena_bytes(cfg)) return LIB_E_NOMEM;

    index_path = cfg->index_path ? cfg->index_path : LIB_INDEX_PATH;
    temp_path  = cfg->temp_path  ? cfg->temp_path  : LIB_TEMP_PATH;

    memset(&local, 0, sizeof(local));
    memset(&b, 0, sizeof(b));
    b.cfg = cfg;
    b.st = stats ? stats : &local;
    memset(b.st, 0, sizeof(*b.st));
    carve(&b, cfg);

    if (cfg->io->open(cfg->io->ctx, temp_path, LIB_IO_WRITE, &b.temp_fh) != 0) {
        return LIB_E_IO;
    }

    rc = walk(&b);

    cfg->io->close(cfg->io->ctx, b.temp_fh);
    b.temp_fh = NULL;
    if (rc != LIB_OK) goto done;

    /* reopen the temp file for the random reads of pass 2 */
    if (cfg->io->open(cfg->io->ctx, temp_path, LIB_IO_READ, &b.temp_fh) != 0) {
        rc = LIB_E_IO;
        goto done;
    }
    if (cfg->io->open(cfg->io->ctx, index_path, LIB_IO_WRITE, &out_fh) != 0) {
        rc = LIB_E_IO;
        goto done;
    }

    rc = emit(&b, out_fh);

done:
    if (out_fh) cfg->io->close(cfg->io->ctx, out_fh);
    if (b.temp_fh) cfg->io->close(cfg->io->ctx, b.temp_fh);

    b.st->artists = b.n_art;
    b.st->albums = b.n_alb;

    if (rc == LIB_OK && cfg->io->unlink) {
        (void)cfg->io->unlink(cfg->io->ctx, temp_path);
    }
    return rc;
}
