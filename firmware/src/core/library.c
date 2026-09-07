/*
 * library — see library.h.
 *
 * Ported from sim_main.c lines 190-228 (build_library) and 393-535 (the
 * level transitions and row building), with the SDL event handling removed
 * and the string copies replaced by indices.
 */
#include "library.h"

#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static bool same_str(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* Does this album contain `track`? Used to propagate the play marker up. */
static bool album_has_track(const album_node_t *ab, int track)
{
    for (int t = 0; t < ab->count; t++) {
        if (ab->track_idx[t] == (uint16_t)track) {
            return true;
        }
    }
    return false;
}

static bool artist_has_track(const artist_node_t *ar, int track)
{
    for (int b = 0; b < ar->album_count; b++) {
        if (album_has_track(&ar->albums[b], track)) {
            return true;
        }
    }
    return false;
}

/* ── build ───────────────────────────────────────────────────────────── */

void library_build(library_t *lib, const track_meta_t *tracks, int count)
{
    memset(lib, 0, sizeof(*lib));
    lib->tracks = tracks;

    if (count > LIB_MAX_TRACKS) {
        count = LIB_MAX_TRACKS;
    }
    lib->track_count = count;

    for (int i = 0; i < count; i++) {
        const char *artist = tracks[i].artist;
        const char *album  = tracks[i].album;

        /* find or create the artist */
        int ai = -1;
        for (int a = 0; a < lib->artist_count; a++) {
            if (same_str(lib->tracks[lib->artists[a].name_track].artist,
                         artist)) {
                ai = a;
                break;
            }
        }
        if (ai < 0) {
            if (lib->artist_count >= LIB_MAX_ARTISTS) {
                continue;          /* full — drop the track, keep going */
            }
            ai = lib->artist_count++;
            lib->artists[ai].name_track  = (uint16_t)i;
            lib->artists[ai].album_count = 0;
        }

        /* find or create the album under that artist */
        artist_node_t *ar = &lib->artists[ai];
        int bi = -1;
        for (int b = 0; b < ar->album_count; b++) {
            if (same_str(lib->tracks[ar->albums[b].name_track].album, album)) {
                bi = b;
                break;
            }
        }
        if (bi < 0) {
            if (ar->album_count >= LIB_MAX_ALBUMS_PER_ART) {
                continue;
            }
            bi = ar->album_count++;
            ar->albums[bi].name_track = (uint16_t)i;
            ar->albums[bi].count      = 0;
        }

        album_node_t *ab = &ar->albums[bi];
        if (ab->count < LIB_MAX_TRACKS_PER_ALB) {
            ab->track_idx[ab->count++] = (uint16_t)i;
        }
    }
}

void lib_nav_init(lib_nav_t *nav)
{
    memset(nav, 0, sizeof(*nav));
    nav->level = LIB_LEVEL_ARTIST;
}

/* ── name lookup ─────────────────────────────────────────────────────── */

const char *lib_artist_name(const library_t *lib, int artist)
{
    if (artist < 0 || artist >= lib->artist_count) {
        return "";
    }
    return lib->tracks[lib->artists[artist].name_track].artist;
}

const char *lib_album_name(const library_t *lib, int artist, int album)
{
    if (artist < 0 || artist >= lib->artist_count) {
        return "";
    }
    const artist_node_t *ar = &lib->artists[artist];
    if (album < 0 || album >= ar->album_count) {
        return "";
    }
    return lib->tracks[ar->albums[album].name_track].album;
}

/* ── rows ────────────────────────────────────────────────────────────── */

int lib_row_count(const library_t *lib, const lib_nav_t *nav)
{
    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        return lib->artist_count;

    case LIB_LEVEL_ALBUM:
        if (nav->artist_sel < 0 || nav->artist_sel >= lib->artist_count) {
            return 0;
        }
        return lib->artists[nav->artist_sel].album_count;

    default: /* LIB_LEVEL_TRACK */
        if (nav->artist_sel < 0 || nav->artist_sel >= lib->artist_count) {
            return 0;
        }
        {
            const artist_node_t *ar = &lib->artists[nav->artist_sel];
            if (nav->album_sel < 0 || nav->album_sel >= ar->album_count) {
                return 0;
            }
            return ar->albums[nav->album_sel].count;
        }
    }
}

int lib_build_rows(const library_t *lib, const lib_nav_t *nav,
                   lib_row_t *rows, int max_rows, int playing)
{
    int n = lib_row_count(lib, nav);
    if (n > max_rows) {
        n = max_rows;
    }

    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        for (int a = 0; a < n; a++) {
            rows[a].text       = lib_artist_name(lib, a);
            rows[a].has_sub    = true;
            rows[a].is_current = (playing >= 0) &&
                                 artist_has_track(&lib->artists[a], playing);
        }
        break;

    case LIB_LEVEL_ALBUM: {
        const artist_node_t *ar = &lib->artists[nav->artist_sel];
        for (int b = 0; b < n; b++) {
            rows[b].text       = lib_album_name(lib, nav->artist_sel, b);
            rows[b].has_sub    = true;
            rows[b].is_current = (playing >= 0) &&
                                 album_has_track(&ar->albums[b], playing);
        }
        break;
    }

    default: { /* LIB_LEVEL_TRACK */
        const artist_node_t *ar = &lib->artists[nav->artist_sel];
        const album_node_t  *ab = &ar->albums[nav->album_sel];
        for (int t = 0; t < n; t++) {
            int idx = ab->track_idx[t];
            rows[t].text       = lib->tracks[idx].title;
            rows[t].has_sub    = false;   /* a track is a leaf — no chevron */
            rows[t].is_current = (idx == playing);
        }
        break;
    }
    }

    return n;
}

const char *lib_header(const library_t *lib, const lib_nav_t *nav)
{
    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        return "Artists";
    case LIB_LEVEL_ALBUM:
        return lib_artist_name(lib, nav->artist_sel);
    default:
        return lib_album_name(lib, nav->artist_sel, nav->album_sel);
    }
}

/* ── navigation ──────────────────────────────────────────────────────── */

/* The selection and scroll for whichever level we are on. Returning
 * pointers keeps move/descend/ascend from repeating a three-way switch
 * for every field they touch. */
static void nav_cursor(lib_nav_t *nav, int **sel, int **top)
{
    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        *sel = &nav->artist_sel;
        *top = &nav->artist_top;
        break;
    case LIB_LEVEL_ALBUM:
        *sel = &nav->album_sel;
        *top = &nav->album_top;
        break;
    default:
        *sel = &nav->track_sel;
        *top = &nav->track_top;
        break;
    }
}

void lib_move(const library_t *lib, lib_nav_t *nav, int delta)
{
    int *sel, *top;
    nav_cursor(nav, &sel, &top);

    int n = lib_row_count(lib, nav);
    if (n <= 0) {
        *sel = 0;
        *top = 0;
        return;
    }

    *sel += delta;
    if (*sel < 0)  *sel = 0;
    if (*sel >= n) *sel = n - 1;

    /* scroll_top is screen_library's to compute — one definition */
    lib_clamp_scroll(*sel, n, top);
}

int lib_descend(const library_t *lib, lib_nav_t *nav)
{
    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        if (lib->artist_count == 0) {
            return -1;
        }
        nav->level      = LIB_LEVEL_ALBUM;
        nav->album_sel  = 0;
        nav->album_top  = 0;
        return -1;

    case LIB_LEVEL_ALBUM:
        if (lib->artists[nav->artist_sel].album_count == 0) {
            return -1;
        }
        nav->level     = LIB_LEVEL_TRACK;
        nav->track_sel = 0;
        nav->track_top = 0;
        return -1;

    default: { /* LIB_LEVEL_TRACK — the bottom; hand back what to play */
        const artist_node_t *ar = &lib->artists[nav->artist_sel];
        const album_node_t  *ab = &ar->albums[nav->album_sel];
        if (nav->track_sel < 0 || nav->track_sel >= ab->count) {
            return -1;
        }
        return ab->track_idx[nav->track_sel];
    }
    }
}

bool lib_ascend(const library_t *lib, lib_nav_t *nav)
{
    (void)lib;   /* selection and scroll are already remembered per level */

    switch (nav->level) {
    case LIB_LEVEL_ARTIST:
        return false;
    case LIB_LEVEL_ALBUM:
        nav->level = LIB_LEVEL_ARTIST;
        return true;
    default:
        nav->level = LIB_LEVEL_ALBUM;
        return true;
    }
}
