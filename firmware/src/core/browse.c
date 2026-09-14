/* browse.c — windowed artist/album/track navigation over libidx. */

#include "browse.h"

#include <string.h>

/* The page cache must hold a whole screenful plus the lead, or scrolling
 * reloads it every frame. Only this file sees both constants, so the check
 * lives here. */
#if (LIB_PAGE_TRACKS) < (LIB_VISIBLE + LIB_PAGE_LEAD + 2)
#error "LIB_PAGE_TRACKS is too small for LIB_VISIBLE — the page cache will thrash on every redraw"
#endif

/* ------------------------------------------------------------- helpers */

static void copy_text(char *dst, const char *src)
{
    uint32_t i = 0;
    if (src != NULL) {
        while (src[i] != '\0' && i + 1u < BROWSE_TEXT_MAX) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int *sel_ptr(browse_t *br)
{
    switch (br->level) {
    case LIB_LEVEL_ARTIST: return &br->artist_sel;
    case LIB_LEVEL_ALBUM:  return &br->album_sel;
    default:               return &br->track_sel;
    }
}

static int *top_ptr(browse_t *br)
{
    switch (br->level) {
    case LIB_LEVEL_ARTIST: return &br->artist_top;
    case LIB_LEVEL_ALBUM:  return &br->album_top;
    default:               return &br->track_top;
    }
}

/* 1 when a pinned row occupies row 0 of the current level, else 0. Only the
 * artist list has one, so every other level maps rows to items directly. */
static int pin_offset(const browse_t *br)
{
    return (br->pinned != NULL && br->level == LIB_LEVEL_ARTIST) ? 1 : 0;
}

/* The artist the selection refers to, with the pinned row accounted for.
 * Returns LIBIDX_NONE when the pinned row itself is selected. */
static uint32_t sel_artist(const browse_t *br)
{
    int i = br->artist_sel - ((br->pinned != NULL) ? 1 : 0);
    if (i < 0) return LIBIDX_NONE;
    return (uint32_t)i;
}

/* Global album index of the selected artist's Nth album. */
static uint32_t cur_album(const browse_t *br)
{
    uint32_t artist;

    if (br->idx == NULL) return LIBIDX_NONE;
    artist = sel_artist(br);
    if (artist == LIBIDX_NONE) return LIBIDX_NONE;
    return libidx_artist_album(br->idx, artist, (uint32_t)br->album_sel);
}

/* Does this album contain the playing track? */
static bool album_is_playing(const libidx_t *idx, uint32_t album,
                             uint32_t playing)
{
    uint32_t first, count;

    if (playing == BROWSE_NOTHING_PLAYING) return false;
    first = libidx_album_track_first(idx, album);
    count = libidx_album_track_count(idx, album);
    return (playing >= first) && (playing < first + count);
}

/* Does this artist own the album the playing track is in? */
static bool artist_is_playing(const libidx_t *idx, uint32_t artist,
                              uint32_t playing)
{
    uint32_t album;

    if (playing == BROWSE_NOTHING_PLAYING) return false;
    album = libidx_album_of_track(idx, playing);
    if (album == LIBIDX_NONE) return false;
    return libidx_album_artist(idx, album) == artist;
}

/* --------------------------------------------------------------- api */

void browse_set_pinned(browse_t *br, const char *label)
{
    if (br == NULL) return;
    br->pinned = label;
}

void browse_init(browse_t *br, libidx_t *idx)
{
    if (br == NULL) return;
    memset(br, 0, sizeof(*br));
    br->idx = idx;
    br->level = LIB_LEVEL_ARTIST;
}

lib_level_t browse_level(const browse_t *br)
{
    return (br != NULL) ? br->level : LIB_LEVEL_ARTIST;
}

int browse_row_count(const browse_t *br)
{
    uint32_t album;

    if (br == NULL || br->idx == NULL) return 0;

    switch (br->level) {
    case LIB_LEVEL_ARTIST:
        return (int)libidx_artist_count(br->idx) + pin_offset(br);

    case LIB_LEVEL_ALBUM: {
        uint32_t artist = sel_artist(br);
        if (artist == LIBIDX_NONE) return 0;
        return (int)libidx_artist_album_count(br->idx, artist);
    }

    default:
        album = cur_album(br);
        if (album == LIBIDX_NONE) return 0;
        return (int)libidx_album_track_count(br->idx, album);
    }
}

int browse_selected(const browse_t *br)
{
    if (br == NULL) return 0;
    switch (br->level) {
    case LIB_LEVEL_ARTIST: return br->artist_sel;
    case LIB_LEVEL_ALBUM:  return br->album_sel;
    default:               return br->track_sel;
    }
}

int browse_scroll_top(const browse_t *br)
{
    if (br == NULL) return 0;
    switch (br->level) {
    case LIB_LEVEL_ARTIST: return br->artist_top;
    case LIB_LEVEL_ALBUM:  return br->album_top;
    default:               return br->track_top;
    }
}

void browse_move(browse_t *br, int delta)
{
    int n, *sel, *top;

    if (br == NULL) return;

    n = browse_row_count(br);
    sel = sel_ptr(br);
    top = top_ptr(br);

    if (n <= 0) {
        *sel = 0;
        *top = 0;
        return;
    }

    *sel += delta;
    if (*sel < 0)  *sel = 0;
    if (*sel >= n) *sel = n - 1;

    lib_clamp_scroll(*sel, n, top);
}

const char *browse_header(browse_t *br)
{
    uint32_t album;

    if (br == NULL) return "";
    if (br->idx == NULL) return "Library";

    switch (br->level) {
    case LIB_LEVEL_ARTIST:
        copy_text(br->header, "Artists");
        break;

    case LIB_LEVEL_ALBUM: {
        uint32_t artist = sel_artist(br);
        copy_text(br->header, (artist == LIBIDX_NONE)
                                  ? ""
                                  : libidx_artist_name(br->idx, artist));
        break;
    }

    default:
        album = cur_album(br);
        copy_text(br->header, (album == LIBIDX_NONE)
                                  ? ""
                                  : libidx_album_name(br->idx, album));
        break;
    }
    return br->header;
}

int browse_fill_rows(browse_t *br, lib_row_t *rows, int max, uint32_t playing)
{
    int n, start, count, i;
    uint32_t album;

    if (br == NULL || rows == NULL || max <= 0 || br->idx == NULL) return 0;

    n = browse_row_count(br);
    if (n <= 0) return 0;

    start = browse_scroll_top(br);
    if (start < 0) start = 0;
    if (start >= n) start = n - 1;

    count = n - start;
    if (count > max)             count = max;
    if (count > LIB_VISIBLE)     count = LIB_VISIBLE;
    if (count > BROWSE_MAX_ROWS) count = BROWSE_MAX_ROWS;

    for (i = 0; i < count; i++) {
        uint32_t abs = (uint32_t)(start + i);

        rows[i].text = br->text[i];
        rows[i].has_sub = true;
        rows[i].is_current = false;
        rows[i].is_pinned = false;

        switch (br->level) {
        case LIB_LEVEL_ARTIST: {
            int off = pin_offset(br);
            if (off != 0 && abs == 0u) {
                /* The pinned row is chrome: no chevron, no play marker, and
                 * the screen draws it in a quieter colour so the artists
                 * stay the thing you are looking at. */
                copy_text(br->text[i], br->pinned);
                rows[i].has_sub = false;
                rows[i].is_pinned = true;
                break;
            }
            {
                uint32_t a = abs - (uint32_t)off;
                copy_text(br->text[i], libidx_artist_name(br->idx, a));
                rows[i].is_current = artist_is_playing(br->idx, a, playing);
            }
            break;
        }

        case LIB_LEVEL_ALBUM: {
            uint32_t artist = sel_artist(br);
            uint32_t g = (artist == LIBIDX_NONE)
                       ? LIBIDX_NONE
                       : libidx_artist_album(br->idx, artist, abs);
            if (g == LIBIDX_NONE) {
                copy_text(br->text[i], "");
            } else {
                copy_text(br->text[i], libidx_album_name(br->idx, g));
                rows[i].is_current = album_is_playing(br->idx, g, playing);
            }
            break;
        }

        default: {
            lib_track_t t;
            album = cur_album(br);
            /* A track is a leaf: no chevron, and the play marker goes here
             * rather than on the row's right edge. */
            rows[i].has_sub = false;

            if (album == LIBIDX_NONE ||
                libidx_album_track(br->idx, album, abs, &t) != LIB_OK) {
                /* A failed page read must not leave the previous track's
                 * title on screen under a new row's index. */
                copy_text(br->text[i], "...");
            } else {
                copy_text(br->text[i], t.title);
                rows[i].is_current =
                    (playing != BROWSE_NOTHING_PLAYING) &&
                    (playing == libidx_album_track_first(br->idx, album) + abs);
            }
            break;
        }
        }
    }

    return count;
}

browse_result_t browse_activate(browse_t *br, lib_track_t *out,
                                uint32_t *out_index)
{
    uint32_t album;

    if (br == NULL || br->idx == NULL) return BROWSE_NONE;
    if (browse_row_count(br) <= 0)     return BROWSE_NONE;

    switch (br->level) {
    case LIB_LEVEL_ARTIST:
        if (pin_offset(br) != 0 && br->artist_sel == 0) {
            return BROWSE_PINNED;
        }
        br->level = LIB_LEVEL_ALBUM;
        /* A fresh list starts at the top; keeping a stale selection from a
         * different artist would land somewhere arbitrary. */
        br->album_sel = 0;
        br->album_top = 0;
        return BROWSE_DESCENDED;

    case LIB_LEVEL_ALBUM:
        br->level = LIB_LEVEL_TRACK;
        br->track_sel = 0;
        br->track_top = 0;
        return BROWSE_DESCENDED;

    default:
        album = cur_album(br);
        if (album == LIBIDX_NONE) return BROWSE_NONE;

        if (out != NULL) {
            if (libidx_album_track(br->idx, album, (uint32_t)br->track_sel,
                                   out) != LIB_OK) {
                return BROWSE_NONE;
            }
        }
        if (out_index != NULL) {
            *out_index = libidx_album_track_first(br->idx, album)
                       + (uint32_t)br->track_sel;
        }
        return BROWSE_PLAY;
    }
}

bool browse_back(browse_t *br)
{
    if (br == NULL) return false;

    switch (br->level) {
    case LIB_LEVEL_TRACK:
        br->level = LIB_LEVEL_ALBUM;
        return true;
    case LIB_LEVEL_ALBUM:
        br->level = LIB_LEVEL_ARTIST;
        return true;
    default:
        return false;
    }
}
