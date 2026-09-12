/* platform_library.c — scan the card if needed, then open the index. */

#include "platform_library.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "library_build.h"
#include "platform_libio.h"

/* Scan limits. These set the scratch size, so they are a RAM decision, not a
 * capacity wish: raising max_tracks by 1000 costs 16 KB during the scan.
 * ~66 KB total at these numbers, freed the moment the scan finishes. */
#define SCAN_MAX_TRACKS   2000u
#define SCAN_MAX_ARTISTS   128u
#define SCAN_MAX_ALBUMS    512u
#define SCAN_POOL_BYTES  (24u * 1024u)

/* Resident: artist table + album table + name pool + one 2.5 KB page cache.
 * Held for as long as the library is open. */
#define INDEX_ARENA_BYTES (20u * 1024u)

static libidx_t g_idx;
static lib_io_t  g_lio;
static lib_dir_t g_ldir;
static int       g_open;

static uint8_t g_index_arena[INDEX_ARENA_BYTES];
static uint8_t g_scan_arena[
      ((SCAN_MAX_TRACKS * 16u) + (SCAN_MAX_ARTISTS * 4u)
     + (SCAN_MAX_ALBUMS * 8u) + (SCAN_MAX_ARTISTS * 8u)
     + (SCAN_MAX_ALBUMS * 8u) + SCAN_POOL_BYTES + 1024u)];

static const char *err_name(int rc)
{
    switch (rc) {
    case LIB_OK:       return "OK";
    case LIB_E_IO:     return "IO";
    case LIB_E_FORMAT: return "FORMAT";
    case LIB_E_NOMEM:  return "NOMEM";
    case LIB_E_RANGE:  return "RANGE";
    case LIB_E_ARG:    return "ARG";
    case LIB_E_FULL:   return "FULL";
    default:           return "?";
    }
}

static void on_progress(void *ctx, int phase, uint32_t done, uint32_t total)
{
    (void)ctx;
    if (phase == LIB_PHASE_SCAN) {
        printf("  scan: %lu tracks\r\n", (unsigned long)done);
    } else if (total != 0u && done == total) {
        printf("  write: %lu tracks\r\n", (unsigned long)done);
    }
}

static void fill_cfg(libidx_scan_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->io = &g_lio;
    cfg->dir = &g_ldir;
    cfg->root = DAP_MUSIC_ROOT;
    cfg->build_id = DAP_INDEX_BUILD_ID;
    cfg->max_tracks = SCAN_MAX_TRACKS;
    cfg->max_artists = SCAN_MAX_ARTISTS;
    cfg->max_albums = SCAN_MAX_ALBUMS;
    cfg->pool_bytes = SCAN_POOL_BYTES;
    cfg->arena = g_scan_arena;
    cfg->arena_len = (uint32_t)sizeof(g_scan_arena);
    cfg->progress = on_progress;
}

static int try_open(void)
{
    int rc = libidx_open(&g_idx, &g_lio, LIB_INDEX_PATH,
                         g_index_arena, (uint32_t)sizeof(g_index_arena));
    if (rc == LIB_E_NOMEM) {
        printf("index: arena too small, need %lu of %lu bytes\r\n",
               (unsigned long)g_idx.required_bytes,
               (unsigned long)sizeof(g_index_arena));
    }
    g_open = (rc == LIB_OK);
    return rc;
}

static int do_scan(void)
{
    libidx_scan_cfg_t cfg;
    libidx_scan_stats_t st;
    uint32_t t0, ms;
    int rc;

    printf("index: scanning %s ...\r\n", DAP_MUSIC_ROOT);
    fill_cfg(&cfg);

    if (sizeof(g_scan_arena) < libidx_scan_arena_bytes(&cfg)) {
        printf("index: scan arena too small (%lu < %lu)\r\n",
               (unsigned long)sizeof(g_scan_arena),
               (unsigned long)libidx_scan_arena_bytes(&cfg));
        return LIB_E_NOMEM;
    }

    t0 = HAL_GetTick();
    rc = libidx_scan(&cfg, &st);
    ms = HAL_GetTick() - t0;

    printf("index: %s in %lu ms — %lu tracks, %lu albums, %lu artists, "
           "%lu dirs\r\n",
           err_name(rc), (unsigned long)ms,
           (unsigned long)st.tracks, (unsigned long)st.albums,
           (unsigned long)st.artists, (unsigned long)st.dirs_visited);
    printf("index: skipped %lu non-audio, %lu path-too-long, %lu too-deep\r\n",
           (unsigned long)st.skipped_not_audio,
           (unsigned long)st.skipped_path_too_long,
           (unsigned long)st.skipped_too_deep);

    if (rc == LIB_E_FULL) {
        printf("index: a scan limit was hit — raise SCAN_MAX_* and rebuild\r\n");
    }
    if (rc == LIB_E_IO) {
        printf("index: FatFs said %s\r\n",
               plat_libio_result_name(plat_libio_last_result()));
    }
    printf("index: peak handles %u files / %u dirs\r\n",
           plat_libio_peak_files(), plat_libio_peak_dirs());

    return rc;
}

int dap_library_init(int force_rescan)
{
    int rc;

    plat_libio_init(&g_lio, &g_ldir);
    g_open = 0;

    if (!force_rescan) {
        rc = try_open();
        if (rc == LIB_OK) {
            if (g_idx.hdr.build_id == DAP_INDEX_BUILD_ID) {
                printf("index: loaded — %lu artists, %lu albums, %lu tracks\r\n",
                       (unsigned long)libidx_artist_count(&g_idx),
                       (unsigned long)libidx_album_count(&g_idx),
                       (unsigned long)libidx_track_count(&g_idx));
                return LIB_OK;
            }
            printf("index: build_id %lu != %lu, rescanning\r\n",
                   (unsigned long)g_idx.hdr.build_id,
                   (unsigned long)DAP_INDEX_BUILD_ID);
            libidx_close(&g_idx);
            g_open = 0;
        } else {
            printf("index: no usable index (%s), scanning\r\n", err_name(rc));
        }
    }

    rc = do_scan();
    if (rc != LIB_OK) return rc;

    rc = try_open();
    if (rc != LIB_OK) {
        /* The scan claimed success but the file will not open — that is a
         * builder bug, not a card problem, and it must not be silent. */
        printf("index: BUILT BUT WILL NOT OPEN (%s)\r\n", err_name(rc));
        return rc;
    }

    printf("index: ready — %lu artists, %lu albums, %lu tracks\r\n",
           (unsigned long)libidx_artist_count(&g_idx),
           (unsigned long)libidx_album_count(&g_idx),
           (unsigned long)libidx_track_count(&g_idx));
    return LIB_OK;
}

libidx_t *dap_library(void)
{
    return g_open ? &g_idx : 0;
}
