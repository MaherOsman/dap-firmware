/*
 * player — the playback state machine and buffer-health policy.
 *
 * This is the part of a DAP that actually decides whether it sounds good.
 * The rules encoded here:
 *
 *   1. Never start the I2S DMA until the ring buffer is PREROLL_PCT full.
 *     Starting early is the #1 cause of a click at the top of every track.
 *   2. If the buffer falls below REFILL_PCT, the main loop must read from
 *      the SD card *now* — that is the whole job of player_needs_refill().
 *   3. If it hits zero while playing, that is an underrun: output silence
 *      (never stale samples) and count it. A rising underrun count is the
 *      signal that your SD read block size or SPI clock is wrong.
 *
 * No hardware calls in here at all — it takes buffer levels and events in,
 * and returns decisions out.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_BUFFERING,   /* prerolling before first sample goes out */
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_EOF          /* file drained, waiting for the next track */
} player_state_t;

#define PLAYER_PREROLL_PCT 75u
#define PLAYER_REFILL_PCT  50u

typedef struct {
    player_state_t state;
    uint32_t track_frames;   /* total frames in the current file */
    uint32_t frames_played;
    uint32_t sample_rate;
    uint8_t  volume;         /* 0..100 */
    uint32_t underruns;
    bool     file_exhausted; /* reader hit end of the data chunk */
} player_t;

void player_init(player_t *p);

/* Load a new track. Moves to BUFFERING. */
void player_open(player_t *p, uint32_t total_frames, uint32_t sample_rate);

/* Feed the current buffer fill (0..100%). Advances BUFFERING -> PLAYING. */
void player_tick(player_t *p, uint8_t buffer_pct);

/* True when the main loop should go read more bytes off the SD card. */
bool player_needs_refill(const player_t *p, uint8_t buffer_pct);

/* True when the I2S DMA should be pulling samples right now. */
bool player_output_enabled(const player_t *p);

void player_play_pause(player_t *p);
void player_stop(player_t *p);

/* Call from the audio side after handing `frames` to the DAC. */
void player_frames_consumed(player_t *p, uint32_t frames);
/* Call when the DMA needed samples and the buffer was empty. */
void player_underrun(player_t *p);

/* Reader tells the player the data chunk is finished. */
void player_set_exhausted(player_t *p, bool exhausted);

uint8_t  player_set_volume(player_t *p, int delta); /* clamped 0..100 */
uint32_t player_position_ms(const player_t *p);
uint8_t  player_progress_pct(const player_t *p);

const char *player_state_str(player_state_t s);

#endif /* PLAYER_H */
