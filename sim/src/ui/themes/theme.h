#ifndef THEME_H
#define THEME_H

#include <stdint.h>

/* ─────────────────────────────────────────────
   Colour helper: pack R,G,B into a single uint32
   We store as 0x00RRGGBB for SDL_MapRGB usage
───────────────────────────────────────────── */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

typedef uint32_t Color;

/* ─────────────────────────────────────────────
   Font size tokens  (mapped to loaded TTF sizes
   in the renderer – adjust to your Roboto sizes)
───────────────────────────────────────────── */
typedef enum {
    FONT_SM  = 0,   /* 11 px – timestamps, secondary labels */
    FONT_MD  = 1,   /* 14 px – artist / subtitle            */
    FONT_LG  = 2,   /* 18 px – track title                  */
    FONT_XL  = 3,   /* 24 px – not used on NowPlaying yet   */
    FONT_COUNT
} FontSize;

/* ─────────────────────────────────────────────
   Progress-bar scrubber style
   PILL: thin horizontal line + tall thin pill
───────────────────────────────────────────── */
typedef enum {
    SCRUBBER_PILL   /* thin line + vertical pill knob */
} ScrubberStyle;

/* ─────────────────────────────────────────────
   The Theme struct – every screen reads from
   a pointer to one of these; never hardcodes.
───────────────────────────────────────────── */
typedef struct {
    const char *name;

    /* ── Surface colours ── */
    Color bg;               /* main background                  */
    Color surface;          /* card / elevated surface          */
    Color surface_alt;      /* subtle divider / inactive area   */

    /* ── Text colours ── */
    Color text_primary;     /* track title, main labels         */
    Color text_secondary;   /* artist, timestamps               */
    Color text_inactive;    /* scrub time, disabled items       */

    /* ── Accent ── */
    Color accent;           /* scrubber pill, active icon       */
    Color accent_dim;       /* unfilled progress track          */

    /* ── Icon / control tint ── */
    Color icon_active;
    Color icon_inactive;

    /* ── Album-art placeholder (when no art loaded) ── */
    Color art_placeholder_bg;
    Color art_placeholder_fg;

    /* ── Progress bar geometry ── */
    ScrubberStyle scrubber_style;
    int  bar_height;        /* px – thickness of the track line */
    int  pill_width;        /* px – scrubber pill width         */
    int  pill_height;       /* px – scrubber pill height        */
    int  pill_radius;       /* px – corner radius of pill       */

    /* ── Layout ── */
    int  topbar_height;     /* px                               */
    int  controls_height;   /* px                               */

} Theme;

/* ── Theme registry ── */
extern const Theme THEME_DARK;
extern const Theme THEME_WARM;
extern const Theme THEME_IPOD;

/* Convenience array for cycling */
extern const Theme *ALL_THEMES[];
extern const int    THEME_COUNT;

#endif /* THEME_H */
