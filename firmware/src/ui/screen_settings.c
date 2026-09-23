/* screen_settings.c — the settings list and its drawing. */

#include "screen_settings.h"

#include <string.h>

#include "screen_library.h"   /* lib_clamp_scroll, LIB_* metrics */

#define MARGIN 10

static const char *const THEME_VALUES[] = { "Dark", "Warm", "iPod", "Midnight" };
static const char *const REPEAT_VALUES[] = { "Off", "Once", "All" };
static const char *const LAYOUT_VALUES[] = { "Standard", "Art only" };

#define N_THEME_VALUES  ((uint8_t)(sizeof(THEME_VALUES) / sizeof(THEME_VALUES[0])))
#define N_REPEAT_VALUES ((uint8_t)(sizeof(REPEAT_VALUES) / sizeof(REPEAT_VALUES[0])))
#define N_LAYOUT_VALUES ((uint8_t)(sizeof(LAYOUT_VALUES) / sizeof(LAYOUT_VALUES[0])))

/* The names above must cover every theme the build actually has, or a theme
 * would be selectable with no label. */
#if 0
/* THEME_COUNT is a runtime extern, so this is checked in settings_init
 * rather than here. */
#endif

void settings_init(settings_t *s, uint8_t theme, uint8_t repeat,
                   uint8_t np_layout)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));

    s->items[0].id = SET_ID_THEME;
    s->items[0].label = "Theme";
    s->items[0].values = THEME_VALUES;
    s->items[0].value_count = N_THEME_VALUES;
    s->items[0].value = (theme < N_THEME_VALUES) ? theme : 0u;

    s->items[1].id = SET_ID_REPEAT;
    s->items[1].label = "Repeat";
    s->items[1].values = REPEAT_VALUES;
    s->items[1].value_count = N_REPEAT_VALUES;
    s->items[1].value = (repeat < N_REPEAT_VALUES) ? repeat : 0u;

    s->items[2].id = SET_ID_NP_LAYOUT;
    s->items[2].label = "Now Playing";
    s->items[2].values = LAYOUT_VALUES;
    s->items[2].value_count = N_LAYOUT_VALUES;
    s->items[2].value = (np_layout < N_LAYOUT_VALUES) ? np_layout : 0u;

    s->items[3].id = SET_ID_RESCAN;
    s->items[3].label = "Rescan card";
    s->items[3].values = NULL;      /* action row */
    s->items[3].value_count = 0u;
    s->items[3].value = 0u;

    s->count = 4u;
    s->sel = 0;
    s->top = 0;
}

int settings_count(const settings_t *s)
{
    return (s != NULL) ? (int)s->count : 0;
}

int settings_selected(const settings_t *s)
{
    return (s != NULL) ? s->sel : 0;
}

int settings_scroll_top(const settings_t *s)
{
    return (s != NULL) ? s->top : 0;
}

void settings_move(settings_t *s, int delta)
{
    int n;

    if (s == NULL) return;
    n = (int)s->count;
    if (n <= 0) { s->sel = 0; s->top = 0; return; }

    s->sel += delta;
    if (s->sel < 0)  s->sel = 0;
    if (s->sel >= n) s->sel = n - 1;

    lib_clamp_scroll(s->sel, n, &s->top);
}

set_id_t settings_activate(settings_t *s)
{
    set_item_t *it;

    if (s == NULL || s->count == 0u) return SET_ID_COUNT;
    if (s->sel < 0 || s->sel >= (int)s->count) return SET_ID_COUNT;

    it = &s->items[s->sel];

    /* A value row cycles in place; an action row just reports itself and
     * lets the caller decide what it means. */
    if (it->values != NULL && it->value_count > 0u) {
        it->value = (uint8_t)((it->value + 1u) % it->value_count);
    }
    return it->id;
}

static set_item_t *find(settings_t *s, set_id_t id)
{
    uint8_t i;
    for (i = 0; i < s->count; i++) {
        if (s->items[i].id == id) return &s->items[i];
    }
    return NULL;
}

uint8_t settings_value(const settings_t *s, set_id_t id)
{
    uint8_t i;
    if (s == NULL) return 0u;
    for (i = 0; i < s->count; i++) {
        if (s->items[i].id == id) return s->items[i].value;
    }
    return 0u;
}

void settings_set_value(settings_t *s, set_id_t id, uint8_t value)
{
    set_item_t *it;
    if (s == NULL) return;
    it = find(s, id);
    if (it == NULL || it->value_count == 0u) return;
    it->value = (value < it->value_count) ? value : 0u;
}

/* ------------------------------------------------------------------ */

static void draw_chevron(gfx_t *g, int x, int y, int size, uint16_t c)
{
    int i;
    for (i = 0; i < size; i++) {
        gfx_pixel(g, x + i, y + i, c);
        gfx_pixel(g, x + i, y + (size - 1) * 2 - i, c);
        gfx_pixel(g, x + i + 1, y + i, c);
        gfx_pixel(g, x + i + 1, y + (size - 1) * 2 - i, c);
    }
}

void screen_settings_draw(gfx_t *g, const theme_t *t, const settings_t *s)
{
    int hdr_h, hdr_y, i;

    if (g == NULL || t == NULL || s == NULL) return;

    gfx_clip_reset(g);
    gfx_clear(g, t->bg);

    hdr_h = (int)t->topbar_height;
    gfx_hline(g, 0, hdr_h - 1, SCREEN_W, t->surface_alt);
    hdr_y = (hdr_h - font_sm.height) / 2;
    (void)gfx_text_centered(g, &font_sm, "Settings", 0, hdr_y, SCREEN_W,
                            t->text_secondary);

    gfx_clip(g, 0, LIB_LIST_TOP, SCREEN_W, LIB_LIST_BOT - LIB_LIST_TOP);

    for (i = s->top; i < (int)s->count; i++) {
        const set_item_t *it = &s->items[i];
        int row_y = LIB_LIST_TOP + (i - s->top) * LIB_ROW_H;
        bool is_sel = (i == s->sel);
        int text_y;
        uint16_t label_col;

        if (row_y + LIB_ROW_H > LIB_LIST_BOT) break;

        if (is_sel) {
            gfx_fill_rect(g, 0, row_y, SCREEN_W, LIB_ROW_H, t->accent_dim);
        }

        text_y = row_y + (LIB_ROW_H - font_sm.height) / 2;
        label_col = is_sel ? t->text_primary : t->text_secondary;

        (void)gfx_text_ellipsis(g, &font_sm, it->label, MARGIN, text_y,
                                SCREEN_W / 2, label_col);

        if (it->values != NULL && it->value < it->value_count) {
            /* The value carries the accent: it is the part that changes,
             * so it should be the part the eye lands on. */
            (void)gfx_text_right(g, &font_sm, it->values[it->value],
                                 SCREEN_W - MARGIN, text_y,
                                 is_sel ? t->accent : t->text_inactive);
        } else {
            draw_chevron(g, SCREEN_W - MARGIN - 6,
                         row_y + (LIB_ROW_H - 7) / 2, 4,
                         is_sel ? t->text_primary : t->text_inactive);
        }
    }

    gfx_clip_reset(g);

    gfx_hline(g, 0, LIB_LIST_BOT, SCREEN_W, t->surface_alt);
    {
        /* No save hint: changes are saved as they are made. Saying "hold:
         * save" would imply the opposite. */
        int hint_y = LIB_LIST_BOT + (LIB_HINT_H - font_sm.height) / 2 + 1;
        (void)gfx_text_centered(g, &font_sm,
                                "turn: select   press: change   hold: back",
                                0, hint_y, SCREEN_W, t->text_inactive);
    }
}
