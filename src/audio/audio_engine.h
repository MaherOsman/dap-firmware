#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

/** Initialise SDL audio subsystem and SDL_mixer.
 *  Call once after SDL_Init.  Returns 1 on success. */
int  audio_init(void);

/** Tear down SDL_mixer and audio subsystem. */
void audio_quit(void);

/** Load an audio file ready for playback.
 *  Stops and frees any previously loaded track.
 *  Returns 1 on success. */
int  audio_load(const char *path);

/** Provide the track duration (from metadata) so elapsed/progress
 *  can be computed accurately.                                      */
void audio_set_duration_sec(int sec);

/** Start or resume playback. */
void audio_play(void);

/** Pause playback, remembering the current position. */
void audio_pause(void);

/** Stop playback and reset position to zero. */
void audio_stop(void);

/** 1 while the track is actively playing (not paused, not finished). */
int  audio_is_playing(void);

/** 1 when the track has finished playing (fires once per track). */
int  audio_is_finished(void);

/** Elapsed playback time in whole seconds. */
int  audio_get_elapsed_sec(void);

/** Set playback volume 0–100. */
void audio_set_volume(int pct);

/** Begin decoding path to PCM in the background so it is ready instantly
 *  when audio_load() is called for it.  Safe to call while a track plays. */
void audio_prefetch(const char *path);

#endif /* AUDIO_ENGINE_H */
