/*
 * playqueue — what is playing, what plays next, and whether it is paused.
 *
 * This is the piece that makes auto-advance possible without every screen
 * knowing about playback. It owns the *policy*: given that a track finished,
 * which track (if any) should start. It does not own the *mechanism* — it
 * never opens a file, never touches the SAI, never calls the platform. It
 * hands back a track index and the caller decides what to do with it, the
 * same contract browse_activate() uses.
 *
 * That split is what lets repeat, shuffle, a play queue, or "next album"
 * arrive later as changes to this one file, with tests, instead of as more
 * branches inside main's loop.
 *
 * Scope of "next": the next track in the same album. A library is browsed by
 * album, so an album is the natural unit to play through. Crossing into the
 * next album on its own would surprise more often than it helps; when that is
 * wanted it becomes a pq_repeat_t mode rather than a change of default.
 */
#ifndef PLAYQUEUE_H
#define PLAYQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "library_index.h"

#define PQ_NO_TRACK  LIBIDX_NONE

typedef enum {
    PQ_STOPPED = 0,
    PQ_PLAYING,
    PQ_PAUSED
} pq_state_t;

typedef enum {
    PQ_REPEAT_OFF = 0,   /* stop at the end of the album      */
    PQ_REPEAT_ONE,       /* play the same track again         */
    PQ_REPEAT_ALL        /* wrap to the first track of the album */
} pq_repeat_t;

typedef struct {
    libidx_t   *idx;
    uint32_t    current;      /* global track index, or PQ_NO_TRACK */
    pq_state_t  state;
    pq_repeat_t repeat;
} playqueue_t;

/* `idx` may be NULL; every call stays safe and reports nothing playing. */
void pq_init(playqueue_t *pq, libidx_t *idx);

void        pq_set_repeat(playqueue_t *pq, pq_repeat_t mode);
pq_repeat_t pq_repeat(const playqueue_t *pq);

/* Begin playing a global track index. Returns false (and changes nothing)
 * if the index is out of range. */
bool pq_start(playqueue_t *pq, uint32_t track);

void pq_stop(playqueue_t *pq);

uint32_t   pq_current(const playqueue_t *pq);
pq_state_t pq_state(const playqueue_t *pq);

/* True while a track is loaded, paused or not. */
bool pq_is_active(const playqueue_t *pq);

/* Flips between PQ_PLAYING and PQ_PAUSED. Returns true if now paused.
 * Does nothing when stopped. */
bool pq_toggle_pause(playqueue_t *pq);

/*
 * Skip. Both stay inside the current album.
 *
 * pq_next: at the last track, PQ_REPEAT_ALL wraps and anything else stops.
 * pq_prev: at the first track, stays there (returns it again) — matching
 *          what every player does, so a stray back-press doesn't stop
 *          playback.
 *
 * Return true and set *out to the track to play; false means nothing to
 * play and the queue has stopped itself. `out` may be NULL.
 */
bool pq_next(playqueue_t *pq, uint32_t *out);
bool pq_prev(playqueue_t *pq, uint32_t *out);

/*
 * The platform reported that the current track finished. Same as pq_next
 * except that PQ_REPEAT_ONE restarts the same track — a deliberate skip
 * should still move on, so the two cases cannot share one function.
 */
bool pq_track_finished(playqueue_t *pq, uint32_t *out);

#endif /* PLAYQUEUE_H */
