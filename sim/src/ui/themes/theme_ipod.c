#include "theme.h"

/* ═══════════════════════════════════════════════════
   THEME: IPOD
   Pure white background, graphite text,
   classic red accent — clean Apple-era nostalgia.
═══════════════════════════════════════════════════ */

const Theme THEME_IPOD = {
    .name = "iPod",

    /* Surfaces */
    .bg             = RGB(0xFF, 0xFF, 0xFF),   /* pure white               */
    .surface        = RGB(0xF2, 0xF2, 0xF2),   /* light grey card          */
    .surface_alt    = RGB(0xE0, 0xE0, 0xE0),   /* divider line             */

    /* Text */
    .text_primary   = RGB(0x1A, 0x1A, 0x1A),   /* near-black graphite      */
    .text_secondary = RGB(0x6E, 0x6E, 0x6E),   /* mid grey                 */
    .text_inactive  = RGB(0xBB, 0xBB, 0xBB),   /* light inactive           */

    /* Accent – classic iPod red */
    .accent         = RGB(0xFF, 0x2D, 0x55),
    .accent_dim     = RGB(0xFF, 0xCC, 0xD5),   /* dim track fill           */

    /* Icons */
    .icon_active    = RGB(0x1A, 0x1A, 0x1A),
    .icon_inactive  = RGB(0xBB, 0xBB, 0xBB),

    /* Art placeholder */
    .art_placeholder_bg = RGB(0xF2, 0xF2, 0xF2),
    .art_placeholder_fg = RGB(0xCC, 0xCC, 0xCC),

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
