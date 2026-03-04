#include "theme.h"

/* ═══════════════════════════════════════════════════
   THEME: DARK
   Dark charcoal background, cool-white text,
   electric cyan accent — modern audiophile DAP feel.
═══════════════════════════════════════════════════ */

const Theme THEME_DARK = {
    .name = "Dark",

    /* Surfaces */
    .bg             = RGB(0x0E, 0x0E, 0x10),   /* near-black, warm hint    */
    .surface        = RGB(0x1A, 0x1A, 0x1F),   /* elevated card            */
    .surface_alt    = RGB(0x26, 0x26, 0x2E),   /* divider / inactive zone  */

    /* Text */
    .text_primary   = RGB(0xF0, 0xF0, 0xF5),   /* bright white             */
    .text_secondary = RGB(0x9A, 0x9A, 0xAA),   /* muted grey-blue          */
    .text_inactive  = RGB(0x55, 0x55, 0x66),   /* very dim                 */

    /* Accent – electric cyan */
    .accent         = RGB(0x00, 0xD4, 0xFF),
    .accent_dim     = RGB(0x00, 0x3A, 0x44),   /* dim track fill           */

    /* Icons */
    .icon_active    = RGB(0xF0, 0xF0, 0xF5),
    .icon_inactive  = RGB(0x55, 0x55, 0x66),

    /* Art placeholder */
    .art_placeholder_bg = RGB(0x1A, 0x1A, 0x1F),
    .art_placeholder_fg = RGB(0x33, 0x33, 0x44),

    /* Scrubber */
    .scrubber_style = SCRUBBER_PILL,
    .bar_height     = 2,
    .pill_width     = 3,
    .pill_height    = 14,
    .pill_radius    = 1,

    /* Layout */
    .topbar_height    = 24,
    .controls_height  = 32,
};
