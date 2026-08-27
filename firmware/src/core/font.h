/*
 * font — 1-bit proportional bitmap fonts baked into flash.
 *
 * The STM32 has no font engine, so glyphs are rasterised from Roboto on a PC
 * by tools/fontgen.py and checked in as C arrays. Roboto is deliberate: it's
 * the same typeface the SDL simulator used, so the hardware UI matches the
 * design work already done there.
 *
 * Glyphs are proportional, not fixed-width — a lowercase 'i' takes 3 px where
 * an 'M' takes 10. On a 240 px wide screen that difference is roughly four
 * extra characters per line, which matters when you're showing track titles.
 *
 * Drawing is deliberately separated from *where* it draws: font_draw_text()
 * takes a pixel-plot callback. That keeps this module testable on a PC (plot
 * into an image) and reusable on hardware (plot into a framebuffer) with no
 * changes.
 */
#ifndef FONT_H
#define FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  w;        /* bitmap width in pixels                        */
    uint8_t  h;        /* bitmap height in pixels                       */
    int8_t   xo;       /* x offset from the pen position                */
    int8_t   yo;       /* y offset from the baseline (negative == above) */
    uint8_t  advance;  /* how far the pen moves after drawing           */
    uint16_t off;      /* byte offset into the font's bitmap blob       */
} font_glyph_t;

typedef struct {
    const char         *name;
    uint8_t             height;  /* ascent + descent — use for line spacing */
    uint8_t             ascent;  /* baseline offset from the top of a line  */
    uint8_t             first;   /* first character code present            */
    uint8_t             last;    /* last character code present             */
    const font_glyph_t *glyphs;
    const uint8_t      *bitmap;
} font_t;

/* The three sizes, matching the simulator's FONT_SM / FONT_MD / FONT_LG. */
extern const font_t font_sm;  /* Roboto 11 — timestamps, hints, labels */
extern const font_t font_md;  /* Roboto 14 — artist, list rows         */
extern const font_t font_lg;  /* Roboto Medium 18 — track titles       */

/*
 * Pixel plotting callback. Called once per lit pixel; the renderer never
 * asks for the pixels it isn't setting, so a 1-bit glyph costs exactly as
 * many calls as it has ink.
 */
typedef void (*font_plot_fn)(void *ctx, int x, int y);

/* Width in pixels the string would occupy. Does not draw anything. */
int font_text_width(const font_t *f, const char *s);
/* Width of the first `n` bytes — used by the truncation logic. */
int font_text_width_n(const font_t *f, const char *s, size_t n);

/*
 * Draw `s` with the pen starting at (x, y), where y is the TOP of the line
 * (not the baseline — top-left origin is what UI layout code actually wants).
 * Returns the x position just past the last glyph.
 */
int font_draw_text(const font_t *f, const char *s, int x, int y,
                   font_plot_fn plot, void *ctx);

/*
 * Draw `s` clipped to `max_w` pixels, appending an ellipsis if it doesn't
 * fit. Track titles are routinely longer than the screen, so this is the
 * common case, not an edge case.
 *
 * Returns true if the text was truncated.
 */
bool font_draw_text_ellipsis(const font_t *f, const char *s, int x, int y,
                             int max_w, font_plot_fn plot, void *ctx);

/* Centre `s` within [x, x+w). Returns the x it actually started at. */
int font_draw_text_centered(const font_t *f, const char *s, int x, int y,
                            int w, font_plot_fn plot, void *ctx);

/* Right-align `s` so it ends at `right_x`. */
int font_draw_text_right(const font_t *f, const char *s, int right_x, int y,
                         font_plot_fn plot, void *ctx);

/*
 * How many bytes of `s` fit in `max_w` pixels. Useful for scrolling marquee
 * text later on.
 */
size_t font_fit_chars(const font_t *f, const char *s, int max_w);

#endif /* FONT_H */
