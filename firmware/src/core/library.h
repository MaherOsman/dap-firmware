/*
 * library — the artist→album→track model and drill-down navigation.
 *
 * Extracted from the simulator's sim_main.c, where it was tangled with SDL
 * event handling. Pure logic: no HAL, no SDL, no file I/O. You hand it a
 * flat array of track metadata, it groups that into a tree and answers
 * "what rows should the screen show right now".
 *
 * Where the metadata comes from is deliberately not this module's problem.
 * Today it's a hardcoded array; later it will be a tag scanner reading ID3
 * and Vorbis comments off the SD card. The model does not change.
 *
 * Memory: the tree stores *indices* into the track array, never copies of
 * names. To get an artist's name you look up any track that belongs to it.
 * That costs 2 bytes per node instead of 48, and — more importantly — the
 * whole tree serialises cleanly to a file later, when the library outgrows
 * RAM and has to live on the card as an index.
 */
#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#include "../ui/screen_library.h"

/* Ceilings. These bound a static allocation — raising them costs RAM
 * directly, so they are sized for the breadboard rig, not for a finished
 * device. See the note above about moving the index to the SD card. */
#define LIB_MAX_TRACKS          256
#define LIB_MAX_ARTISTS          64
#define LIB_MAX_ALBUMS_PER_ART   16
#define LIB_MAX_TRACKS_PER_ALB   64

typedef struct {
    char     artist[48];
    char     album[48];
    char     title[64];
    char     path[128];      /* path on the SD card, for the player */
    uint16_t track_no;
    uint16_t duration_s;
} track_meta_t;

typedef struct {
    uint16_t name_track;                        /* any track in this album */
    uint16_t track_idx[LIB_MAX_TRACKS_PER_ALB];
    uint8_t  count;
} album_node_t;

typedef struct {
    uint16_t     name_track;                    /* any track by this artist */
    album_node_t albums[LIB_MAX_ALBUMS_PER_ART];
    uint8_t      album_count;
} artist_node_t;

/*
 * Navigation state. Each level keeps its own selection and scroll offset,
 * so backing out of an album returns you to where you were in the artist
 * list rather than to the top. The simulator did this with six loose ints;
 * they are gathered here so a single struct can be saved or reset.
 */
typedef struct {
    lib_level_t level;
    int         artist_sel,  album_sel,  track_sel;
    int         artist_top,  album_top,  track_top;   /* scroll offsets */
} lib_nav_t;

typedef struct {
    const track_meta_t *tracks;      /* not owned — caller keeps this alive */
    int                 track_count;

    artist_node_t artists[LIB_MAX_ARTISTS];
    int           artist_count;
} library_t;

/*
 * Group `tracks` into the artist→album→track tree.
 *
 * `tracks` is borrowed, not copied: it must outlive the library_t. Tracks
 * beyond the ceilings above are silently dropped rather than failing — a
 * player that shows most of your music beats one that refuses to start.
 */
void library_build(library_t *lib, const track_meta_t *tracks, int count);

void lib_nav_init(lib_nav_t *nav);

/* Names, resolved through the index indirection. */
const char *lib_artist_name(const library_t *lib, int artist);
const char *lib_album_name(const library_t *lib, int artist, int album);

/* How many rows the current level has. */
int lib_row_count(const library_t *lib, const lib_nav_t *nav);

/*
 * Fill `rows` with what the current level should display, ready to hand
 * straight to screen_library_draw(). `playing` is the index of the track
 * currently playing, or -1 — rows containing it get is_current set, which
 * is how the play marker propagates up to the artist and album levels.
 *
 * Returns the number of rows written, capped at `max_rows`.
 *
 * The text pointers point into the caller's track array, so they stay
 * valid exactly as long as it does.
 */
int lib_build_rows(const library_t *lib, const lib_nav_t *nav,
                   lib_row_t *rows, int max_rows, int playing);

/* Header text for the current level: "Artists", the artist name, or the
 * album name. */
const char *lib_header(const library_t *lib, const lib_nav_t *nav);

/* Move the selection by `delta` rows, clamped, adjusting scroll to follow. */
void lib_move(const library_t *lib, lib_nav_t *nav, int delta);

/*
 * Descend into the selected row. ARTIST→ALBUM→TRACK.
 *
 * At TRACK level there is nowhere further to go, so this returns the index
 * of the selected track — that is the caller's cue to start playing it.
 * Returns -1 at every other level.
 */
int lib_descend(const library_t *lib, lib_nav_t *nav);

/* Back up one level. Returns false if already at ARTIST. */
bool lib_ascend(const library_t *lib, lib_nav_t *nav);

#endif /* LIBRARY_H */
