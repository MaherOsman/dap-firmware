#include "theme.h"

/* ═══════════════════════════════════════════════════
   THEME: WARM
   Deep espresso background, cream text,
   amber/gold accent — vintage hifi / vinyl feel.
═══════════════════════════════════════════════════ */

const Theme THEME_WARM = {
    .name = "Warm",

    /* Surfaces */
    .bg             = RGB(0x14, 0x0F, 0x0A),   /* deep espresso            */
    .surface        = RGB(0x1F, 0x17, 0x10),   /* dark amber card          */
    .surface_alt    = RGB(0x2C, 0x20, 0x15),   /* warm divider             */

    /* Text */
    .text_primary   = RGB(0xF5, 0xEC, 0xD8),   /* warm cream               */
    .text_secondary = RGB(0xAA, 0x90, 0x6A),   /* muted tan                */
    .text_inactive  = RGB(0x5A, 0x44, 0x30),   /* dim brown                */

    /* Accent – amber gold */
    .accent         = RGB(0xFF, 0xA8, 0x30),
    .accent_dim     = RGB(0x3D, 0x27, 0x0A),   /* dim track fill           */

    /* Icons */
    .icon_active    = RGB(0xF5, 0xEC, 0xD8),
    .icon_inactive  = RGB(0x5A, 0x44, 0x30),

    /* Art placeholder */
    .art_placeholder_bg = RGB(0x1F, 0x17, 0x10),
    .art_placeholder_fg = RGB(0x35, 0x27, 0x1A),

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
