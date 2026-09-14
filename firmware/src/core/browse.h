/*
 * browse — artist→album→track navigation over the on-card index.
 *
 * Replaces the RAM-array browsing in library.c. The difference that shapes
 * everything else: this never materialises a row per track. It fills only
 * the rows that are on screen, so scrolling a 3000-track album costs one
 * page read and a fixed 1.5 KB of RAM.
 *
 * Two rules worth knowing before using it:
 *
 *   1. Rows OWN their text. A title read from the index lives in the page
 *      cache and is overwritten the next time you scroll — a borrowed
 *      pointer would quietly start showing a different track. browse_t
 *      holds a buffer per visible row and lib_row_t.text points into that.
 *      So the rows are valid until the next browse_fill_rows() call, and
 *      the browse_t must outlive them.
 *
 *   2. Activating a row returns a DECISION, not an action. At track level
 *      browse_activate() hands back the track and says BROWSE_PLAY; it does
 *      not start playback. Deciding what "play" means belongs to the caller,
 *      which is what lets a queue, shuffle, or a now-playing screen arrive
 *      later without touching this file.
 *
 * No HAL, no platform calls. Testable on the host against any index.
 */
#ifndef BROWSE_H
#define BROWSE_H

#include <stdbool.h>
#include <stdint.h>

#include "library_index.h"
#include "../ui/screen_library.h"

/* One text buffer per visible row. Sized from the layout, not guessed --
 * LIB_VISIBLE comes from theme.h via screen_library.h. The +4 is slack so a
 * layout change doesn't silently truncate the window. */
#define BROWSE_MAX_ROWS  (LIB_VISIBLE + 4)
#define BROWSE_TEXT_MAX  (LIB_TITLE_MAX + 1)

/* No track is playing. */
#define BROWSE_NOTHING_PLAYING  ((uint32_t)0xFFFFFFFFu)

typedef enum {
    BROWSE_NONE = 0,   /* nothing to activate (empty level)         */
    BROWSE_DESCENDED,  /* moved a level deeper                      */
    BROWSE_PLAY        /* a track was chosen; `out` is filled       */
} browse_result_t;

typedef struct {
    libidx_t   *idx;
    lib_level_t level;

    /* Per level, so backing out of an album returns you to where you were
     * in the artist list rather than to the top. */
    int artist_sel, album_sel, track_sel;
    int artist_top, album_top, track_top;

    /* Row text storage — see rule 1 above. */
    char text[BROWSE_MAX_ROWS][BROWSE_TEXT_MAX];
    char header[BROWSE_TEXT_MAX];
} browse_t;

/* `idx` may be NULL or an empty library; every call stays safe and reports
 * zero rows. */
void browse_init(browse_t *br, libidx_t *idx);

lib_level_t browse_level(const browse_t *br);
int         browse_row_count(const browse_t *br);
int         browse_selected(const browse_t *br);
int         browse_scroll_top(const browse_t *br);

/* Move the selection by `delta` rows, clamped, scrolling to follow. */
void browse_move(browse_t *br, int delta);

/*
 * Fills `rows` with the visible window, starting at browse_scroll_top().
 * Returns how many were written (at most `max`, at most LIB_VISIBLE).
 *
 * `playing` is the global track index currently playing, or
 * BROWSE_NOTHING_PLAYING. Rows containing it get is_current set, which is
 * how the play marker propagates up to the album and artist levels.
 *
 * Pass the result straight to screen_library_draw_window() together with
 * browse_row_count(), browse_selected() and browse_scroll_top().
 */
int browse_fill_rows(browse_t *br, lib_row_t *rows, int max, uint32_t playing);

/* "Artists", the artist's name, or the album's name. Valid until the next
 * call. */
const char *browse_header(browse_t *br);

/*
 * Activate the selected row. Descends a level, or at track level fills
 * `out` (may be NULL) and `out_index` (may be NULL) with the chosen track
 * and returns BROWSE_PLAY.
 */
browse_result_t browse_activate(browse_t *br, lib_track_t *out,
                                uint32_t *out_index);

/* Back up one level. Returns false if already at the artist list. */
bool browse_back(browse_t *br);

#endif /* BROWSE_H */
