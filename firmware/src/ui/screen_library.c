#include "screen_library.h"

#define MARGIN 10

/* Per-level indent, so drilling down reads as going deeper. */
static const int INDENT[3] = {0, 8, 16};

void lib_clamp_scroll(int selected, int row_count, int *scroll_top)
{
    int top = *scroll_top;

    if (selected < top) {
        top = selected;
    }
    if (selected >= top + LIB_VISIBLE) {
        top = selected - LIB_VISIBLE + 1;
    }

    /* Don't leave blank rows at the bottom when the list is longer than the
     * window — the simulator's version could, because it only ever clamped
     * against the selection. */
    if (row_count > LIB_VISIBLE) {
        int max_top = row_count - LIB_VISIBLE;
        if (top > max_top) {
            top = max_top;
        }
    } else {
        top = 0;
    }
    if (top < 0) {
        top = 0;
    }
    *scroll_top = top;
}

/* A small right-pointing triangle: the "now playing" marker. Drawn rather
 * than baked into the font so it scales with the row height and stays crisp. */
static void draw_play_marker(gfx_t *g, int x, int y, int size, color_t c)
{
    for (int i = 0; i < size; i++) {
        /* Height shrinks by 2 each column, centred vertically. */
        int h = size * 2 - 1 - i * 2;
        if (h <= 0) {
            break;
        }
        gfx_vline(g, x + i, y + i, h, c);
    }
}

/* A right-pointing chevron for rows that drill down further. */
static void draw_chevron(gfx_t *g, int x, int y, int size, color_t c)
{
    for (int i = 0; i < size; i++) {
        gfx_pixel(g, x + i, y + i, c);
        gfx_pixel(g, x + i, y + (size - 1) * 2 - i, c);
        /* Two pixels wide so it doesn't disappear against the background. */
        gfx_pixel(g, x + i + 1, y + i, c);
        gfx_pixel(g, x + i + 1, y + (size - 1) * 2 - i, c);
    }
}

void screen_library_draw(gfx_t *g, const theme_t *t, const lib_row_t *rows,
                         int row_count, int selected, int scroll_top,
                         const char *header, lib_level_t level)
{
    gfx_clip_reset(g);
    gfx_clear(g, t->bg);

    /* ---- header ---- */
    const int HDR_H = t->topbar_height;
    gfx_hline(g, 0, HDR_H - 1, SCREEN_W, t->surface_alt);

    int hdr_y = (HDR_H - font_sm.height) / 2;
    gfx_text_centered(g, &font_sm, header ? header : "", 0, hdr_y, SCREEN_W,
                      t->text_secondary);

    /* Back-affordance when we're not at the root level. */
    if (level > LIB_LEVEL_ARTIST) {
        int cy = HDR_H / 2 - 3;
        for (int i = 0; i < 4; i++) {
            gfx_pixel(g, 6 + 3 - i, cy + i, t->text_inactive);
            gfx_pixel(g, 6 + 3 - i, cy + 6 - i, t->text_inactive);
        }
    }

    /* ---- list ---- */
    const int text_indent = INDENT[level];
    const int chevron_w   = 6;

    gfx_clip(g, 0, LIB_LIST_TOP, SCREEN_W, LIB_LIST_BOT - LIB_LIST_TOP);

    for (int i = scroll_top; i < row_count; i++) {
        int row_y = LIB_LIST_TOP + (i - scroll_top) * LIB_ROW_H;
        if (row_y + LIB_ROW_H > LIB_LIST_BOT) {
            break;
        }

        bool is_sel     = (i == selected);
        bool is_current = rows[i].is_current;

        if (is_sel) {
            gfx_fill_rect(g, 0, row_y, SCREEN_W, LIB_ROW_H, t->accent_dim);
        }

        int text_x  = MARGIN + text_indent;
        int text_y  = row_y + (LIB_ROW_H - font_sm.height) / 2;

        if (rows[i].has_sub) {
            color_t cc = is_sel ? t->text_primary : t->text_inactive;
            draw_chevron(g, SCREEN_W - chevron_w - MARGIN,
                         row_y + (LIB_ROW_H - 7) / 2, 4, cc);
        }

        if (!rows[i].has_sub && is_current) {
            draw_play_marker(g, text_x, row_y + (LIB_ROW_H - 9) / 2, 5,
                             t->accent);
            text_x += 12;
        }

        int avail_w = SCREEN_W - text_x - MARGIN
                      - (rows[i].has_sub ? chevron_w + MARGIN : 0);

        color_t text_col = is_sel     ? t->text_primary
                           : is_current ? t->accent
                                        : t->text_secondary;

        gfx_text_ellipsis(g, &font_sm, rows[i].text ? rows[i].text : "",
                          text_x, text_y, avail_w, text_col);
    }

    gfx_clip_reset(g);

    /* ---- scrollbar ---- */
    if (row_count > LIB_VISIBLE) {
        int track_h = LIB_LIST_BOT - LIB_LIST_TOP;
        int bar_h   = track_h * LIB_VISIBLE / row_count;
        int bar_y   = LIB_LIST_TOP + track_h * scroll_top / row_count;
        if (bar_h < 8) {
            bar_h = 8; /* stays visible on a very long library */
        }
        gfx_fill_rect(g, SCREEN_W - 3, bar_y, 2, bar_h, t->surface_alt);
    }

    /* ---- hint bar ---- */
    gfx_hline(g, 0, LIB_LIST_BOT, SCREEN_W, t->surface_alt);

    /* Hints describe the real controls — one encoder with a button — not the
     * simulator's keyboard. */
    const char *hint = (level == LIB_LEVEL_TRACK)
                           ? "turn: scroll   press: play   hold: back"
                           : "turn: scroll   press: open   hold: back";
    int hint_y = LIB_LIST_BOT + (LIB_HINT_H - font_sm.height) / 2 + 1;
    gfx_text_centered(g, &font_sm, hint, 0, hint_y, SCREEN_W,
                      t->text_inactive);
}
