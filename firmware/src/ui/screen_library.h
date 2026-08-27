/*
 * screen_library — the artist/album/track browser, drawn to a framebuffer.
 *
 * Ported from the simulator's SDL version. Same layout, same chevrons, same
 * scrollbar, but retuned for 240 pixels wide and drawing through gfx instead
 * of SDL_Renderer — which means it runs identically on the STM32 and on a PC
 * where we render it to a PNG to look at.
 *
 * This module draws. It does not own selection or scroll state; the caller
 * passes those in, exactly as the simulator did.
 */
#ifndef SCREEN_LIBRARY_H
#define SCREEN_LIBRARY_H

#include "../core/gfx.h"
#include "../core/theme.h"

typedef enum {
    LIB_LEVEL_ARTIST = 0,
    LIB_LEVEL_ALBUM  = 1,
    LIB_LEVEL_TRACK  = 2
} lib_level_t;

typedef struct {
    const char *text;
    bool        has_sub;     /* artist/album — draws a › chevron */
    bool        is_current;  /* contains or is the playing track */
} lib_row_t;

void screen_library_draw(gfx_t *g, const theme_t *t, const lib_row_t *rows,
                         int row_count, int selected, int scroll_top,
                         const char *header, lib_level_t level);

/*
 * Keep `selected` visible by adjusting `*scroll_top`. Single definition —
 * the simulator computed the visible row count in two places from duplicated
 * constants, so a change to either could silently desync the scroll from
 * what was drawn.
 */
void lib_clamp_scroll(int selected, int row_count, int *scroll_top);

#endif /* SCREEN_LIBRARY_H */
