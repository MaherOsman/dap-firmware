#include "theme.h"

/* ─────────────────────────────────────────────
   Central registry – add new themes here only.
   ALL_THEMES is iterated for skin cycling.
───────────────────────────────────────────── */

const Theme *ALL_THEMES[] = {
    &THEME_DARK,
    &THEME_WARM,
    &THEME_IPOD,
};

const int THEME_COUNT = 3;
