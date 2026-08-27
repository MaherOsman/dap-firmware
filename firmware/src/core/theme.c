#include "theme.h"

/* Ported from the SDL simulator. Source RGB888 values kept in comments so
 * the two can still be compared side by side. */

const theme_t THEME_DARK = {
    .name               = "Dark",
    .bg                 = 0x0862,  /* #0E0E10 */
    .surface            = 0x18C3,  /* #1A1A1F */
    .surface_alt        = 0x2125,  /* #26262E */
    .text_primary       = 0xF79E,  /* #F0F0F5 */
    .text_secondary     = 0x9CD5,  /* #9A9AAA */
    .text_inactive      = 0x52AC,  /* #555566 */
    .accent             = 0x06BF,  /* #00D4FF */
    .accent_dim         = 0x01C8,  /* #003A44 */
    .icon_active        = 0xF79E,  /* #F0F0F5 */
    .icon_inactive      = 0x52AC,  /* #555566 */
    .art_placeholder_bg = 0x18C3,  /* #1A1A1F */
    .art_placeholder_fg = 0x3188,  /* #333344 */
    .bar_height         = 2,
    .pill_width         = 3,
    .pill_height        = 14,
    .pill_radius        = 1,
    .topbar_height      = 24,
    .controls_height    = 32,
};

const theme_t THEME_WARM = {
    .name               = "Warm",
    .bg                 = 0x1061,  /* #140F0A */
    .surface            = 0x18A2,  /* #1F1710 */
    .surface_alt        = 0x2902,  /* #2C2015 */
    .text_primary       = 0xF77B,  /* #F5ECD8 */
    .text_secondary     = 0xAC8D,  /* #AA906A */
    .text_inactive      = 0x5A26,  /* #5A4430 */
    .accent             = 0xFD46,  /* #FFA830 */
    .accent_dim         = 0x3921,  /* #3D270A */
    .icon_active        = 0xF77B,  /* #F5ECD8 */
    .icon_inactive      = 0x5A26,  /* #5A4430 */
    .art_placeholder_bg = 0x18A2,  /* #1F1710 */
    .art_placeholder_fg = 0x3123,  /* #35271A */
    .bar_height         = 2,
    .pill_width         = 3,
    .pill_height        = 14,
    .pill_radius        = 1,
    .topbar_height      = 24,
    .controls_height    = 32,
};

const theme_t THEME_IPOD = {
    .name               = "iPod",
    .bg                 = 0xFFFF,  /* #FFFFFF */
    .surface            = 0xF79E,  /* #F2F2F2 */
    .surface_alt        = 0xE71C,  /* #E0E0E0 */
    .text_primary       = 0x18C3,  /* #1A1A1A */
    .text_secondary     = 0x6B6D,  /* #6E6E6E */
    .text_inactive      = 0xBDD7,  /* #BBBBBB */
    .accent             = 0xF96A,  /* #FF2D55 */
    .accent_dim         = 0xFE7A,  /* #FFCCD5 */
    .icon_active        = 0x18C3,  /* #1A1A1A */
    .icon_inactive      = 0xBDD7,  /* #BBBBBB */
    .art_placeholder_bg = 0xF79E,  /* #F2F2F2 */
    .art_placeholder_fg = 0xCE79,  /* #CCCCCC */
    .bar_height         = 2,
    .pill_width         = 3,
    .pill_height        = 14,
    .pill_radius        = 1,
    .topbar_height      = 24,
    .controls_height    = 32,
};

const theme_t *const ALL_THEMES[] = {
    &THEME_DARK,
    &THEME_WARM,
    &THEME_IPOD,
};

const int THEME_COUNT = 3;
