#include "font.h"

#define ELLIPSIS "..."

/* Characters outside the baked range render as this. Silently dropping them
 * makes "Bj�rk" look like "Bjrk", which is worse than an obvious box. */
#define FALLBACK_CHAR '?'

static const font_glyph_t *glyph_for(const font_t *f, unsigned char c)
{
    if (c < f->first || c > f->last) {
        c = FALLBACK_CHAR;
        if (c < f->first || c > f->last) {
            return NULL;
        }
    }
    return &f->glyphs[c - f->first];
}

int font_text_width_n(const font_t *f, const char *s, size_t n)
{
    int w = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        const font_glyph_t *g = glyph_for(f, (unsigned char)s[i]);
        if (g) {
            w += g->advance;
        }
    }
    return w;
}

int font_text_width(const font_t *f, const char *s)
{
    int w = 0;
    for (const char *p = s; *p; p++) {
        const font_glyph_t *g = glyph_for(f, (unsigned char)*p);
        if (g) {
            w += g->advance;
        }
    }
    return w;
}

static void draw_glyph(const font_t *f, const font_glyph_t *g, int pen_x,
                       int baseline_y, font_plot_fn plot, void *ctx)
{
    if (g->w == 0 || g->h == 0) {
        return; /* space — advance only, no ink */
    }
    int stride = (g->w + 7) / 8;
    const uint8_t *bits = f->bitmap + g->off;
    int ox = pen_x + g->xo;
    int oy = baseline_y + g->yo;

    for (int row = 0; row < g->h; row++) {
        const uint8_t *r = bits + (size_t)row * stride;
        for (int col = 0; col < g->w; col++) {
            if (r[col >> 3] & (0x80u >> (col & 7))) {
                plot(ctx, ox + col, oy + row);
            }
        }
    }
}

int font_draw_text(const font_t *f, const char *s, int x, int y,
                   font_plot_fn plot, void *ctx)
{
    /* y is the top of the line; glyph offsets are baseline-relative. */
    int baseline = y + f->ascent;
    for (const char *p = s; *p; p++) {
        const font_glyph_t *g = glyph_for(f, (unsigned char)*p);
        if (!g) {
            continue;
        }
        draw_glyph(f, g, x, baseline, plot, ctx);
        x += g->advance;
    }
    return x;
}

size_t font_fit_chars(const font_t *f, const char *s, int max_w)
{
    int w = 0;
    size_t i = 0;
    for (; s[i]; i++) {
        const font_glyph_t *g = glyph_for(f, (unsigned char)s[i]);
        int adv = g ? g->advance : 0;
        if (w + adv > max_w) {
            break;
        }
        w += adv;
    }
    return i;
}

bool font_draw_text_ellipsis(const font_t *f, const char *s, int x, int y,
                             int max_w, font_plot_fn plot, void *ctx)
{
    if (max_w <= 0) {
        return true;
    }
    if (font_text_width(f, s) <= max_w) {
        font_draw_text(f, s, x, y, plot, ctx);
        return false;
    }

    int ell_w = font_text_width(f, ELLIPSIS);
    int baseline = y + f->ascent;

    /* If even the ellipsis doesn't fit, draw nothing rather than overflow. */
    if (ell_w > max_w) {
        return true;
    }

    int budget = max_w - ell_w;
    size_t n = font_fit_chars(f, s, budget);

    /* Don't leave a dangling space before the ellipsis — "Chopin ..." reads
     * worse than "Chopin...". */
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }

    int pen = x;
    for (size_t i = 0; i < n; i++) {
        const font_glyph_t *g = glyph_for(f, (unsigned char)s[i]);
        if (!g) {
            continue;
        }
        draw_glyph(f, g, pen, baseline, plot, ctx);
        pen += g->advance;
    }
    font_draw_text(f, ELLIPSIS, pen, y, plot, ctx);
    return true;
}

int font_draw_text_centered(const font_t *f, const char *s, int x, int y,
                            int w, font_plot_fn plot, void *ctx)
{
    int tw = font_text_width(f, s);
    int sx = x + (w - tw) / 2;
    if (sx < x) {
        sx = x;
    }
    font_draw_text(f, s, sx, y, plot, ctx);
    return sx;
}

int font_draw_text_right(const font_t *f, const char *s, int right_x, int y,
                         font_plot_fn plot, void *ctx)
{
    int sx = right_x - font_text_width(f, s);
    font_draw_text(f, s, sx, y, plot, ctx);
    return sx;
}
