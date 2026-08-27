/*  sim_main.c — SDL2 simulator with real audio playback
 *
 *  Usage: ./dap_sim [music_dir]
 *  Default: /mnt/c/Users/maher/Downloads/testmusic
 *
 *  Navigation (click-wheel style):
 *    UP         → Info Panel  (from Now Playing)
 *                 Back        (from Info Panel)
 *    DOWN       → Library     (from Now Playing)
 *    LEFT       → Previous track (Now Playing) / Back / Up level (Library)
 *    RIGHT      → Next track  (Now Playing) / Descend / Play (Library)
 *    SPACE      → Play / Pause  (centre button)
 *    [ / ]      → Volume down / up
 *    T          → Cycle theme
 *    Q / ESC    → Quit
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <string.h>

#include "src/ui/themes/theme.h"
#include "src/ui/screens/screen_now_playing.h"
#include "src/ui/screens/screen_library.h"
#include "src/audio/metadata.h"
#include "src/audio/audio_engine.h"
#include "src/audio/file_scanner.h"

#define SCREEN_W   320
#define SCREEN_H   240
#define TARGET_FPS  60

#define DEFAULT_MUSIC_DIR "/mnt/c/Users/maher/Downloads/testmusic"

/* Volume overlay shows for this many frames after a change */
#define VOL_DISPLAY_FRAMES  90

/* How many library rows fit on screen */
#define LIB_HEADER_H  24
#define LIB_HINT_H    15
#define LIB_VISIBLE   ((SCREEN_H - LIB_HEADER_H - LIB_HINT_H) / LIBRARY_ROW_H)

/* ─────────────────────────────────────────────
   App screen
───────────────────────────────────────────── */
typedef enum {
    SCREEN_NOW_PLAYING,
    SCREEN_INFO_PANEL,
    SCREEN_LIBRARY
} AppScreen;

/* ─────────────────────────────────────────────
   Persistent string storage for NowPlayingState
───────────────────────────────────────────── */
static char g_title[256];
static char g_artist[256];
static char g_album[256];
static char g_year[16];
static char g_format[16];

/* ─────────────────────────────────────────────
   Full metadata cache (loaded at startup)
───────────────────────────────────────────── */
static TrackMetadata g_meta[MAX_TRACKS];

/* ─────────────────────────────────────────────
   Artist → Album → Track hierarchy
───────────────────────────────────────────── */
#define MAX_ARTISTS         64
#define MAX_ALBUMS          32
#define MAX_TRACKS_PER_ALB  128

typedef struct {
    char name[256];
    int  track_indices[MAX_TRACKS_PER_ALB];
    int  count;
} AlbumNode;

typedef struct {
    char      name[256];
    AlbumNode albums[MAX_ALBUMS];
    int       album_count;
} ArtistNode;

static ArtistNode g_artists[MAX_ARTISTS];
static int        g_artist_count = 0;

/* ─────────────────────────────────────────────
   Cover art: search alongside the track file
───────────────────────────────────────────── */
static SDL_Texture *load_cover_art(SDL_Renderer *ren, const char *track_path)
{
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", track_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else       snprintf(dir, sizeof(dir), ".");

    const char *candidates[] = {
        "folder.jpg", "Folder.jpg",
        "cover.jpg",  "Cover.jpg",
        "AlbumArtSmall.jpg",
        "front.jpg",  "Front.jpg",
        NULL
    };

    char img_path[MAX_PATH_LEN * 2];
    for (int i = 0; candidates[i]; i++) {
        snprintf(img_path, sizeof(img_path), "%s/%s", dir, candidates[i]);
        SDL_Surface *surf = IMG_Load(img_path);
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
            SDL_FreeSurface(surf);
            if (tex) return tex;
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────
   Load a track: audio + art + update UI state
───────────────────────────────────────────── */
static void load_track(int idx,
                        const TrackList *tl,
                        SDL_Renderer    *ren,
                        NowPlayingState *state,
                        SDL_Texture    **art_tex)
{
    if (idx < 0 || idx >= tl->count) return;

    audio_stop();
    if (*art_tex) { SDL_DestroyTexture(*art_tex); *art_tex = NULL; }

    /* Use cached metadata */
    const TrackMetadata *m = &g_meta[idx];

    snprintf(g_title,  sizeof(g_title),  "%s", m->title[0]        ? m->title        : "(Unknown)");
    snprintf(g_artist, sizeof(g_artist), "%s", m->artist[0]       ? m->artist       : "(Unknown)");
    snprintf(g_album,  sizeof(g_album),  "%s", m->album[0]        ? m->album        : "(Unknown)");
    snprintf(g_year,   sizeof(g_year),   "%s", m->year[0]         ? m->year         : "");
    snprintf(g_format, sizeof(g_format), "%s", m->file_format[0]  ? m->file_format  : "");

    state->track_title    = g_title;
    state->artist         = g_artist;
    state->album          = g_album;
    state->year           = g_year;
    state->file_format    = g_format;
    state->bit_depth      = m->bit_depth;
    state->sample_rate_hz = m->sample_rate_hz;
    state->bitrate_kbps   = m->bitrate_kbps;
    state->duration_sec   = m->duration_sec;
    state->elapsed_sec    = 0;
    state->progress       = 0.f;

    if (audio_load(tl->paths[idx])) {
        audio_set_duration_sec(m->duration_sec);
        audio_play();
        state->is_playing = 1;
    } else {
        state->is_playing = 0;
    }

    *art_tex = load_cover_art(ren, tl->paths[idx]);
    state->art_texture = *art_tex;

    printf("[%d/%d] %s \xe2\x80\x93 %s\n", idx + 1, tl->count, g_artist, g_title);

    /* Start decoding the next track in the background while this one plays */
    int next = (idx + 1) % tl->count;
    audio_prefetch(tl->paths[next]);
}

/* ─────────────────────────────────────────────
   Clamp library scroll so `selected` is visible
───────────────────────────────────────────── */
static void lib_clamp_scroll(int selected, int *scroll_top)
{
    if (selected < *scroll_top)
        *scroll_top = selected;
    if (selected >= *scroll_top + LIB_VISIBLE)
        *scroll_top = selected - LIB_VISIBLE + 1;
    if (*scroll_top < 0) *scroll_top = 0;
}

/* ─────────────────────────────────────────────
   Build Artist→Album→Track hierarchy from g_meta
───────────────────────────────────────────── */
static void build_library(int track_count)
{
    g_artist_count = 0;
    memset(g_artists, 0, sizeof(g_artists));

    for (int i = 0; i < track_count; i++) {
        const char *artist = g_meta[i].artist[0] ? g_meta[i].artist : "(Unknown)";
        const char *album  = g_meta[i].album[0]  ? g_meta[i].album  : "(Unknown)";

        /* Find or create artist */
        int ai = -1;
        for (int a = 0; a < g_artist_count; a++) {
            if (strcmp(g_artists[a].name, artist) == 0) { ai = a; break; }
        }
        if (ai < 0) {
            if (g_artist_count >= MAX_ARTISTS) continue;
            ai = g_artist_count++;
            snprintf(g_artists[ai].name, sizeof(g_artists[ai].name), "%s", artist);
        }

        /* Find or create album under this artist */
        ArtistNode *ar = &g_artists[ai];
        int bi = -1;
        for (int b = 0; b < ar->album_count; b++) {
            if (strcmp(ar->albums[b].name, album) == 0) { bi = b; break; }
        }
        if (bi < 0) {
            if (ar->album_count >= MAX_ALBUMS) continue;
            bi = ar->album_count++;
            snprintf(ar->albums[bi].name, sizeof(ar->albums[bi].name), "%s", album);
        }

        /* Add track index */
        AlbumNode *ab = &ar->albums[bi];
        if (ab->count < MAX_TRACKS_PER_ALB) {
            ab->track_indices[ab->count++] = i;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   main
═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    const char *music_dir = (argc > 1) ? argv[1] : DEFAULT_MUSIC_DIR;

    /* ── SDL init ── */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 1;
    }
    if (IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG) == 0) {
        fprintf(stderr, "IMG_Init: %s\n", IMG_GetError()); return 1;
    }
    if (!audio_init()) return 1;

    SDL_Window *win = SDL_CreateWindow(
        "DAP Sim  |  UP=info  DOWN=library  SPACE=play  [/]=vol  T=theme  Q=quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_RaiseWindow(win);
    SDL_StopTextInput();

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }

    screen_now_playing_init();
    screen_library_init();

    /* ── Scan and cache all track metadata ── */
    TrackList tl;
    int n = file_scanner_scan(music_dir, &tl);
    if (n == 0) {
        fprintf(stderr, "No audio files found in: %s\n", music_dir);
        return 1;
    }
    printf("Found %d tracks — loading metadata...\n", n);
    for (int i = 0; i < tl.count; i++) {
        if (!metadata_read(tl.paths[i], &g_meta[i]))
            memset(&g_meta[i], 0, sizeof(g_meta[i]));
    }
    printf("Building library hierarchy...\n");
    build_library(tl.count);
    printf("Ready. %d artists.\n", g_artist_count);

    /* ── Playback state ── */
    NowPlayingState state;
    memset(&state, 0, sizeof(state));
    SDL_Texture *art_tex   = NULL;
    int          cur_track = 0;
    load_track(cur_track, &tl, ren, &state, &art_tex);

    /* ── Navigation state ── */
    AppScreen cur_screen   = SCREEN_NOW_PLAYING;

    /* Library drill-down state */
    LibLevel lib_level      = LIB_LEVEL_ARTIST;
    int      lib_artist_sel = 0;
    int      lib_album_sel  = 0;
    int      lib_track_sel  = 0;
    /* Separate scroll per level so position is remembered when going back */
    int      lib_artist_scroll = 0;
    int      lib_album_scroll  = 0;
    int      lib_track_scroll  = 0;

    /* ── Volume state ── */
    int g_volume    = 100;   /* 0–100 % */
    int g_vol_timer = 0;     /* frames remaining to show overlay */
    audio_set_volume(g_volume);

    int    theme_idx = 0;
    int    running   = 1;
    Uint32 frame_ms  = 1000 / TARGET_FPS;

    /* LibRow scratch buffer for the current library level */
    static LibRow lib_rows[MAX_TRACKS];

    while (running) {
        Uint32 frame_start = SDL_GetTicks();

        /* ── Events ── */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type != SDL_KEYDOWN) continue;

            SDL_Keycode key = ev.key.keysym.sym;

            /* ── Global keys (any screen) ── */
            if (key == SDLK_q || key == SDLK_ESCAPE) { running = 0; break; }

            if (key == SDLK_t) {
                theme_idx = (theme_idx + 1) % THEME_COUNT;
                printf("Theme \xe2\x86\x92 %s\n", ALL_THEMES[theme_idx]->name);
                char title[128];
                snprintf(title, sizeof(title),
                    "DAP Sim  |  Theme: %s  |  [/]=vol  Q=quit",
                    ALL_THEMES[theme_idx]->name);
                SDL_SetWindowTitle(win, title);
            }

            /* Volume: [ = down, ] = up */
            if (key == SDLK_LEFTBRACKET) {
                g_volume -= 5;
                if (g_volume < 0) g_volume = 0;
                audio_set_volume(g_volume);
                g_vol_timer = VOL_DISPLAY_FRAMES;
            }
            if (key == SDLK_RIGHTBRACKET) {
                g_volume += 5;
                if (g_volume > 100) g_volume = 100;
                audio_set_volume(g_volume);
                g_vol_timer = VOL_DISPLAY_FRAMES;
            }

            /* ── Screen-specific keys ── */
            if (cur_screen == SCREEN_NOW_PLAYING) {
                if (key == SDLK_UP) {
                    cur_screen = SCREEN_INFO_PANEL;
                }
                else if (key == SDLK_DOWN) {
                    /* Open library at ARTIST level; pre-select the playing artist */
                    lib_level = LIB_LEVEL_ARTIST;
                    /* Find which artist owns cur_track */
                    for (int a = 0; a < g_artist_count; a++) {
                        for (int b = 0; b < g_artists[a].album_count; b++) {
                            for (int t = 0; t < g_artists[a].albums[b].count; t++) {
                                if (g_artists[a].albums[b].track_indices[t] == cur_track) {
                                    lib_artist_sel = a;
                                    goto found_artist;
                                }
                            }
                        }
                    }
                    found_artist:
                    lib_clamp_scroll(lib_artist_sel, &lib_artist_scroll);
                    cur_screen = SCREEN_LIBRARY;
                }
                else if (key == SDLK_SPACE) {
                    if (state.is_playing) { audio_pause(); state.is_playing = 0; }
                    else                  { audio_play();  state.is_playing = 1; }
                }
                else if (key == SDLK_RIGHT) {
                    cur_track = (cur_track + 1) % tl.count;
                    load_track(cur_track, &tl, ren, &state, &art_tex);
                }
                else if (key == SDLK_LEFT) {
                    cur_track = (cur_track - 1 + tl.count) % tl.count;
                    load_track(cur_track, &tl, ren, &state, &art_tex);
                }
            }
            else if (cur_screen == SCREEN_INFO_PANEL) {
                if (key == SDLK_UP || key == SDLK_SPACE) {
                    cur_screen = SCREEN_NOW_PLAYING;
                }
            }
            else if (cur_screen == SCREEN_LIBRARY) {
                if (lib_level == LIB_LEVEL_ARTIST) {
                    if (key == SDLK_UP) {
                        if (lib_artist_sel > 0) {
                            lib_artist_sel--;
                            lib_clamp_scroll(lib_artist_sel, &lib_artist_scroll);
                        }
                    }
                    else if (key == SDLK_DOWN) {
                        if (lib_artist_sel < g_artist_count - 1) {
                            lib_artist_sel++;
                            lib_clamp_scroll(lib_artist_sel, &lib_artist_scroll);
                        }
                    }
                    else if (key == SDLK_RIGHT || key == SDLK_SPACE) {
                        /* Descend into albums */
                        lib_level      = LIB_LEVEL_ALBUM;
                        lib_album_sel  = 0;
                        lib_album_scroll = 0;
                    }
                    else if (key == SDLK_LEFT) {
                        cur_screen = SCREEN_NOW_PLAYING;
                    }
                }
                else if (lib_level == LIB_LEVEL_ALBUM) {
                    int album_count = g_artists[lib_artist_sel].album_count;
                    if (key == SDLK_UP) {
                        if (lib_album_sel > 0) {
                            lib_album_sel--;
                            lib_clamp_scroll(lib_album_sel, &lib_album_scroll);
                        }
                    }
                    else if (key == SDLK_DOWN) {
                        if (lib_album_sel < album_count - 1) {
                            lib_album_sel++;
                            lib_clamp_scroll(lib_album_sel, &lib_album_scroll);
                        }
                    }
                    else if (key == SDLK_RIGHT || key == SDLK_SPACE) {
                        /* Descend into tracks */
                        lib_level      = LIB_LEVEL_TRACK;
                        lib_track_sel  = 0;
                        lib_track_scroll = 0;
                    }
                    else if (key == SDLK_LEFT) {
                        lib_level = LIB_LEVEL_ARTIST;
                        lib_clamp_scroll(lib_artist_sel, &lib_artist_scroll);
                    }
                }
                else { /* LIB_LEVEL_TRACK */
                    AlbumNode *ab = &g_artists[lib_artist_sel].albums[lib_album_sel];
                    if (key == SDLK_UP) {
                        if (lib_track_sel > 0) {
                            lib_track_sel--;
                            lib_clamp_scroll(lib_track_sel, &lib_track_scroll);
                        }
                    }
                    else if (key == SDLK_DOWN) {
                        if (lib_track_sel < ab->count - 1) {
                            lib_track_sel++;
                            lib_clamp_scroll(lib_track_sel, &lib_track_scroll);
                        }
                    }
                    else if (key == SDLK_SPACE || key == SDLK_RIGHT) {
                        /* Play selected track and return to Now Playing */
                        if (lib_track_sel < ab->count) {
                            cur_track = ab->track_indices[lib_track_sel];
                            load_track(cur_track, &tl, ren, &state, &art_tex);
                            cur_screen = SCREEN_NOW_PLAYING;
                        }
                    }
                    else if (key == SDLK_LEFT) {
                        lib_level = LIB_LEVEL_ALBUM;
                        lib_clamp_scroll(lib_album_sel, &lib_album_scroll);
                    }
                }
            }
        }

        /* ── Auto-advance on track end ── */
        if (audio_is_finished()) {
            cur_track = (cur_track + 1) % tl.count;
            load_track(cur_track, &tl, ren, &state, &art_tex);
        }

        /* ── Sync playback position ── */
        state.elapsed_sec = audio_get_elapsed_sec();
        if (state.duration_sec > 0)
            state.progress = (float)state.elapsed_sec / (float)state.duration_sec;
        state.is_playing = audio_is_playing();

        /* ── Build library rows for current level ── */
        int         lib_row_count = 0;
        int         lib_scroll    = 0;
        int         lib_sel       = 0;
        const char *lib_header    = "Library";

        if (cur_screen == SCREEN_LIBRARY) {
            if (lib_level == LIB_LEVEL_ARTIST) {
                lib_header    = "Library";
                lib_sel       = lib_artist_sel;
                lib_scroll    = lib_artist_scroll;
                lib_row_count = g_artist_count;
                for (int a = 0; a < g_artist_count; a++) {
                    lib_rows[a].text    = g_artists[a].name;
                    lib_rows[a].has_sub = 1;
                    int is_cur = 0;
                    for (int b = 0; b < g_artists[a].album_count && !is_cur; b++)
                        for (int t = 0; t < g_artists[a].albums[b].count && !is_cur; t++)
                            if (g_artists[a].albums[b].track_indices[t] == cur_track)
                                is_cur = 1;
                    lib_rows[a].is_current = is_cur;
                }
            }
            else if (lib_level == LIB_LEVEL_ALBUM) {
                ArtistNode *ar = &g_artists[lib_artist_sel];
                lib_header     = ar->name;
                lib_sel        = lib_album_sel;
                lib_scroll     = lib_album_scroll;
                lib_row_count  = ar->album_count;
                for (int b = 0; b < ar->album_count; b++) {
                    lib_rows[b].text    = ar->albums[b].name;
                    lib_rows[b].has_sub = 1;
                    int is_cur = 0;
                    for (int t = 0; t < ar->albums[b].count && !is_cur; t++)
                        if (ar->albums[b].track_indices[t] == cur_track)
                            is_cur = 1;
                    lib_rows[b].is_current = is_cur;
                }
            }
            else { /* LIB_LEVEL_TRACK */
                ArtistNode *ar = &g_artists[lib_artist_sel];
                AlbumNode  *ab = &ar->albums[lib_album_sel];
                lib_header     = ab->name;
                lib_sel        = lib_track_sel;
                lib_scroll     = lib_track_scroll;
                lib_row_count  = ab->count;
                for (int t = 0; t < ab->count; t++) {
                    int idx = ab->track_indices[t];
                    lib_rows[t].text       = g_meta[idx].title[0]
                                             ? g_meta[idx].title : "(Unknown)";
                    lib_rows[t].has_sub    = 0;
                    lib_rows[t].is_current = (idx == cur_track);
                }
            }
        }

        /* ── Draw active screen ── */
        switch (cur_screen) {
            case SCREEN_INFO_PANEL:
                screen_info_panel_draw(ren, ALL_THEMES[theme_idx], &state,
                                       SCREEN_W, SCREEN_H);
                break;
            case SCREEN_LIBRARY:
                screen_library_draw(ren, ALL_THEMES[theme_idx],
                                    lib_rows, lib_row_count,
                                    lib_sel, lib_scroll,
                                    lib_header, lib_level,
                                    SCREEN_W, SCREEN_H);
                break;
            default:
                screen_now_playing_draw(ren, ALL_THEMES[theme_idx], &state,
                                        SCREEN_W, SCREEN_H);
                break;
        }

        /* ── Volume overlay (drawn on top of any screen) ── */
        if (g_vol_timer > 0) {
            draw_volume_overlay(ren, ALL_THEMES[theme_idx], g_volume, SCREEN_W);
            g_vol_timer--;
        }

        SDL_RenderPresent(ren);

        Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    /* ── Cleanup ── */
    if (art_tex) SDL_DestroyTexture(art_tex);
    screen_now_playing_destroy();
    screen_library_destroy();
    audio_quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
