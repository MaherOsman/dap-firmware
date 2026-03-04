#ifndef SCREEN_NOW_PLAYING_H
#define SCREEN_NOW_PLAYING_H

#include "../themes/theme.h"

/* ─────────────────────────────────────────────
   Playback state passed in from the audio layer
   (or test stubs in the simulator).
───────────────────────────────────────────── */
typedef struct {
    const char *track_title;    /* "Clair de Lune"            */
    const char *artist;         /* "Claude Debussy"           */
    const char *album;          /* "Suite Bergamasque"        */
    const char *year;           /* "1905"  (up to 16 chars)   */
    const char *file_format;    /* "FLAC", "WAV", "MP3"       */
    int         bit_depth;      /* 16, 24, 32                 */
    int         sample_rate_hz; /* 44100, 48000, 96000        */
    int         bitrate_kbps;   /* 320, 1411, 2304            */

    float       progress;       /* 0.0 – 1.0                  */
    int         elapsed_sec;    /* seconds played             */
    int         duration_sec;   /* total track length         */

    int         is_playing;     /* 1 = playing, 0 = paused    */

    /* Album art – NULL = show placeholder */
    void       *art_texture;    /* SDL_Texture* cast to void* */
    int         art_w;
    int         art_h;
} NowPlayingState;

/* ─────────────────────────────────────────────
   Lifecycle
───────────────────────────────────────────── */

/** Call once after SDL init to allocate any cached resources. */
void screen_now_playing_init(void);

/** Call every frame. renderer and theme are passed in so this
 *  file has zero global state and is fully testable.          */
void screen_now_playing_draw(void        *renderer,   /* SDL_Renderer* */
                             const Theme *theme,
                             const NowPlayingState *state,
                             int          screen_w,
                             int          screen_h);

/** Full-screen info overlay showing track metadata.
 *  Activated by pressing UP on the now-playing screen.        */
void screen_info_panel_draw(void        *renderer,
                            const Theme *theme,
                            const NowPlayingState *state,
                            int          screen_w,
                            int          screen_h);

/** Release any resources allocated in _init. */
void screen_now_playing_destroy(void);

/** Draw a volume feedback overlay on top of the current screen.
 *  volume_pct: 0–100. Call after the active screen draw,
 *  before SDL_RenderPresent.                                     */
void draw_volume_overlay(void *renderer, const Theme *theme,
                         int volume_pct, int screen_w);

#endif /* SCREEN_NOW_PLAYING_H */
