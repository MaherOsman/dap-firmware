/* playqueue.c — playback policy over the index. No HAL, no platform calls. */

#include "playqueue.h"

#include <string.h>

/* Album bounds for the current track. Returns false when nothing is loaded
 * or the index cannot place the track. */
static bool album_span(const playqueue_t *pq, uint32_t *first, uint32_t *count)
{
    uint32_t album;

    if (pq->idx == NULL || pq->current == PQ_NO_TRACK) return false;

    album = libidx_album_of_track(pq->idx, pq->current);
    if (album == LIBIDX_NONE) return false;

    *first = libidx_album_track_first(pq->idx, album);
    *count = libidx_album_track_count(pq->idx, album);
    return (*count > 0u);
}

static bool emit(playqueue_t *pq, uint32_t track, uint32_t *out)
{
    pq->current = track;
    pq->state = PQ_PLAYING;
    if (out != NULL) *out = track;
    return true;
}

void pq_init(playqueue_t *pq, libidx_t *idx)
{
    if (pq == NULL) return;
    memset(pq, 0, sizeof(*pq));
    pq->idx = idx;
    pq->current = PQ_NO_TRACK;
    pq->state = PQ_STOPPED;
    pq->repeat = PQ_REPEAT_OFF;
}

void pq_set_repeat(playqueue_t *pq, pq_repeat_t mode)
{
    if (pq != NULL) pq->repeat = mode;
}

pq_repeat_t pq_repeat(const playqueue_t *pq)
{
    return (pq != NULL) ? pq->repeat : PQ_REPEAT_OFF;
}

bool pq_start(playqueue_t *pq, uint32_t track)
{
    if (pq == NULL || pq->idx == NULL) return false;
    if (track >= libidx_track_count(pq->idx)) return false;

    pq->current = track;
    pq->state = PQ_PLAYING;
    return true;
}

void pq_stop(playqueue_t *pq)
{
    if (pq == NULL) return;
    pq->current = PQ_NO_TRACK;
    pq->state = PQ_STOPPED;
}

uint32_t pq_current(const playqueue_t *pq)
{
    return (pq != NULL) ? pq->current : PQ_NO_TRACK;
}

pq_state_t pq_state(const playqueue_t *pq)
{
    return (pq != NULL) ? pq->state : PQ_STOPPED;
}

bool pq_is_active(const playqueue_t *pq)
{
    return (pq != NULL) && (pq->state != PQ_STOPPED);
}

bool pq_toggle_pause(playqueue_t *pq)
{
    if (pq == NULL || pq->state == PQ_STOPPED) return false;
    pq->state = (pq->state == PQ_PLAYING) ? PQ_PAUSED : PQ_PLAYING;
    return pq->state == PQ_PAUSED;
}

bool pq_next(playqueue_t *pq, uint32_t *out)
{
    uint32_t first, count;

    if (pq == NULL) return false;
    if (!album_span(pq, &first, &count)) {
        pq_stop(pq);
        return false;
    }

    if (pq->current + 1u < first + count) {
        return emit(pq, pq->current + 1u, out);
    }

    /* end of the album */
    if (pq->repeat == PQ_REPEAT_ALL) {
        return emit(pq, first, out);
    }

    pq_stop(pq);
    return false;
}

bool pq_prev(playqueue_t *pq, uint32_t *out)
{
    uint32_t first, count;

    if (pq == NULL) return false;
    if (!album_span(pq, &first, &count)) {
        pq_stop(pq);
        return false;
    }

    if (pq->current > first) {
        return emit(pq, pq->current - 1u, out);
    }

    /* At the first track, restart it rather than stopping — a stray back
     * press should not end playback. */
    return emit(pq, first, out);
}

bool pq_track_finished(playqueue_t *pq, uint32_t *out)
{
    if (pq == NULL) return false;

    /* Repeat-one only applies when a track ends on its own; pressing skip
     * must still move on, which is why this is not just pq_next(). */
    if (pq->repeat == PQ_REPEAT_ONE && pq->current != PQ_NO_TRACK) {
        return emit(pq, pq->current, out);
    }
    return pq_next(pq, out);
}
