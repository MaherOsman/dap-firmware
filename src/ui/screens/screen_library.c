#include "screen_library.h"
#include "../themes/theme.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ─────────────────────────────────────────────
   Font cache
───────────────────────────────────────────── */
static TTF_Font *g_fonts[FONT_COUNT] = {0};
static const int FONT_SIZES[FONT_COUNT] = { 11, 14, 18, 24 };

void screen_library_init(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        g_fonts[i] = TTF_OpenFont("assets/fonts/Roboto-Regular.ttf", FONT_SIZES[i]);
        if (!g_fonts[i])
            g_fonts[i] = TTF_OpenFont("/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf", FONT_SIZES[i]);
        if (!g_fonts[i])
            g_fonts[i] = TTF_OpenFont("/usr/share/fonts/google-droid-sans-fonts/DroidSans.ttf", FONT_SIZES[i]);
        if (!g_fonts[i])
            g_fonts[i] = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", FONT_SIZES[i]);
        if (!g_fonts[i])
            g_fonts[i] = TTF_OpenFont("/usr/share/fonts/google-carlito-fonts/Carlito-Regular.ttf", FONT_SIZES[i]);
    }
}

void screen_library_destroy(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        if (g_fonts[i]) { TTF_CloseFont(g_fonts[i]); g_fonts[i] = NULL; }
    }
}

/* ─────────────────────────────────────────────
   Internal draw helpers
───────────────────────────────────────────── */
static void set_color(SDL_Renderer *r, Color c, Uint8 alpha)
{
    SDL_SetRenderDrawColor(r, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, alpha);
}

static void fill_rrect(SDL_Renderer *r, SDL_Rect rect, int radius)
{
    if (radius <= 0) { SDL_RenderFillRect(r, &rect); return; }
    if (radius > rect.w/2) radius = rect.w/2;
    if (radius > rect.h/2) radius = rect.h/2;
    SDL_Rect body  = { rect.x+radius, rect.y,         rect.w-2*radius, rect.h          };
    SDL_Rect left  = { rect.x,        rect.y+radius,  radius,          rect.h-2*radius };
    SDL_Rect right = { rect.x+rect.w-radius, rect.y+radius, radius,    rect.h-2*radius };
    SDL_RenderFillRect(r, &body);
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
    for (int dy = 0; dy < radius; dy++) {
        int dx = (int)(sqrtf((float)(radius*radius-(radius-dy)*(radius-dy)))+0.5f);
        SDL_RenderDrawLine(r, rect.x+radius-dx, rect.y+dy, rect.x+radius, rect.y+dy);
        SDL_RenderDrawLine(r, rect.x+rect.w-radius, rect.y+dy,
                              rect.x+rect.w-radius+dx-1, rect.y+dy);
        SDL_RenderDrawLine(r, rect.x+radius-dx, rect.y+rect.h-1-dy,
                              rect.x+radius,    rect.y+rect.h-1-dy);
        SDL_RenderDrawLine(r, rect.x+rect.w-radius, rect.y+rect.h-1-dy,
                              rect.x+rect.w-radius+dx-1, rect.y+rect.h-1-dy);
    }
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font,
                      const char *text, Color c, int x, int y)
{
    if (!text || !font || !*text) return;
    SDL_Color col = { (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void draw_text_centered(SDL_Renderer *ren, TTF_Font *font,
                                const char *text, Color c, int cx, int cy)
{
    if (!text || !font || !*text) return;
    SDL_Color col = { (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst = { cx-surf->w/2, cy-surf->h/2, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

/* Truncate with "…" to fit max_w pixels */
static void truncate_fit(TTF_Font *font, const char *src,
                          char *dst, int dst_sz, int max_w)
{
    snprintf(dst, dst_sz, "%s", src);
    if (!font) return;
    int tw; TTF_SizeUTF8(font, dst, &tw, NULL);
    if (tw <= max_w) return;
    int len = (int)strlen(dst);
    while (len > 0) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s\xe2\x80\xa6", len, dst);
        TTF_SizeUTF8(font, tmp, &tw, NULL);
        if (tw <= max_w) { snprintf(dst, dst_sz, "%s", tmp); return; }
        len--;
    }
    snprintf(dst, dst_sz, "\xe2\x80\xa6");
}

/* ═══════════════════════════════════════════
   PUBLIC: screen_library_draw

   320×240 layout:
     [  0 – 23]  header (level name / breadcrumb)
     [ 24 – 224] track list
     [225 – 239] hint bar
═══════════════════════════════════════════ */
void screen_library_draw(void         *renderer,
                          const Theme  *theme,
                          const LibRow *rows,
                          int           row_count,
                          int           selected,
                          int           scroll_top,
                          const char   *header,
                          LibLevel      level,
                          int           screen_w,
                          int           screen_h)
{
    SDL_Renderer *ren = (SDL_Renderer *)renderer;
    const Theme  *t   = theme;

    set_color(ren, t->bg, 255);
    SDL_RenderClear(ren);

    /* ── Header ── */
    const int HDR_H = t->topbar_height;
    set_color(ren, t->surface_alt, 255);
    SDL_RenderDrawLine(ren, 0, HDR_H - 1, screen_w, HDR_H - 1);

    /* Indent indicator dots showing depth: • Library  • Artist  • Album */
    const char *depth_labels[] = { "Library", NULL, NULL };
    (void)depth_labels; /* not used directly — just show header */
    draw_text_centered(ren, g_fonts[FONT_SM], header,
                       t->text_secondary, screen_w / 2, HDR_H / 2);

    /* Small left-arrow breadcrumb hint when not at root */
    if (level > LIB_LEVEL_ARTIST) {
        draw_text(ren, g_fonts[FONT_SM], "\xe2\x86\x90",
                  t->text_inactive, 6, HDR_H / 2 - 5);
    }

    /* ── List ── */
    const int LIST_TOP = HDR_H;
    const int HINT_H   = 15;
    const int LIST_BOT = screen_h - HINT_H;
    const int ROW_H    = LIBRARY_ROW_H;
    const int MARGIN   = 10;

    /* Indentation per level for the text content */
    const int INDENT[] = { 0, 8, 16 };
    int text_indent = INDENT[level];

    /* Chevron "›" width — pre-measure once */
    int chevron_w = 0;
    if (g_fonts[FONT_SM]) {
        TTF_SizeUTF8(g_fonts[FONT_SM], "\xe2\x80\xba ", &chevron_w, NULL);
    }

    /* Clip to list area */
    SDL_Rect clip = { 0, LIST_TOP, screen_w, LIST_BOT - LIST_TOP };
    SDL_RenderSetClipRect(ren, &clip);

    for (int i = scroll_top; i < row_count; i++) {
        int row_y = LIST_TOP + (i - scroll_top) * ROW_H;
        if (row_y + ROW_H > LIST_BOT) break;

        int is_sel     = (i == selected);
        int is_current = rows[i].is_current;

        /* Selected row highlight */
        if (is_sel) {
            set_color(ren, t->accent_dim, 255);
            SDL_Rect hl = { 0, row_y, screen_w, ROW_H };
            fill_rrect(ren, hl, 0);
        }

        /* Chevron for artist / album rows */
        int text_x = MARGIN + text_indent;
        if (rows[i].has_sub) {
            /* Draw "›" on the far right */
            int chev_x = screen_w - chevron_w - MARGIN;
            Color chev_col = is_sel ? t->text_primary : t->text_inactive;
            draw_text(ren, g_fonts[FONT_SM], "\xe2\x80\xba",
                      chev_col, chev_x, row_y + (ROW_H - 11) / 2);
        }

        /* Now-playing indicator ▶ (track level only) */
        if (!rows[i].has_sub && is_current) {
            draw_text(ren, g_fonts[FONT_SM], "\xe2\x96\xb6",
                      t->accent, text_x, row_y + (ROW_H - 11) / 2);
            text_x += 12;
        }

        /* Row text — truncated to fit, accounting for chevron space */
        int avail_w = screen_w - text_x - MARGIN
                    - (rows[i].has_sub ? chevron_w + MARGIN : 0);
        char display[256];
        truncate_fit(g_fonts[FONT_SM], rows[i].text ? rows[i].text : "",
                     display, sizeof(display), avail_w);

        Color text_col = is_sel      ? t->text_primary
                       : is_current  ? t->accent
                       :               t->text_secondary;

        draw_text(ren, g_fonts[FONT_SM], display,
                  text_col, text_x, row_y + (ROW_H - 11) / 2);
    }

    SDL_RenderSetClipRect(ren, NULL);

    /* Scrollbar */
    int visible_rows = (LIST_BOT - LIST_TOP) / ROW_H;
    if (row_count > visible_rows) {
        int bar_h = (LIST_BOT - LIST_TOP) * visible_rows / row_count;
        int bar_y = LIST_TOP + (LIST_BOT - LIST_TOP) * scroll_top / row_count;
        set_color(ren, t->surface_alt, 255);
        SDL_Rect sb = { screen_w - 3, bar_y, 2, bar_h };
        SDL_RenderFillRect(ren, &sb);
    }

    /* ── Hint bar ── */
    set_color(ren, t->surface_alt, 255);
    SDL_RenderDrawLine(ren, 0, LIST_BOT, screen_w, LIST_BOT);

    const char *hint =
        (level == LIB_LEVEL_TRACK)
        ? "\xe2\x86\x91\xe2\x86\x93 scroll   SPACE play   \xe2\x86\x90 back"
        : "\xe2\x86\x91\xe2\x86\x93 scroll   \xe2\x86\x92 open   \xe2\x86\x90 back";
    draw_text_centered(ren, g_fonts[FONT_SM], hint,
                       t->text_inactive, screen_w / 2, LIST_BOT + HINT_H / 2);
}
