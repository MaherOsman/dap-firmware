#include "audio_engine.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────
   Architecture note
   ─────────────────────────────────────────────
   Mix_LoadWAV pre-decodes the entire file to PCM before playback.
   The audio callback then just copies memory — no real-time decode
   pressure, no underrun risk from decode latency.

   audio_prefetch() decodes the NEXT track on a background thread
   while the current track plays, so audio_load() finds the PCM
   already in memory and returns instantly.
─────────────────────────────────────────────── */

#define MUSIC_CHANNEL  0

static Mix_Chunk *g_chunk         = NULL;
static int        g_duration_sec  = 0;
static int        g_started       = 0;
static int        g_paused        = 0;

static Uint32 g_play_start_ticks = 0;
static int    g_paused_at_ms     = 0;

/* ─────────────────────────────────────────────
   Background prefetch
───────────────────────────────────────────── */
static Mix_Chunk      *g_pre_chunk = NULL;
static char            g_pre_path[512];
static pthread_mutex_t g_pre_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *prefetch_fn(void *arg)
{
    char *path = (char *)arg;   /* heap-allocated by audio_prefetch */
    Mix_Chunk *chunk = Mix_LoadWAV(path);

    pthread_mutex_lock(&g_pre_mutex);
    if (g_pre_chunk) { Mix_FreeChunk(g_pre_chunk); g_pre_chunk = NULL; }
    g_pre_chunk = chunk;
    snprintf(g_pre_path, sizeof(g_pre_path), "%s", path);
    pthread_mutex_unlock(&g_pre_mutex);

    free(path);
    return NULL;
}

void audio_prefetch(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return;
    pthread_t t;
    if (pthread_create(&t, NULL, prefetch_fn, copy) != 0) { free(copy); return; }
    pthread_detach(t);
}

/* ─────────────────────────────────────────────
   Lifecycle
───────────────────────────────────────────── */
int audio_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL audio init: %s\n", SDL_GetError());
        return 0;
    }

    /* Init decoders before opening device so Mix_LoadWAV handles FLAC */
    Mix_Init(MIX_INIT_FLAC | MIX_INIT_MP3 | MIX_INIT_OGG);

    /* 44100 Hz = source sample rate, zero resampling in SDL_mixer. */
    static const struct { int freq; Uint16 fmt; } configs[] = {
        { 44100, AUDIO_S32SYS },
        { 48000, AUDIO_S32SYS },
        { 44100, AUDIO_S16SYS },
        {     0, 0            }
    };

    /* On Linux, a smaller buffer (1024-4096) is usually better.
       WSL2 may still need a larger one, but 32768 is excessive. */
    int buffer_size = 4096;
    const char *env_wsl = getenv("WSL_DISTRO_NAME");
    if (env_wsl) buffer_size = 32768;

    int opened = 0;
    for (int i = 0; configs[i].freq; i++) {
        if (Mix_OpenAudio(configs[i].freq, configs[i].fmt, 2, buffer_size) == 0) {
            int freq; Uint16 fmt; int chans;
            Mix_QuerySpec(&freq, &fmt, &chans);
            fprintf(stderr, "Audio opened: %d Hz / %d-bit / %dch / %d-sample buffer\n",
                    freq, SDL_AUDIO_BITSIZE(fmt), chans, buffer_size);
            opened = 1;
            break;
        }
    }
    if (!opened) {
        fprintf(stderr, "Mix_OpenAudio: %s\n", Mix_GetError());
        return 0;
    }

    Mix_AllocateChannels(1);   /* channel 0 = music */
    return 1;
}

void audio_quit(void)
{
    audio_stop();

    pthread_mutex_lock(&g_pre_mutex);
    if (g_pre_chunk) { Mix_FreeChunk(g_pre_chunk); g_pre_chunk = NULL; }
    pthread_mutex_unlock(&g_pre_mutex);

    if (g_chunk) { Mix_FreeChunk(g_chunk); g_chunk = NULL; }
    Mix_CloseAudio();
    Mix_Quit();
}

/* ─────────────────────────────────────────────
   Track loading
───────────────────────────────────────────── */
int audio_load(const char *path)
{
    audio_stop();
    if (g_chunk) { Mix_FreeChunk(g_chunk); g_chunk = NULL; }

    /* Use prefetched PCM if available, otherwise decode now */
    pthread_mutex_lock(&g_pre_mutex);
    if (g_pre_chunk && strcmp(g_pre_path, path) == 0) {
        g_chunk      = g_pre_chunk;
        g_pre_chunk  = NULL;
        g_pre_path[0] = '\0';
        pthread_mutex_unlock(&g_pre_mutex);
        fprintf(stderr, "Loaded (prefetched): %s\n", path);
    } else {
        pthread_mutex_unlock(&g_pre_mutex);
        fprintf(stderr, "Decoding: %s\n", path);
        g_chunk = Mix_LoadWAV(path);
    }

    g_duration_sec = 0;
    g_paused_at_ms = 0;
    g_started      = 0;
    g_paused       = 0;

    if (!g_chunk) {
        fprintf(stderr, "Mix_LoadWAV(%s): %s\n", path, Mix_GetError());
        return 0;
    }
    return 1;
}

void audio_set_duration_sec(int sec)
{
    g_duration_sec = sec;
}

/* ─────────────────────────────────────────────
   Playback control
───────────────────────────────────────────── */
void audio_play(void)
{
    if (!g_chunk) return;

    if (g_paused) {
        Mix_Resume(MUSIC_CHANNEL);
        g_play_start_ticks = SDL_GetTicks() - (Uint32)g_paused_at_ms;
        g_paused = 0;
    } else if (!g_started) {
        if (Mix_PlayChannel(MUSIC_CHANNEL, g_chunk, 0) == -1) {
            fprintf(stderr, "Mix_PlayChannel: %s\n", Mix_GetError());
            return;
        }
        g_play_start_ticks = SDL_GetTicks();
        g_paused_at_ms     = 0;
        g_started          = 1;
    }
}

void audio_pause(void)
{
    if (!g_chunk || !g_started || g_paused) return;
    g_paused_at_ms = (int)(SDL_GetTicks() - g_play_start_ticks);
    Mix_Pause(MUSIC_CHANNEL);
    g_paused = 1;
}

void audio_stop(void)
{
    Mix_HaltChannel(MUSIC_CHANNEL);
    g_paused_at_ms = 0;
    g_started      = 0;
    g_paused       = 0;
}

void audio_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    Mix_Volume(MUSIC_CHANNEL, pct * MIX_MAX_VOLUME / 100);
}

/* ─────────────────────────────────────────────
   State queries
───────────────────────────────────────────── */
int audio_is_playing(void)
{
    return g_started && !g_paused && Mix_Playing(MUSIC_CHANNEL);
}

int audio_is_finished(void)
{
    return g_started && !g_paused && !Mix_Playing(MUSIC_CHANNEL);
}

int audio_get_elapsed_sec(void)
{
    if (!g_started) return 0;
    if (g_paused)   return g_paused_at_ms / 1000;
    int ms = (int)(SDL_GetTicks() - g_play_start_ticks);
    if (g_duration_sec > 0 && ms / 1000 > g_duration_sec)
        return g_duration_sec;
    return ms / 1000;
}
