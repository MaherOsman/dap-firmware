/* library_build.h — scan a card and write the binary index.
 *
 * Two passes, both streaming:
 *
 *   pass 1  walk the directory tree, derive tags for every audio file, intern
 *           artist and album names in RAM, append a 320-byte record per track
 *           to a temp file on the card, keep a 16-byte sort entry per track
 *           in RAM
 *   pass 2  sort artists by name, albums by (artist, name), tracks by
 *           (album, track_no, title); then walk the sorted entries, reading
 *           each record back from the temp file by offset and writing it to
 *           the index file in order
 *
 * The index file is therefore written strictly sequentially — the only random
 * access is 320-byte reads from the temp file, which is far cheaper on SD
 * than random writes (no FAT churn, no cluster reallocation).
 *
 * RAM cost is bounded by the caller-supplied limits, not by the card. For
 * 3000 tracks / 300 albums / 100 artists with a 48 KB pool that is ~120 KB of
 * scratch during the scan and zero afterwards.
 *
 * Like the reader, this file touches no HAL: directories come in through
 * lib_dir_t, files through lib_io_t.
 */
#ifndef LIBRARY_BUILD_H
#define LIBRARY_BUILD_H

#include <stdint.h>
#include "lib_io.h"
#include "library_index.h"

#define LIB_E_FULL   (-6)   /* a caller-supplied limit was exceeded */

#define LIB_NAME_MAX       64u   /* artist / album name, bytes */
#define LIB_FILENAME_MAX   256u  /* one directory entry name */
#define LIB_BUILD_MAX_DEPTH 8u   /* nested directory levels walked */
#define LIB_TEMP_PATH      "/dap.tmp"

/* ---------------------------------------------------------- directories */

typedef struct {
    char     name[LIB_FILENAME_MAX];
    int      is_dir;
    uint32_t size;
} lib_dirent_t;

typedef struct lib_dir {
    void *ctx;
    int (*opendir)(void *ctx, const char *path, void **dh);
    /* Fills *out and sets *done=0, or sets *done=1 at end of directory.
     * Returns non-zero only on a real error. */
    int (*readdir)(void *ctx, void *dh, lib_dirent_t *out, int *done);
    int (*closedir)(void *ctx, void *dh);
} lib_dir_t;

/* ---------------------------------------------------------------- tags */

typedef struct {
    char     artist[LIB_NAME_MAX];
    char     album[LIB_NAME_MAX];
    char     title[LIB_TITLE_MAX + 1];
    uint16_t track_no;
    uint32_t duration_ms;
} lib_tags_t;

/* Optional metadata hook. The builder fills `out` from the path first, then
 * calls this; anything the callback leaves alone keeps the path-derived value.
 * Return 0 on success. A non-zero return is not fatal — the track is still
 * indexed with the path-derived tags. */
typedef int (*lib_tag_fn)(void *ctx, const lib_io_t *io, const char *path,
                          uint32_t file_size, uint8_t codec, lib_tags_t *out);

/* Path-derived tags, exposed for testing and reuse:
 *   /Music/Artist/Album/03 Title.flac
 *     -> artist "Artist", album "Album", title "Title", track_no 3
 * Shallow paths fall back to "Unknown Artist" / "Unknown Album". */
void lib_tags_from_path(const char *path, lib_tags_t *out);

/* Extension -> LIB_CODEC_*, case-insensitive. LIB_CODEC_UNKNOWN if not audio. */
uint8_t lib_codec_from_path(const char *path);

/* ------------------------------------------------------------- building */

enum {
    LIB_PHASE_SCAN  = 0,
    LIB_PHASE_WRITE = 1
};

typedef struct {
    const lib_io_t  *io;
    const lib_dir_t *dir;

    lib_tag_fn tags;
    void      *tags_ctx;

    const char *root;        /* directory to walk, e.g. "/" or "/Music" */
    const char *index_path;  /* NULL -> LIB_INDEX_PATH */
    const char *temp_path;   /* NULL -> LIB_TEMP_PATH */

    uint32_t build_id;

    /* Hard limits. libidx_scan_arena_bytes() turns these into a size. */
    uint32_t max_tracks;
    uint32_t max_artists;
    uint32_t max_albums;
    uint32_t pool_bytes;

    void    *arena;
    uint32_t arena_len;

    /* Optional. done/total are in the units of the current phase; during
     * LIB_PHASE_SCAN total is 0 because it is not knowable yet. */
    void (*progress)(void *ctx, int phase, uint32_t done, uint32_t total);
    void  *progress_ctx;
} libidx_scan_cfg_t;

typedef struct {
    uint32_t dirs_visited;
    uint32_t files_seen;
    uint32_t tracks;
    uint32_t artists;
    uint32_t albums;
    uint32_t skipped_not_audio;
    uint32_t skipped_path_too_long;
    uint32_t skipped_too_deep;
} libidx_scan_stats_t;

uint32_t libidx_scan_arena_bytes(const libidx_scan_cfg_t *cfg);

/* Builds the index. Returns LIB_OK, or LIB_E_FULL if a limit was hit (stats
 * still reflect what was counted), or LIB_E_IO / LIB_E_ARG.
 * On success the temp file is removed when the io backend supports unlink. */
int libidx_scan(const libidx_scan_cfg_t *cfg, libidx_scan_stats_t *stats);

#endif /* LIBRARY_BUILD_H */
