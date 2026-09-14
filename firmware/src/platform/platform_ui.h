/*
 * platform_ui — which screen is showing, and what the encoder does on it.
 *
 * One encoder, three inputs, three screens. Rather than growing main's loop
 * a branch per combination, the dispatch lives here:
 *
 *   Library      turn scrolls · click descends or plays · hold goes back
 *   Now playing  turn moves the focus ring (or volume, in volume mode)
 *                click activates the focused control · hold returns to the
 *                library
 *   Info         hold returns to now playing
 *
 * This is also where end-of-track auto-advance happens: the playqueue says
 * what should play next, this asks the platform to play it. Keeping that in
 * one place is what stops "what plays next" from being spread across the
 * loop, the screen, and the queue.
 *
 * main.c is left with four calls.
 */
#ifndef PLATFORM_UI_H
#define PLATFORM_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "st7789.h"
#include "library_index.h"

typedef enum {
    UI_LIBRARY = 0,
    UI_NOW_PLAYING,
    UI_INFO
} ui_screen_t;

/* `idx` may be NULL — the library screen shows empty and nothing crashes. */
void dap_ui_init(gfx_t *fb, st7789_t *tft, libidx_t *idx);

/* Encoder input. `delta` is detents (signed), `btn` is 0 none / 1 click /
 * 2 long-press — the same codes main's ISR already produces. */
void dap_ui_input(int delta, int btn);

/* Call every pass of the main loop. Handles end-of-track advance and
 * repaints when something changed (or when the scrubber has moved). */
void dap_ui_tick(void);

ui_screen_t dap_ui_screen(void);

#endif /* PLATFORM_UI_H */
