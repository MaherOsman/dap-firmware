/*
 * gfx — a tiny RGB565 framebuffer and the drawing primitives the UI needs.
 *
 * Why a framebuffer at all, when the ST7789 has its own memory: drawing
 * straight to the panel over SPI means every overlapping element causes a
 * visible repaint, and text on a coloured background needs two passes. A
 * 240x240x16bpp buffer is 115,200 bytes — which the H7 has plenty of (1 MB
 * of RAM) — and it lets us compose a frame in RAM and push it out in one
 * DMA transfer.
 *
 * On the PC the exact same buffer gets written out as a PNG, so what you see
 * in a preview image is bit-for-bit what the panel will show.
 */
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"

typedef struct {
    uint16_t *px;      /* w*h RGB565 pixels */
    int       w;
    int       h;
    /* Clip rectangle — all drawing is confined to this. Set by gfx_clip(). */
    int       cx0, cy0, cx1, cy1;
} gfx_t;

void gfx_init(gfx_t *g, uint16_t *storage, int w, int h);

/* Restrict drawing to [x, x+w) x [y, y+h). gfx_clip_reset() opens it back up. */
void gfx_clip(gfx_t *g, int x, int y, int w, int h);
void gfx_clip_reset(gfx_t *g);

uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);
/* Convert the simulator's 0x00RRGGBB colours to RGB565. */
uint16_t gfx_rgb888(uint32_t rgb888);

void gfx_clear(gfx_t *g, uint16_t color);
void gfx_pixel(gfx_t *g, int x, int y, uint16_t color);
void gfx_fill_rect(gfx_t *g, int x, int y, int w, int h, uint16_t color);
void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t color);
void gfx_hline(gfx_t *g, int x, int y, int w, uint16_t color);
void gfx_vline(gfx_t *g, int x, int y, int h, uint16_t color);
/* Rounded rectangle — the scrubber pill and volume overlay want this. */
void gfx_fill_round_rect(gfx_t *g, int x, int y, int w, int h, int r,
                         uint16_t color);

/* Text. These wrap the font module with a colour and this framebuffer. */
int  gfx_text(gfx_t *g, const font_t *f, const char *s, int x, int y,
              uint16_t color);
bool gfx_text_ellipsis(gfx_t *g, const font_t *f, const char *s, int x, int y,
                       int max_w, uint16_t color);
int  gfx_text_centered(gfx_t *g, const font_t *f, const char *s, int x, int y,
                       int w, uint16_t color);
int  gfx_text_right(gfx_t *g, const font_t *f, const char *s, int right_x,
                    int y, uint16_t color);

/* Read a pixel — for tests and for the PNG writer. */
uint16_t gfx_get(const gfx_t *g, int x, int y);

#endif /* GFX_H */
