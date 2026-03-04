/*  sim_main.c — SDL2 simulator entry point
 *
 *  Navigation (click-wheel style):
 *    UP         → toggle Info Panel / back to Now Playing
 *    LEFT/RIGHT → scrub progress ±5 %
 *    SPACE      → toggle play / pause  (centre button)
 *    T          → cycle theme
 *    Q / ESC    → quit
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>

#include "src/ui/themes/theme.h"
#include "src/ui/screens/screen_now_playing.h"

#define SCREEN_W  320
#define SCREEN_H  240
#define TARGET_FPS 60

typedef enum {
    SCREEN_NOW_PLAYING,
    SCREEN_INFO_PANEL
} AppScreen;

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "DAP Simulator  |  UP=info  SPACE=play  </>=scrub  T=theme  Q=quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_RaiseWindow(win);
    SDL_StopTextInput();

    SDL_Renderer *ren = SDL_CreateRenderer(
        win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }

    screen_now_playing_init();

    NowPlayingState state = {
        .track_title    = "Clair de Lune",
        .artist         = "Claude Debussy",
        .album          = "Suite Bergamasque",
        .year           = "1905",
        .file_format    = "FLAC",
        .bit_depth      = 24,
        .sample_rate_hz = 96000,
        .bitrate_kbps   = 2304,

        .progress       = 0.35f,
        .elapsed_sec    = 83,
        .duration_sec   = 300,
        .is_playing     = 1,
        .art_texture    = NULL,
    };

    int        theme_idx  = 0;
    AppScreen  cur_screen = SCREEN_NOW_PLAYING;
    int        running    = 1;
    Uint32     frame_ms   = 1000 / TARGET_FPS;

    while (running) {
        Uint32 frame_start = SDL_GetTicks();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {

                    case SDLK_q:
                    case SDLK_ESCAPE:
                        running = 0;
                        break;

                    /* UP: toggle info panel */
                    case SDLK_UP:
                        cur_screen = (cur_screen == SCREEN_NOW_PLAYING)
                                     ? SCREEN_INFO_PANEL
                                     : SCREEN_NOW_PLAYING;
                        break;

                    /* SPACE: play / pause (centre button) */
                    case SDLK_SPACE:
                        state.is_playing = !state.is_playing;
                        break;

                    /* LEFT / RIGHT: scrub ±5 % */
                    case SDLK_RIGHT:
                        state.progress += 0.05f;
                        if (state.progress > 1.f) state.progress = 1.f;
                        state.elapsed_sec = (int)(state.progress * state.duration_sec);
                        break;

                    case SDLK_LEFT:
                        state.progress -= 0.05f;
                        if (state.progress < 0.f) state.progress = 0.f;
                        state.elapsed_sec = (int)(state.progress * state.duration_sec);
                        break;

                    /* T: cycle theme */
                    case SDLK_t:
                        theme_idx = (theme_idx + 1) % THEME_COUNT;
                        printf("Theme → %s\n", ALL_THEMES[theme_idx]->name);
                        {
                            char title[128];
                            snprintf(title, sizeof(title),
                                "DAP Simulator  |  Theme: %s  |  UP=info  SPACE=play",
                                ALL_THEMES[theme_idx]->name);
                            SDL_SetWindowTitle(win, title);
                        }
                        break;
                }
            }
        }

        /* Simulate playback tick (1 sec every ~60 frames) */
        static int frame_count = 0;
        if (state.is_playing) {
            frame_count++;
            if (frame_count >= TARGET_FPS) {
                frame_count = 0;
                if (state.elapsed_sec < state.duration_sec) {
                    state.elapsed_sec++;
                    state.progress = (float)state.elapsed_sec / (float)state.duration_sec;
                }
            }
        }

        /* Draw active screen */
        if (cur_screen == SCREEN_INFO_PANEL) {
            screen_info_panel_draw(ren, ALL_THEMES[theme_idx], &state,
                                   SCREEN_W, SCREEN_H);
        } else {
            screen_now_playing_draw(ren, ALL_THEMES[theme_idx], &state,
                                    SCREEN_W, SCREEN_H);
        }
        SDL_RenderPresent(ren);

        Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    screen_now_playing_destroy();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
