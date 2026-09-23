/* platform_ui.c — screen state, input dispatch, auto-advance, panel push. */

#include "platform_ui.h"

#include <stdio.h>
#include <string.h>

#include "main.h"

#include "browse.h"
#include "playqueue.h"
#include "theme.h"
#include "screen_library.h"
#include "screen_now_playing.h"
#include "platform_audio.h"
#include "platform_libio.h"
#include "platform_library.h"
#include "platform_art.h"
#include "config.h"
#include "screen_settings.h"

/* Repaint cadence while a track plays, so the scrubber and elapsed time
 * move without the encoder being touched. 500 ms is twice the resolution
 * the seconds display needs, which is enough to never look stuck. */
#define TICK_REDRAW_MS  500u

/* Art-only layout: how long the scrubber and controls stay up after the
 * last touch of the encoder. */
#define NP_OVERLAY_MS   3000u

/* Rows per SPI burst when pushing the framebuffer.
 *
 * The display and the SD card share SPI1, so every pixel pushed is time the
 * card is not feeding the DAC. A whole 320x240 frame is 150 KB and blocks
 * the main loop for roughly 40 ms — well past the ~23 ms the audio ring can
 * survive without a refill, which showed up as dozens of underruns per track
 * and audible static. Pushing in bands and servicing audio between them
 * keeps the longest blocking window to a few milliseconds.
 *
 * 16 rows = 7.5 KB ≈ 4 ms at 15 MHz. Smaller bands mean more window-set
 * overhead; larger ones creep back toward the budget. */
#define PANEL_BAND_ROWS 16

static gfx_t     *g_fb;
static st7789_t  *g_tft;
static libidx_t  *g_idx;

static browse_t    g_browse;
static playqueue_t g_pq;
static ui_screen_t g_screen = UI_LIBRARY;
static np_state_t  g_np;

static lib_row_t   g_rows[BROWSE_MAX_ROWS];
static dap_config_t g_cfg;
static settings_t   g_settings;
static bool      g_dirty = true;
static uint32_t  g_last_paint;
static uint32_t  g_overlay_at;   /* when the art-only controls last woke */

/* Strings for the now-playing screen. np_state_t holds pointers, and the
 * track record they would point into lives in the index page cache — which
 * is overwritten by the next browse scroll. So they are copied here. */
static char g_title[LIB_TITLE_MAX + 1];
static char g_artist[64];
static char g_album[64];
static char g_path[LIB_PATH_MAX + 1];

/* Live rather than constant: changing the theme in Settings repaints every
 * screen immediately, which is what makes choosing one feel like choosing
 * rather than guessing. */
static const theme_t *g_theme = &THEME_DARK;

static void apply_theme(uint8_t idx)
{
    if (THEME_COUNT > 0 && idx < (uint8_t)THEME_COUNT) {
        g_theme = ALL_THEMES[idx];
    }
}

/* Settings are written the moment they change — there is no save button, so
 * there is no unsaved state to lose if the battery dies. A failed write is
 * worth saying out loud: silently not saving is the failure people notice
 * three boots later. */
static void save_config(void)
{
    if (!config_save(&g_cfg, plat_libio_shared(), CFG_PATH)) {
        printf("config: save FAILED (%s)\r\n",
               plat_libio_result_name(plat_libio_last_result()));
    }
}

/* ------------------------------------------------------------------ */

static void copy_to(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (src != NULL) {
        while (src[i] != '\0' && i + 1u < cap) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

/* Caches the strings for whatever the queue says is current. */
static void capture_track(uint32_t track)
{
    lib_track_t t;
    uint32_t album;

    g_title[0] = g_artist[0] = g_album[0] = g_path[0] = '\0';
    if (g_idx == NULL || track == PQ_NO_TRACK) return;

    if (libidx_track_global(g_idx, track, &t) != LIB_OK) return;

    copy_to(g_title, sizeof(g_title), t.title);
    copy_to(g_path, sizeof(g_path), t.path);

    album = libidx_album_of_track(g_idx, track);
    if (album != LIBIDX_NONE) {
        copy_to(g_album, sizeof(g_album), libidx_album_name(g_idx, album));
        copy_to(g_artist, sizeof(g_artist),
                libidx_artist_name(g_idx, libidx_album_artist(g_idx, album)));
    }
}

/* Rebuilds the parts of np_state that change every frame. Focus, volume
 * mode and the enabled mask are owned by the input handler, so they are
 * deliberately not touched here. */
static void refresh_np(void)
{
    /* Album art — only for the now-playing screen itself. The cache makes
     * this free after the first call per album; the first call decodes. */
    g_np.art = NULL;
    g_np.art_size = 0u;
    if (g_screen == UI_NOW_PLAYING && g_idx != NULL &&
        pq_current(&g_pq) != PQ_NO_TRACK) {
        int size = np_art_size(g_np.layout);
        uint32_t album = libidx_album_of_track(g_idx, pq_current(&g_pq));
        g_np.art = plat_art_for(album, g_path, size);
        if (g_np.art != NULL) g_np.art_size = (uint16_t)size;
    }

    g_np.title  = g_title;
    g_np.artist = g_artist;
    g_np.album  = g_album;
    g_np.path   = g_path;
    g_np.format = plat_audio_format();

    g_np.elapsed_ms  = plat_audio_position_ms();
    g_np.duration_ms = plat_audio_duration_ms();

    g_np.sample_rate_hz = plat_audio_sample_rate();
    g_np.bit_depth      = plat_audio_bit_depth();
    g_np.channels       = plat_audio_channels();
    g_np.bitrate_kbps   = 0u;   /* the decoders do not report this yet */

    g_np.is_playing = !plat_audio_is_paused() && plat_audio_is_active();
    g_np.volume_pct = plat_audio_volume();
}

static bool start_track(uint32_t track)
{
    lib_track_t t;

    if (g_idx == NULL) return false;
    if (libidx_track_global(g_idx, track, &t) != LIB_OK) return false;

    plat_audio_stop();
    if (plat_audio_play(t.path) != 0) {
        /* The index says this file exists and the card disagrees. Never
         * silent: an empty now-playing screen with no explanation is the
         * worst possible outcome here. */
        printf("play FAILED: %s\r\n", t.path);
        pq_stop(&g_pq);
        capture_track(PQ_NO_TRACK);
        return false;
    }

    pq_start(&g_pq, track);
    capture_track(track);
    printf("play: %s\r\n", t.path);
    return true;
}

/* ------------------------------------------------------------------ */

void dap_ui_init(gfx_t *fb, st7789_t *tft, libidx_t *idx)
{
    plat_art_init();
    g_fb = fb;
    g_tft = tft;
    g_idx = idx;

    /* A missing or corrupt settings file is not an error worth stopping
     * for — config_load leaves defaults either way. */
    if (!config_load(&g_cfg, plat_libio_shared(), CFG_PATH)) {
        printf("config: using defaults\r\n");
    }
    apply_theme(g_cfg.theme);
    settings_init(&g_settings, g_cfg.theme, g_cfg.repeat, g_cfg.np_layout);

    browse_init(&g_browse, idx);
    browse_set_pinned(&g_browse, "Settings");
    pq_init(&g_pq, idx);
    pq_set_repeat(&g_pq, (pq_repeat_t)g_cfg.repeat);

    /* plat_audio_set_volume takes a delta, so restoring an absolute level
     * means asking where it currently is. */
    (void)plat_audio_set_volume((int)g_cfg.volume - (int)plat_audio_volume());

    memset(&g_np, 0, sizeof(g_np));
    g_np.enabled = NP_CTL_DEFAULT;
    g_np.focus = NP_CTL_PLAY;
    g_np.layout = g_cfg.np_layout;

    g_screen = UI_LIBRARY;
    g_dirty = true;
}

ui_screen_t dap_ui_screen(void) { return g_screen; }

/* ---------------------------------------------------------- input */

static void input_library(int delta, int btn)
{
    lib_track_t t;
    uint32_t gi;

    if (delta != 0) {
        browse_move(&g_browse, delta);
        g_dirty = true;
    }

    if (btn == 1) {
        browse_result_t r = browse_activate(&g_browse, &t, &gi);

        if (r == BROWSE_PINNED) {
            g_screen = UI_SETTINGS;
        } else if (r == BROWSE_PLAY) {
            /* Selecting the track that is already playing means "show me
             * it", not "start it again" — restarting would throw away the
             * position for a press that looks like navigation. */
            if (gi == pq_current(&g_pq) && plat_audio_is_active()) {
                g_screen = UI_NOW_PLAYING;
            } else if (start_track(gi)) {
                g_screen = UI_NOW_PLAYING;
            }
        }
        if (g_screen == UI_NOW_PLAYING) {
            g_np.focus = NP_CTL_PLAY;
            g_np.vol_active = false;
        }
        g_dirty = true;
    } else if (btn == 2) {
        /* At the top of the library there is nowhere further up, so the
         * gesture is free: use it to return to whatever is playing. */
        if (!browse_back(&g_browse) && pq_current(&g_pq) != PQ_NO_TRACK) {
            g_screen = UI_NOW_PLAYING;
            g_np.focus = NP_CTL_PLAY;
            g_np.vol_active = false;
        }
        g_dirty = true;
    }
}
static void activate_control(void)
{
    switch (g_np.focus) {
    case NP_CTL_PLAY:
        if (plat_audio_is_paused()) plat_audio_resume();
        else                        plat_audio_pause();
        pq_toggle_pause(&g_pq);
        break;

    case NP_CTL_VOL:
        /* A mode, not an action: the turn changes meaning until it is
         * pressed again. The overlay is what makes that visible. */
        g_np.vol_active = !g_np.vol_active;
        if (!g_np.vol_active) {
            /* Saved on leaving the mode rather than on every detent — a
             * card write per click of the encoder would be absurd. */
            g_cfg.volume = plat_audio_volume();
            save_config();
        }
        break;

    case NP_CTL_INFO:
        g_screen = UI_INFO;
        break;

    case NP_CTL_PREV: {
        uint32_t p;
        if (pq_prev(&g_pq, &p)) (void)start_track(p);
        break;
    }
    case NP_CTL_NEXT: {
        uint32_t n;
        if (pq_next(&g_pq, &n)) (void)start_track(n);
        break;
    }
    default:
        break;
    }
}

/* Brings the art-only controls up and restarts their timer. Harmless in the
 * standard layout, where they never go away. */
static void np_wake(void)
{
    g_np.overlay = true;
    g_overlay_at = HAL_GetTick();
}

static void input_now_playing(int delta, int btn)
{
    /* In art-only, the first touch just brings the controls up. Acting on
     * it too would mean a nudge to see the time pauses the music. Hold is
     * the exception: it always leaves, controls showing or not. */
    if (g_np.layout == (uint8_t)NP_LAYOUT_ART_ONLY && btn != 2) {
        bool was_hidden = !g_np.overlay;
        np_wake();
        if (was_hidden) {
            g_dirty = true;
            return;
        }
    }

    if (delta != 0) {
        if (g_np.vol_active) {
            g_np.volume_pct = plat_audio_set_volume(delta * 5);
        } else {
            (void)np_focus_move(&g_np, delta);
        }
        g_dirty = true;
    }

    if (btn == 1) {
        activate_control();
        g_dirty = true;
    } else if (btn == 2) {
        /* Leaving with volume mode still on would strand the turn in a
         * meaning the next screen does not have. */
        g_np.vol_active = false;
        g_screen = UI_LIBRARY;
        g_dirty = true;
    }
}

static void do_rescan(void)
{
    /* Everything derived from the old index dies with it: the playing
     * track index, the browse position, the cached strings. Rebuilding
     * them is cheaper than trying to map them across. */
    plat_audio_stop();
    pq_stop(&g_pq);
    capture_track(PQ_NO_TRACK);
    plat_art_forget();          /* album numbers are about to change */

    (void)dap_library_rescan();

    g_idx = dap_library();
    browse_init(&g_browse, g_idx);
    browse_set_pinned(&g_browse, "Settings");
    pq_init(&g_pq, g_idx);
    pq_set_repeat(&g_pq, (pq_repeat_t)g_cfg.repeat);
}

static void input_settings(int delta, int btn)
{
    if (delta != 0) {
        settings_move(&g_settings, delta);
        g_dirty = true;
    }

    if (btn == 1) {
        switch (settings_activate(&g_settings)) {
        case SET_ID_THEME:
            g_cfg.theme = settings_value(&g_settings, SET_ID_THEME);
            apply_theme(g_cfg.theme);   /* immediately, on this very frame */
            save_config();
            break;

        case SET_ID_REPEAT:
            g_cfg.repeat = settings_value(&g_settings, SET_ID_REPEAT);
            pq_set_repeat(&g_pq, (pq_repeat_t)g_cfg.repeat);
            save_config();
            break;

        case SET_ID_NP_LAYOUT:
            g_cfg.np_layout = settings_value(&g_settings, SET_ID_NP_LAYOUT);
            g_np.layout = g_cfg.np_layout;
            save_config();
            break;

        case SET_ID_RESCAN:
            do_rescan();
            g_screen = UI_LIBRARY;
            break;

        default:
            break;
        }
        g_dirty = true;
    } else if (btn == 2) {
        g_screen = UI_LIBRARY;
        g_dirty = true;
    }
}

static void input_info(int delta, int btn)
{
    (void)delta;
    if (btn == 2 || btn == 1) {
        g_screen = UI_NOW_PLAYING;
        g_dirty = true;
    }
}

void dap_ui_input(int delta, int btn)
{
    ui_screen_t before = g_screen;

    if (delta == 0 && btn == 0) return;

    switch (g_screen) {
    case UI_NOW_PLAYING: input_now_playing(delta, btn); break;
    case UI_INFO:        input_info(delta, btn);        break;
    case UI_SETTINGS:    input_settings(delta, btn);    break;
    case UI_LIBRARY:
    default:             input_library(delta, btn);     break;
    }

    /* Arriving at now-playing from anywhere shows the controls for a
     * moment, so you can see where the track is before they fade. */
    if (g_screen == UI_NOW_PLAYING && before != UI_NOW_PLAYING) {
        np_wake();
    }
}

/* ---------------------------------------------------------- paint */

/* One fingerprint per band of what the panel is currently showing. Most
 * frames change a small part of the screen — a list selection moving, the
 * scrubber advancing — so the push skips every band whose pixels match what
 * was sent last time. A full frame costs ~80 ms of SPI; moving the library
 * selection now costs two or three bands, ~10 ms.
 *
 * Starts invalid, because at boot the panel holds whatever main() drew
 * (the green flash), not anything this cache knows about. */
#define PANEL_MAX_BANDS 64
static uint32_t g_band_hash[PANEL_MAX_BANDS];
static bool     g_band_valid;

static void push_panel(void)
{
    const int w = g_fb->w;
    const int h = g_fb->h;
    int y, band;

    for (y = 0, band = 0; y < h; y += PANEL_BAND_ROWS, band++) {
        int rows = PANEL_BAND_ROWS;
        uint32_t hash;
        if (y + rows > h) rows = h - y;

        hash = gfx_band_hash(g_fb, y, rows);
        if (band < PANEL_MAX_BANDS) {
            if (g_band_valid && g_band_hash[band] == hash) continue;
            g_band_hash[band] = hash;
        }

        g_tft->bus->set_cs(g_tft->bus->ctx, true);
        st7789_set_window(g_tft, 0, (uint16_t)y, (uint16_t)(w - 1),
                          (uint16_t)(y + rows - 1));
        st7789_write_pixels(g_tft, g_fb->px + (size_t)y * (size_t)w,
                            (size_t)rows * (size_t)w);
        g_tft->bus->set_cs(g_tft->bus->ctx, false);

        /* CS is deasserted before this runs, so the SD card is free to use
         * the bus. This is the whole point of banding: the ring gets a
         * refill opportunity every few milliseconds instead of once per
         * frame. */
        plat_audio_service();
    }
    g_band_valid = true;
}

static void paint(void)
{
    if (g_fb == NULL || g_tft == NULL) return;

    switch (g_screen) {
    case UI_NOW_PLAYING:
        refresh_np();
        screen_now_playing_draw(g_fb, g_theme, &g_np);
        break;

    case UI_INFO:
        refresh_np();
        screen_info_draw(g_fb, g_theme, &g_np);
        break;

    case UI_SETTINGS:
        screen_settings_draw(g_fb, g_theme, &g_settings);
        break;

    case UI_LIBRARY:
    default: {
        int n = browse_fill_rows(&g_browse, g_rows, BROWSE_MAX_ROWS,
                                 pq_current(&g_pq));
        screen_library_draw_window(g_fb, g_theme, g_rows, n,
                                   browse_row_count(&g_browse),
                                   browse_selected(&g_browse),
                                   browse_scroll_top(&g_browse),
                                   browse_header(&g_browse),
                                   browse_level(&g_browse));
        break;
    }
    }

    push_panel();
    g_last_paint = HAL_GetTick();
    g_dirty = false;
}

void dap_ui_tick(void)
{
    uint32_t now = HAL_GetTick();

    /* End of track. plat_audio_stop() runs inside the audio service when a
     * file drains, so "the queue thinks it is playing but the platform is
     * idle" is exactly the end-of-track signal. */
    if (pq_state(&g_pq) == PQ_PLAYING && !plat_audio_is_active()) {
        uint32_t next;
        if (pq_track_finished(&g_pq, &next)) {
            (void)start_track(next);
        } else {
            capture_track(PQ_NO_TRACK);
        }
        g_dirty = true;
    }

    /* Art-only: the controls fade once the encoder has been left alone.
     * Volume mode holds them up — it is still waiting for input. */
    if (g_screen == UI_NOW_PLAYING &&
        g_np.layout == (uint8_t)NP_LAYOUT_ART_ONLY &&
        g_np.overlay && !g_np.vol_active &&
        (now - g_overlay_at) >= NP_OVERLAY_MS) {
        g_np.overlay = false;
        g_dirty = true;
    }

    /* The scrubber has to move on its own, but only while something is
     * actually playing and only while it is on screen — repainting a frame
     * that cannot have changed would burn SPI bandwidth for nothing. In
     * art-only with the controls down, the picture is static until the
     * next touch or track change. */
    if (!g_dirty && g_screen == UI_NOW_PLAYING &&
        pq_state(&g_pq) == PQ_PLAYING &&
        !(g_np.layout == (uint8_t)NP_LAYOUT_ART_ONLY && !g_np.overlay) &&
        (now - g_last_paint) >= TICK_REDRAW_MS) {
        g_dirty = true;
    }

    if (g_dirty) {
        paint();
    }
}
