#include "player.h"

void player_init(player_t *p)
{
    p->state          = PLAYER_STOPPED;
    p->track_frames   = 0;
    p->frames_played  = 0;
    p->sample_rate    = 0;
    p->volume         = 60;
    p->underruns      = 0;
    p->file_exhausted = false;
}

void player_open(player_t *p, uint32_t total_frames, uint32_t sample_rate)
{
    p->track_frames   = total_frames;
    p->sample_rate    = sample_rate;
    p->frames_played  = 0;
    p->file_exhausted = false;
    p->state          = PLAYER_BUFFERING;
}

void player_tick(player_t *p, uint8_t buffer_pct)
{
    switch (p->state) {
    case PLAYER_BUFFERING:
        /* Either we filled the preroll, or the file is short enough that it
         * will never fill it — in that case start anyway rather than hang. */
        if (buffer_pct >= PLAYER_PREROLL_PCT || p->file_exhausted) {
            p->state = PLAYER_PLAYING;
        }
        break;
    case PLAYER_PLAYING:
        if (p->file_exhausted && buffer_pct == 0) {
            p->state = PLAYER_EOF;
        }
        break;
    default:
        break;
    }
}

bool player_needs_refill(const player_t *p, uint8_t buffer_pct)
{
    if (p->file_exhausted) {
        return false;
    }
    if (p->state == PLAYER_STOPPED || p->state == PLAYER_EOF) {
        return false;
    }
    /* While prerolling we want the buffer as full as we can get it; while
     * playing we top up as soon as it drops under half. Keeping the reader
     * busy below 50% is what gives the SD card time to survive a slow
     * sector without the DAC ever noticing. */
    if (p->state == PLAYER_BUFFERING) {
        return buffer_pct < PLAYER_PREROLL_PCT;
    }
    return buffer_pct < PLAYER_REFILL_PCT;
}

bool player_output_enabled(const player_t *p)
{
    return p->state == PLAYER_PLAYING;
}

void player_play_pause(player_t *p)
{
    if (p->state == PLAYER_PLAYING) {
        p->state = PLAYER_PAUSED;
    } else if (p->state == PLAYER_PAUSED) {
        /* Resume straight to PLAYING: the buffer is still full from before,
         * so re-prerolling would just add a pointless gap. */
        p->state = PLAYER_PLAYING;
    }
}

void player_stop(player_t *p)
{
    p->state         = PLAYER_STOPPED;
    p->frames_played = 0;
}

void player_frames_consumed(player_t *p, uint32_t frames)
{
    if (p->state != PLAYER_PLAYING) {
        return;
    }
    p->frames_played += frames;
    if (p->track_frames && p->frames_played > p->track_frames) {
        p->frames_played = p->track_frames;
    }
}

void player_underrun(player_t *p)
{
    if (p->state == PLAYER_PLAYING) {
        p->underruns++;
    }
}

void player_set_exhausted(player_t *p, bool exhausted)
{
    p->file_exhausted = exhausted;
}

uint8_t player_set_volume(player_t *p, int delta)
{
    int v = (int)p->volume + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    p->volume = (uint8_t)v;
    return p->volume;
}

uint32_t player_position_ms(const player_t *p)
{
    if (p->sample_rate == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)p->frames_played * 1000u) / p->sample_rate);
}

uint8_t player_progress_pct(const player_t *p)
{
    if (p->track_frames == 0) {
        return 0;
    }
    return (uint8_t)(((uint64_t)p->frames_played * 100u) / p->track_frames);
}

const char *player_state_str(player_state_t s)
{
    switch (s) {
    case PLAYER_STOPPED:   return "stopped";
    case PLAYER_BUFFERING: return "buffering";
    case PLAYER_PLAYING:   return "playing";
    case PLAYER_PAUSED:    return "paused";
    case PLAYER_EOF:       return "eof";
    }
    return "?";
}
