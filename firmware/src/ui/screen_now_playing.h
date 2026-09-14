/*
 * screen_now_playing — album art front and centre, with a focus-ring
 * control row underneath.
 *
 * One encoder gives three inputs, so the controls are navigated rather than
 * pressed directly: turn moves a focus ring along the row, click activates
 * whatever is focused, long-press leaves. That is what gives volume a home
 * without spending the turn on it.
 *
 * Controls are an enum plus an enabled bitmask. PREV and NEXT are written
 * and drawn but disabled by default — turning them on is one bit each when
 * the player grows skip support, rather than a layout change later.
 *
 * Like every other screen here: draws into a gfx_t, owns no state, calls
 * nothing platform-specific. `make preview` renders it on a PC and the
 * pixels are what the panel gets.
 */
#ifndef SCREEN_NOW_PLAYING_H
#define SCREEN_NOW_PLAYING_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/gfx.h"
#include "../core/theme.h"

typedef enum {
    NP_CTL_PREV = 0,
    NP_CTL_PLAY,
    NP_CTL_NEXT,
    NP_CTL_VOL,
    NP_CTL_INFO,
    NP_CTL_COUNT
} np_ctl_t;

#define NP_BIT(c)       (1u << (c))

/* What exists today. PREV/NEXT arrive by adding their bits. */
#define NP_CTL_DEFAULT  (NP_BIT(NP_CTL_PLAY) | NP_BIT(NP_CTL_VOL) | \
                         NP_BIT(NP_CTL_INFO))
#define NP_CTL_ALL      (NP_BIT(NP_CTL_PREV) | NP_BIT(NP_CTL_PLAY) | \
                         NP_BIT(NP_CTL_NEXT) | NP_BIT(NP_CTL_VOL)  | \
                         NP_BIT(NP_CTL_INFO))

typedef struct {
    /* Track. Any of these may be NULL; the screen shows a dash. */
    const char *title;
    const char *artist;
    const char *album;
    const char *path;
    const char *format;        /* "FLAC", "MP3", "WAV" */

    uint32_t elapsed_ms;
    uint32_t duration_ms;      /* 0 = unknown, scrubber reads empty */

    uint32_t sample_rate_hz;
    uint8_t  bit_depth;
    uint8_t  channels;
    uint32_t bitrate_kbps;     /* 0 = unknown */

    bool     is_playing;       /* false = paused; changes the play glyph */

    /* Controls */
    uint32_t enabled;          /* bitmask of np_ctl_t; 0 is treated as
                                * NP_CTL_DEFAULT so a zeroed struct works */
    np_ctl_t focus;
    uint8_t  volume_pct;       /* 0-100 */
    bool     vol_active;       /* true = turn adjusts volume; draws the
                                * overlay so the mode is never invisible */
} np_state_t;

/* --- focus, as logic rather than drawing, so it can be tested --- */

bool     np_ctl_enabled(const np_state_t *s, np_ctl_t c);
/* Moves focus by `delta` over ENABLED controls only, clamping at the ends
 * (wrapping would make a 3-control row feel like a slot machine). Returns
 * the new focus. Safe when nothing is enabled. */
np_ctl_t np_focus_move(np_state_t *s, int delta);

/* mm:ss into `buf` (at least 8 bytes). Values past 99:59 clamp. */
void np_fmt_time(char *buf, uint32_t ms);

/* 0..1000 permille of the track elapsed. 0 when duration is unknown. */
uint32_t np_progress_permille(const np_state_t *s);

/* --- drawing --- */

void screen_now_playing_draw(gfx_t *g, const theme_t *t, const np_state_t *s);

/* Full-screen metadata page, reached from the INFO control. */
void screen_info_draw(gfx_t *g, const theme_t *t, const np_state_t *s);

/* Volume pill, drawn over whatever screen is beneath it. Called by
 * screen_now_playing_draw when vol_active; exposed so the library screen
 * can show it too if volume ever becomes global. */
void screen_volume_overlay_draw(gfx_t *g, const theme_t *t, int volume_pct);

#endif /* SCREEN_NOW_PLAYING_H */
