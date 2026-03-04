#include "screen_now_playing.h"
#include "../themes/theme.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ─────────────────────────────────────────────
   Internal helpers
───────────────────────────────────────────── */

static void set_color(SDL_Renderer *r, Color c, Uint8 alpha)
{
    SDL_SetRenderDrawColor(r,
        (c >> 16) & 0xFF,
        (c >>  8) & 0xFF,
         c        & 0xFF,
        alpha);
}

static void fill_rounded_rect(SDL_Renderer *r, SDL_Rect rect, int radius)
{
    if (radius <= 0) { SDL_RenderFillRect(r, &rect); return; }
    if (radius > rect.w / 2) radius = rect.w / 2;
    if (radius > rect.h / 2) radius = rect.h / 2;

    SDL_Rect body  = { rect.x + radius, rect.y,          rect.w - 2*radius, rect.h          };
    SDL_Rect left  = { rect.x,          rect.y + radius, radius,            rect.h - 2*radius };
    SDL_Rect right = { rect.x + rect.w - radius, rect.y + radius, radius,   rect.h - 2*radius };
    SDL_RenderFillRect(r, &body);
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);

    for (int dy = 0; dy < radius; dy++) {
        int dx = (int)(sqrtf((float)(radius*radius - (radius-dy)*(radius-dy))) + 0.5f);
        SDL_RenderDrawLine(r, rect.x + radius - dx,              rect.y + dy,
                              rect.x + radius,                   rect.y + dy);
        SDL_RenderDrawLine(r, rect.x + rect.w - radius,          rect.y + dy,
                              rect.x + rect.w - radius + dx - 1, rect.y + dy);
        SDL_RenderDrawLine(r, rect.x + radius - dx,              rect.y + rect.h - 1 - dy,
                              rect.x + radius,                   rect.y + rect.h - 1 - dy);
        SDL_RenderDrawLine(r, rect.x + rect.w - radius,          rect.y + rect.h - 1 - dy,
                              rect.x + rect.w - radius + dx - 1, rect.y + rect.h - 1 - dy);
    }
}

static void draw_text_centered(SDL_Renderer *ren, TTF_Font *font,
                                const char *text, Color c, int cx, int cy)
{
    if (!text || !font) return;
    SDL_Color col = { (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { cx - surf->w/2, cy - surf->h/2, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font,
                      const char *text, Color c, int x, int y)
{
    if (!text || !font) return;
    SDL_Color col = { (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void fmt_time(char *buf, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(buf, 16, "%d:%02d", sec / 60, sec % 60);
}

/* ─────────────────────────────────────────────
   Word-wrap text renderer.

   Renders `text` word-wrapped within `max_w` pixels.
   Each line break advances by (line_h + 2) px.
   Stops after `max_lines` lines (0 = unlimited).
   Returns the y position after the final line.
───────────────────────────────────────────── */
static int draw_text_wrapped(SDL_Renderer *ren, TTF_Font *font,
                              const char *text, Color c,
                              int x, int y, int max_w, int max_lines)
{
    if (!text || !font || !*text) return y;

    SDL_Color col = { (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255 };
    int line_h = 0;
    { int dummy; TTF_SizeUTF8(font, "Ag", &dummy, &line_h); }

    const char *src   = text;
    int         lines = 0;

    while (*src) {
        /* Skip leading spaces at the start of each line */
        while (*src == ' ') src++;
        if (!*src) break;

        if (max_lines > 0 && lines >= max_lines) break;

        const char *line_start  = src;
        const char *last_fit    = src;   /* furthest word boundary that fit */
        const char *p           = src;
        int         first_word  = 1;

        while (*p) {
            /* Advance to end of current word */
            const char *word_end = p;
            while (*word_end && *word_end != ' ') word_end++;

            /* Measure from line_start to word_end */
            char tmp[512];
            int  len = (int)(word_end - line_start);
            if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
            memcpy(tmp, line_start, (size_t)len);
            tmp[len] = '\0';

            int tw;
            TTF_SizeUTF8(font, tmp, &tw, NULL);

            if (tw <= max_w || first_word) {
                last_fit   = word_end;
                first_word = 0;
                /* Advance past word and any spaces */
                p = word_end;
                while (*p == ' ') p++;
            } else {
                break;
            }
        }

        /* Render line_start..last_fit */
        int len = (int)(last_fit - line_start);
        if (len > 0) {
            char line_buf[512];
            if (len >= (int)sizeof(line_buf)) len = (int)sizeof(line_buf) - 1;
            memcpy(line_buf, line_start, (size_t)len);
            line_buf[len] = '\0';

            SDL_Surface *surf = TTF_RenderUTF8_Blended(font, line_buf, col);
            if (surf) {
                SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
                SDL_Rect dst = { x, y, surf->w, surf->h };
                SDL_RenderCopy(ren, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
                SDL_FreeSurface(surf);
            }
        }

        y += line_h + 2;
        lines++;
        src = last_fit;
        while (*src == ' ') src++;

        /* Safety: avoid infinite loop if a single word fills more than max_w */
        if (src == line_start) src++;
    }

    return y;
}

/* ─────────────────────────────────────────────
   Font cache  (loaded once in _init)
───────────────────────────────────────────── */
static TTF_Font *g_fonts[FONT_COUNT] = {0};
static const int FONT_SIZES[FONT_COUNT] = { 11, 14, 18, 24 };

void screen_now_playing_init(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        g_fonts[i] = TTF_OpenFont("assets/fonts/Roboto-Regular.ttf", FONT_SIZES[i]);
        if (!g_fonts[i]) {
            char path[128];
            snprintf(path, sizeof(path),
                     "/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf");
            g_fonts[i] = TTF_OpenFont(path, FONT_SIZES[i]);
        }
        if (!g_fonts[i]) {
            g_fonts[i] = TTF_OpenFont(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                FONT_SIZES[i]);
        }
    }
}

void screen_now_playing_destroy(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        if (g_fonts[i]) { TTF_CloseFont(g_fonts[i]); g_fonts[i] = NULL; }
    }
}

/* ─────────────────────────────────────────────
   Scrolling top-bar state
───────────────────────────────────────────── */
#define TOPBAR_PAUSE_FRAMES  90

static int  g_scroll_tick = 0;
static char g_scroll_last[128] = {0};

static void draw_topbar(SDL_Renderer *ren, const Theme *t,
                        const NowPlayingState *s, int w)
{
    set_color(ren, t->surface_alt, 255);
    SDL_RenderDrawLine(ren, 0, t->topbar_height - 1, w, t->topbar_height - 1);

    if (!g_fonts[FONT_SM]) return;

    char label[128];
    snprintf(label, sizeof(label), "%s  \xe2\x80\x93  %s",
             s->artist ? s->artist : "",
             s->album  ? s->album  : "");

    if (strncmp(label, g_scroll_last, sizeof(g_scroll_last)) != 0) {
        snprintf(g_scroll_last, sizeof(g_scroll_last), "%s", label);
        g_scroll_tick = 0;
    }

    const int PAD    = 8;
    const int cont_w = w - PAD * 2;
    const int cont_x = PAD;
    const int text_cy = t->topbar_height / 2;

    SDL_Color col = { (t->text_secondary>>16)&0xFF,
                      (t->text_secondary>>8)&0xFF,
                       t->text_secondary&0xFF, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_fonts[FONT_SM], label, col);
    if (!surf) return;

    int text_w = surf->w;

    if (text_w <= cont_w) {
        SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
        SDL_Rect dst = { cont_x + (cont_w - text_w)/2,
                         text_cy - surf->h/2,
                         surf->w, surf->h };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    } else {
        int excess        = text_w - cont_w;
        int scroll_frames = excess * 2;
        int cycle         = TOPBAR_PAUSE_FRAMES + scroll_frames + TOPBAR_PAUSE_FRAMES;
        int tick          = g_scroll_tick % cycle;

        int scroll_px;
        if (tick < TOPBAR_PAUSE_FRAMES)
            scroll_px = 0;
        else if (tick < TOPBAR_PAUSE_FRAMES + scroll_frames)
            scroll_px = (tick - TOPBAR_PAUSE_FRAMES) / 2;
        else
            scroll_px = excess;
        g_scroll_tick++;

        SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
        SDL_Rect clip = { cont_x, 0, cont_w, t->topbar_height };
        SDL_RenderSetClipRect(ren, &clip);
        SDL_Rect dst = { cont_x - scroll_px, text_cy - surf->h/2, surf->w, surf->h };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_RenderSetClipRect(ren, NULL);
        SDL_DestroyTexture(tex);
    }

    SDL_FreeSurface(surf);
}

static void draw_album_art(SDL_Renderer *ren, const Theme *t,
                           const NowPlayingState *s,
                           int w, int art_y, int art_size)
{
    int art_x = (w - art_size) / 2;
    SDL_Rect art_rect = { art_x, art_y, art_size, art_size };

    if (s->art_texture) {
        SDL_RenderCopy(ren, (SDL_Texture *)s->art_texture, NULL, &art_rect);
    } else {
        set_color(ren, t->art_placeholder_bg, 255);
        SDL_RenderFillRect(ren, &art_rect);

        set_color(ren, t->art_placeholder_fg, 255);
        int cx = art_x + art_size/2, cy = art_y + art_size/2;
        int outer = art_size/2 - 6;
        for (int r = outer; r > 4; r -= 8) {
            for (int deg = 0; deg < 360; deg += 3) {
                float a1 = (float)deg     * 3.14159f / 180.f;
                float a2 = (float)(deg+3) * 3.14159f / 180.f;
                SDL_RenderDrawLine(ren,
                    (int)(cx + r * cosf(a1)), (int)(cy + r * sinf(a1)),
                    (int)(cx + r * cosf(a2)), (int)(cy + r * sinf(a2)));
            }
        }
        set_color(ren, t->accent, 200);
        SDL_Rect dot = { cx - 3, cy - 3, 6, 6 };
        fill_rounded_rect(ren, dot, 3);
    }
}

static void draw_progress(SDL_Renderer *ren, const Theme *t,
                          const NowPlayingState *s,
                          int bar_x, int bar_y, int bar_w)
{
    float progress = s->progress;
    if (progress < 0.f) progress = 0.f;
    if (progress > 1.f) progress = 1.f;

    int filled_w = (int)(bar_w * progress);
    int scrub_cx = bar_x + filled_w;

    set_color(ren, t->accent_dim, 255);
    SDL_Rect track = { bar_x, bar_y - t->bar_height/2, bar_w, t->bar_height };
    SDL_RenderFillRect(ren, &track);

    if (filled_w > 0) {
        set_color(ren, t->accent, 255);
        SDL_Rect filled = { bar_x, bar_y - t->bar_height/2, filled_w, t->bar_height };
        SDL_RenderFillRect(ren, &filled);
    }

    set_color(ren, t->accent, 255);
    SDL_Rect pill = {
        scrub_cx - t->pill_width  / 2,
        bar_y    - t->pill_height / 2,
        t->pill_width,
        t->pill_height
    };
    fill_rounded_rect(ren, pill, t->pill_radius);
}

static void draw_timestamps(SDL_Renderer *ren, const Theme *t,
                            const NowPlayingState *s,
                            int bar_x, int ts_y, int bar_w)
{
    char elapsed[16], duration[16];
    fmt_time(elapsed,  s->elapsed_sec);
    fmt_time(duration, s->duration_sec);

    draw_text(ren, g_fonts[FONT_SM], elapsed, t->text_inactive, bar_x, ts_y);

    if (g_fonts[FONT_SM]) {
        int tw, th; (void)th;
        TTF_SizeUTF8(g_fonts[FONT_SM], duration, &tw, &th);
        draw_text(ren, g_fonts[FONT_SM], duration,
                  t->text_inactive, bar_x + bar_w - tw, ts_y);
    }
}

/* ═══════════════════════════════════════════
   PUBLIC: screen_now_playing_draw
═══════════════════════════════════════════ */
void screen_now_playing_draw(void        *renderer,
                             const Theme *theme,
                             const NowPlayingState *state,
                             int          screen_w,
                             int          screen_h)
{
    SDL_Renderer *ren = (SDL_Renderer *)renderer;
    const Theme  *t   = theme;
    const NowPlayingState *s = state;

    set_color(ren, t->bg, 255);
    SDL_RenderClear(ren);

    const int MARGIN   = 16;
    const int ART_SIZE = 160;
    const int ART_Y    = t->topbar_height + 8;

    draw_topbar(ren, t, s, screen_w);
    draw_album_art(ren, t, s, screen_w, ART_Y, ART_SIZE);

    int bar_x = MARGIN;
    int bar_w = screen_w - MARGIN * 2;
    int bar_y = ART_Y + ART_SIZE + 10;
    int ts_y  = bar_y + 6;

    draw_progress(ren, t, s, bar_x, bar_y, bar_w);
    draw_timestamps(ren, t, s, bar_x, ts_y, bar_w);

    /* Navigation hint: ▲ Info   ▼ Library */
    draw_text_centered(ren, g_fonts[FONT_SM],
                       "\xe2\x96\xb2 Info   \xe2\x96\xbc Library",
                       t->text_inactive, screen_w / 2, screen_h - 6);

    (void)screen_h;
}

/* ═══════════════════════════════════════════
   PUBLIC: screen_info_panel_draw

   Uses word-wrap (max 2 lines per field) so
   long values never exit the screen bounds.
═══════════════════════════════════════════ */
void screen_info_panel_draw(void        *renderer,
                            const Theme *theme,
                            const NowPlayingState *state,
                            int          screen_w,
                            int          screen_h)
{
    SDL_Renderer *ren = (SDL_Renderer *)renderer;
    const Theme  *t   = theme;
    const NowPlayingState *s = state;

    set_color(ren, t->bg, 255);
    SDL_RenderClear(ren);

    /* Header */
    set_color(ren, t->surface_alt, 255);
    SDL_RenderDrawLine(ren, 0, t->topbar_height - 1, screen_w, t->topbar_height - 1);
    draw_text_centered(ren, g_fonts[FONT_SM], "Track Info",
                       t->text_secondary, screen_w / 2, t->topbar_height / 2);

    const int LABEL_X = 8;
    const int VALUE_X = 68;
    const int VALUE_W = screen_w - VALUE_X - 8;
    const int ROW_GAP = 5;   /* vertical gap between fields */
    int y = t->topbar_height + 8;

    /* FIELD: draw label at y (left), draw wrapped value at y (right of label).
       Advances y past the tallest of the two, then adds ROW_GAP.          */
    #define FIELD(lbl, val) do {                                              \
        draw_text(ren, g_fonts[FONT_SM], (lbl), t->text_secondary, LABEL_X, y); \
        int next_y = draw_text_wrapped(ren, g_fonts[FONT_SM], (val),         \
                                       t->text_primary, VALUE_X, y,          \
                                       VALUE_W, 2);                           \
        y = next_y + ROW_GAP;                                                 \
    } while (0)

    FIELD("Track",   s->track_title  ? s->track_title : "\xe2\x80\x94");
    FIELD("Artist",  s->artist       ? s->artist      : "\xe2\x80\x94");
    FIELD("Album",   s->album        ? s->album       : "\xe2\x80\x94");
    FIELD("Year",    s->year && s->year[0] ? s->year  : "\xe2\x80\x94");

    /* Format string: "FLAC  24-bit / 96.0 kHz" */
    char fmt_str[64];
    if (s->file_format && s->bit_depth > 0 && s->sample_rate_hz > 0)
        snprintf(fmt_str, sizeof(fmt_str), "%s  %d-bit / %.1f kHz",
                 s->file_format, s->bit_depth,
                 (float)s->sample_rate_hz / 1000.f);
    else
        snprintf(fmt_str, sizeof(fmt_str), "%s",
                 s->file_format ? s->file_format : "\xe2\x80\x94");
    FIELD("Format", fmt_str);

    /* Bitrate */
    char br_str[32];
    if (s->bitrate_kbps > 0)
        snprintf(br_str, sizeof(br_str), "%d kbps", s->bitrate_kbps);
    else
        snprintf(br_str, sizeof(br_str), "\xe2\x80\x94");
    FIELD("Bitrate", br_str);

    #undef FIELD

    /* Navigation hint */
    draw_text_centered(ren, g_fonts[FONT_SM], "\xe2\x96\xb2 Back",
                       t->text_inactive, screen_w / 2, screen_h - 6);

    (void)screen_h;
}

/* ═══════════════════════════════════════════
   PUBLIC: draw_volume_overlay

   Drawn on top of the active screen whenever
   volume changes. Call after the screen draw,
   before SDL_RenderPresent.
═══════════════════════════════════════════ */
void draw_volume_overlay(void *renderer, const Theme *theme,
                         int volume_pct, int screen_w)
{
    SDL_Renderer *ren = (SDL_Renderer *)renderer;
    const Theme  *t   = theme;

    const int OVL_W = 190, OVL_H = 22;
    const int OVL_X = (screen_w - OVL_W) / 2;
    const int OVL_Y = 30;   /* just below top bar */

    /* Background pill */
    set_color(ren, t->surface, 255);
    SDL_Rect bg = { OVL_X, OVL_Y, OVL_W, OVL_H };
    fill_rounded_rect(ren, bg, 4);

    /* Border */
    set_color(ren, t->surface_alt, 255);
    SDL_RenderDrawRect(ren, &bg);

    /* "VOL" label */
    draw_text(ren, g_fonts[FONT_SM], "VOL",
              t->text_secondary, OVL_X + 6, OVL_Y + 5);

    /* Progress bar */
    const int BAR_X = OVL_X + 36;
    const int BAR_W = OVL_W - 72;
    const int BAR_H = 4;
    const int BAR_Y = OVL_Y + (OVL_H - BAR_H) / 2;

    set_color(ren, t->accent_dim, 255);
    SDL_Rect track = { BAR_X, BAR_Y, BAR_W, BAR_H };
    SDL_RenderFillRect(ren, &track);

    if (volume_pct > 0) {
        set_color(ren, t->accent, 255);
        SDL_Rect fill = { BAR_X, BAR_Y, BAR_W * volume_pct / 100, BAR_H };
        SDL_RenderFillRect(ren, &fill);
    }

    /* Percentage text */
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", volume_pct);
    draw_text(ren, g_fonts[FONT_SM], pct,
              t->text_primary, OVL_X + OVL_W - 28, OVL_Y + 5);
}
