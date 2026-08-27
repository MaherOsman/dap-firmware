/*
 * theme — colour and geometry tokens, ported from the SDL simulator.
 *
 * These are the same three themes designed in the simulator (Dark / Warm /
 * iPod), with two changes for hardware:
 *
 *   1. Colours are RGB565, not 0x00RRGGBB, because that is what the ST7789
 *      takes. The source 8-bit values are kept in comments so the themes can
 *      still be compared against the simulator.
 *   2. Layout constants are retuned for a 240x240 panel. The simulator ran at
 *      320x240 — 80 pixels wider than the screen actually ordered.
 *
 * Screens read every colour through a `const theme_t *`. Nothing hardcodes a
 * colour, which is what makes theme switching a single pointer swap.
 */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

typedef uint16_t color_t;

typedef struct {
    const char *name;

    /* Surfaces */
    color_t bg;
    color_t surface;
    color_t surface_alt;

    /* Text */
    color_t text_primary;
    color_t text_secondary;
    color_t text_inactive;

    /* Accent */
    color_t accent;
    color_t accent_dim;

    /* Icons */
    color_t icon_active;
    color_t icon_inactive;

    /* Album art placeholder */
    color_t art_placeholder_bg;
    color_t art_placeholder_fg;

    /* Scrubber geometry */
    uint8_t bar_height;
    uint8_t pill_width;
    uint8_t pill_height;
    uint8_t pill_radius;

    /* Layout */
    uint8_t topbar_height;
    uint8_t controls_height;
} theme_t;

extern const theme_t THEME_DARK;
extern const theme_t THEME_WARM;
extern const theme_t THEME_IPOD;

extern const theme_t *const ALL_THEMES[];
extern const int THEME_COUNT;

/* Panel geometry — the real one, not the simulator's. */
#define SCREEN_W 240
#define SCREEN_H 240

/* Library list metrics. Derived in ONE place: the simulator computed the
 * visible row count in two files from duplicated constants, which is a
 * silent desync waiting to happen. */
#define LIB_ROW_H     18
#define LIB_HEADER_H  24
#define LIB_HINT_H    15
#define LIB_LIST_TOP  LIB_HEADER_H
#define LIB_LIST_BOT  (SCREEN_H - LIB_HINT_H)
#define LIB_VISIBLE   ((LIB_LIST_BOT - LIB_LIST_TOP) / LIB_ROW_H)

#endif /* THEME_H */
