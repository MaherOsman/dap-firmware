#include "gfx.h"

void gfx_init(gfx_t *g, uint16_t *storage, int w, int h)
{
    g->px = storage;
    g->w  = w;
    g->h  = h;
    gfx_clip_reset(g);
}

void gfx_clip_reset(gfx_t *g)
{
    g->cx0 = 0;
    g->cy0 = 0;
    g->cx1 = g->w;
    g->cy1 = g->h;
}

void gfx_clip(gfx_t *g, int x, int y, int w, int h)
{
    int x1 = x + w;
    int y1 = y + h;
    g->cx0 = x  < 0 ? 0 : x;
    g->cy0 = y  < 0 ? 0 : y;
    g->cx1 = x1 > g->w ? g->w : x1;
    g->cy1 = y1 > g->h ? g->h : y1;
    if (g->cx1 < g->cx0) {
        g->cx1 = g->cx0;
    }
    if (g->cy1 < g->cy0) {
        g->cy1 = g->cy0;
    }
}

uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t gfx_rgb888(uint32_t c)
{
    return gfx_rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}

void gfx_pixel(gfx_t *g, int x, int y, uint16_t color)
{
    if (x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1) {
        return;
    }
    g->px[(size_t)y * g->w + x] = color;
}

uint16_t gfx_get(const gfx_t *g, int x, int y)
{
    if (x < 0 || x >= g->w || y < 0 || y >= g->h) {
        return 0;
    }
    return g->px[(size_t)y * g->w + x];
}

void gfx_fill_rect(gfx_t *g, int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < g->cx0 ? g->cx0 : x;
    int y0 = y < g->cy0 ? g->cy0 : y;
    int x1 = x + w > g->cx1 ? g->cx1 : x + w;
    int y1 = y + h > g->cy1 ? g->cy1 : y + h;

    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = g->px + (size_t)yy * g->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void gfx_clear(gfx_t *g, uint16_t color)
{
    int sx0 = g->cx0, sy0 = g->cy0, sx1 = g->cx1, sy1 = g->cy1;
    gfx_clip_reset(g);
    gfx_fill_rect(g, 0, 0, g->w, g->h, color);
    g->cx0 = sx0; g->cy0 = sy0; g->cx1 = sx1; g->cy1 = sy1;
}

void gfx_hline(gfx_t *g, int x, int y, int w, uint16_t color)
{
    gfx_fill_rect(g, x, y, w, 1, color);
}

void gfx_vline(gfx_t *g, int x, int y, int h, uint16_t color)
{
    gfx_fill_rect(g, x, y, 1, h, color);
}

void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(g, x, y, w, color);
    gfx_hline(g, x, y + h - 1, w, color);
    gfx_vline(g, x, y, h, color);
    gfx_vline(g, x + w - 1, y, h, color);
}

void gfx_fill_round_rect(gfx_t *g, int x, int y, int w, int h, int r,
                         uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    /* Clamp the radius so a fat radius on a thin pill degrades gracefully
     * instead of drawing garbage. */
    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) {
        r = max_r;
    }
    if (r <= 0) {
        gfx_fill_rect(g, x, y, w, h, color);
        return;
    }

    /* Middle slab, then the two end caps row by row. */
    gfx_fill_rect(g, x, y + r, w, h - 2 * r, color);

    for (int dy = 0; dy < r; dy++) {
        /* Horizontal half-extent of the circle at this row. */
        int yy = r - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + yy * yy <= r * r) {
            dx++;
        }
        int inset = r - dx;
        gfx_fill_rect(g, x + inset, y + dy, w - 2 * inset, 1, color);
        gfx_fill_rect(g, x + inset, y + h - 1 - dy, w - 2 * inset, 1, color);
    }
}

/* ---- text ---- */

typedef struct {
    gfx_t   *g;
    uint16_t color;
} plot_ctx_t;

static void plot_cb(void *ctx, int x, int y)
{
    plot_ctx_t *p = ctx;
    gfx_pixel(p->g, x, y, p->color);
}

int gfx_text(gfx_t *g, const font_t *f, const char *s, int x, int y,
             uint16_t color)
{
    plot_ctx_t ctx = {g, color};
    return font_draw_text(f, s, x, y, plot_cb, &ctx);
}

bool gfx_text_ellipsis(gfx_t *g, const font_t *f, const char *s, int x, int y,
                       int max_w, uint16_t color)
{
    plot_ctx_t ctx = {g, color};
    return font_draw_text_ellipsis(f, s, x, y, max_w, plot_cb, &ctx);
}

int gfx_text_centered(gfx_t *g, const font_t *f, const char *s, int x, int y,
                      int w, uint16_t color)
{
    plot_ctx_t ctx = {g, color};
    return font_draw_text_centered(f, s, x, y, w, plot_cb, &ctx);
}

int gfx_text_right(gfx_t *g, const font_t *f, const char *s, int right_x,
                   int y, uint16_t color)
{
    plot_ctx_t ctx = {g, color};
    return font_draw_text_right(f, s, right_x, y, plot_cb, &ctx);
}
