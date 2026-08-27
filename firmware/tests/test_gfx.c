#include "test.h"
#include "../src/core/gfx.h"
#include "../src/core/theme.h"
#include "../src/ui/screen_library.h"

static uint16_t fb[240 * 240];
static gfx_t g;

static void setup(void) { gfx_init(&g, fb, 240, 240); gfx_clear(&g, 0); }

TEST(rgb565_packing_and_888_conversion)
{
    CHECK_EQ(gfx_rgb(255, 0, 0), 0xF800);
    CHECK_EQ(gfx_rgb(0, 255, 0), 0x07E0);
    CHECK_EQ(gfx_rgb(0, 0, 255), 0x001F);
    CHECK_EQ(gfx_rgb888(0xFFFFFF), 0xFFFF);
    CHECK_EQ(gfx_rgb888(0x000000), 0x0000);
    /* The iPod theme's red must survive the trip from the simulator. */
    CHECK_EQ(gfx_rgb888(0xFF2D55), gfx_rgb(0xFF, 0x2D, 0x55));
}

TEST(fill_and_read_back)
{
    setup();
    gfx_fill_rect(&g, 10, 10, 5, 5, 0xF800);
    CHECK_EQ(gfx_get(&g, 10, 10), 0xF800);
    CHECK_EQ(gfx_get(&g, 14, 14), 0xF800);
    CHECK_EQ(gfx_get(&g, 15, 15), 0x0000); /* exclusive upper bound */
    CHECK_EQ(gfx_get(&g, 9, 9), 0x0000);
}

TEST(drawing_off_screen_is_safe)
{
    setup();
    /* None of these may write out of bounds — ASan proves it. */
    gfx_fill_rect(&g, -50, -50, 20, 20, 0xFFFF);
    gfx_fill_rect(&g, 235, 235, 100, 100, 0xFFFF);
    gfx_fill_rect(&g, 1000, 1000, 10, 10, 0xFFFF);
    gfx_pixel(&g, -1, 0, 0xFFFF);
    gfx_pixel(&g, 0, -1, 0xFFFF);
    gfx_pixel(&g, 240, 0, 0xFFFF);
    gfx_pixel(&g, 0, 240, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 0, 0), 0x0000);
    CHECK_EQ(gfx_get(&g, 239, 239), 0xFFFF); /* the 235,235 rect did land */
}

TEST(clipping_confines_drawing)
{
    setup();
    gfx_clip(&g, 100, 100, 20, 20);
    gfx_fill_rect(&g, 0, 0, 240, 240, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 99, 100), 0x0000);
    CHECK_EQ(gfx_get(&g, 100, 100), 0xFFFF);
    CHECK_EQ(gfx_get(&g, 119, 119), 0xFFFF);
    CHECK_EQ(gfx_get(&g, 120, 120), 0x0000);
    gfx_clip_reset(&g);
    gfx_fill_rect(&g, 0, 0, 5, 5, 0x07E0);
    CHECK_EQ(gfx_get(&g, 0, 0), 0x07E0);
}

TEST(clear_ignores_the_clip_rect)
{
    setup();
    gfx_clip(&g, 50, 50, 10, 10);
    gfx_clear(&g, 0x1234);
    CHECK_EQ(gfx_get(&g, 0, 0), 0x1234);
    CHECK_EQ(gfx_get(&g, 239, 239), 0x1234);
    /* And the clip must be restored afterwards. */
    gfx_fill_rect(&g, 0, 0, 240, 240, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 0, 0), 0x1234);
    CHECK_EQ(gfx_get(&g, 55, 55), 0xFFFF);
}

TEST(rect_outline_is_hollow)
{
    setup();
    gfx_rect(&g, 10, 10, 10, 10, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 10, 10), 0xFFFF);
    CHECK_EQ(gfx_get(&g, 19, 19), 0xFFFF);
    CHECK_EQ(gfx_get(&g, 15, 15), 0x0000); /* middle stays empty */
}

TEST(round_rect_has_cut_corners_and_full_middle)
{
    setup();
    gfx_fill_round_rect(&g, 10, 10, 40, 20, 6, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 10, 10), 0x0000);  /* corner cut away */
    CHECK_EQ(gfx_get(&g, 30, 10), 0xFFFF);  /* top edge, middle */
    CHECK_EQ(gfx_get(&g, 10, 20), 0xFFFF);  /* left edge, middle */
    CHECK_EQ(gfx_get(&g, 30, 20), 0xFFFF);  /* centre */
    CHECK_EQ(gfx_get(&g, 49, 29), 0x0000);  /* opposite corner cut */
}

TEST(round_rect_clamps_an_oversized_radius)
{
    setup();
    /* The scrubber pill is 3x14 with radius 1; a bad theme could ask for 50. */
    gfx_fill_round_rect(&g, 10, 10, 3, 14, 50, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 11, 16), 0xFFFF); /* still draws something sane */
}

TEST(text_lands_in_the_framebuffer)
{
    setup();
    int end = gfx_text(&g, &font_md, "Debussy", 5, 5, 0xFFFF);
    CHECK(end > 5);
    int lit = 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 100; x++)
            if (gfx_get(&g, x, y)) lit++;
    CHECK(lit > 20);
}

TEST(text_respects_the_clip_rect)
{
    setup();
    gfx_clip(&g, 0, 0, 20, 240);
    gfx_text(&g, &font_md, "This is much wider than twenty pixels", 0, 5,
             0xFFFF);
    gfx_clip_reset(&g);
    for (int y = 0; y < 30; y++) {
        for (int x = 20; x < 240; x++) {
            if (gfx_get(&g, x, y)) {
                CHECK_EQ(x, -1); /* report the offending column */
                return;
            }
        }
    }
    CHECK(true);
}

/* ---- the library screen, drawn for real ---- */

TEST(library_screen_draws_without_escaping_the_buffer)
{
    setup();
    lib_row_t rows[40];
    for (int i = 0; i < 40; i++) {
        rows[i].text = "Some Artist With A Fairly Long Name Indeed";
        rows[i].has_sub = true;
        rows[i].is_current = (i == 3);
    }
    screen_library_draw(&g, &THEME_DARK, rows, 40, 20, 15, "Library",
                        LIB_LEVEL_ARTIST);
    /* Header line and hint line must both be present. */
    CHECK_EQ(gfx_get(&g, 0, THEME_DARK.topbar_height - 1),
             THEME_DARK.surface_alt);
    CHECK_EQ(gfx_get(&g, 0, LIB_LIST_BOT), THEME_DARK.surface_alt);
}

TEST(library_visible_rows_is_consistent)
{
    /* The simulator computed this in two places from duplicated constants.
     * Here there is one definition, and this pins its value. */
    CHECK_EQ(LIB_VISIBLE, (240 - 24 - 15) / 18);
    CHECK_EQ(LIB_VISIBLE, 11);
}

TEST(scroll_clamp_keeps_selection_visible)
{
    int top = 0;
    lib_clamp_scroll(0, 40, &top);
    CHECK_EQ(top, 0);

    lib_clamp_scroll(10, 40, &top);
    CHECK_EQ(top, 0); /* row 10 is the last visible with 11 rows */

    lib_clamp_scroll(11, 40, &top);
    CHECK_EQ(top, 1); /* scrolled by exactly one */

    lib_clamp_scroll(39, 40, &top);
    CHECK_EQ(top, 29); /* 40 - 11 */

    lib_clamp_scroll(0, 40, &top);
    CHECK_EQ(top, 0);
}

TEST(scroll_clamp_never_leaves_blank_rows)
{
    /* This is the bug the simulator's version could hit: a stale scroll_top
     * left over from a longer list. */
    int top = 30;
    lib_clamp_scroll(2, 40, &top);
    CHECK_EQ(top, 2);

    top = 35;
    lib_clamp_scroll(39, 40, &top);
    CHECK(top <= 29);

    /* Short list: never scrolls at all. */
    top = 5;
    lib_clamp_scroll(1, 4, &top);
    CHECK_EQ(top, 0);
}

TEST(selected_row_is_highlighted)
{
    setup();
    lib_row_t rows[5];
    for (int i = 0; i < 5; i++) {
        rows[i].text = "Track";
        rows[i].has_sub = false;
        rows[i].is_current = false;
    }
    screen_library_draw(&g, &THEME_IPOD, rows, 5, 2, 0, "Album",
                        LIB_LEVEL_TRACK);
    int row_y = LIB_LIST_TOP + 2 * LIB_ROW_H;
    /* Highlight band spans the full width at the selected row. */
    CHECK_EQ(gfx_get(&g, 200, row_y + 1), THEME_IPOD.accent_dim);
    /* And the row above it is not highlighted. */
    CHECK_EQ(gfx_get(&g, 200, LIB_LIST_TOP + 1), THEME_IPOD.bg);
}

TEST(scrollbar_appears_only_when_needed)
{
    lib_row_t rows[5];
    for (int i = 0; i < 5; i++) {
        rows[i].text = "x"; rows[i].has_sub = false; rows[i].is_current = false;
    }
    setup();
    screen_library_draw(&g, &THEME_DARK, rows, 5, 0, 0, "H", LIB_LEVEL_TRACK);
    bool found = false;
    for (int y = LIB_LIST_TOP; y < LIB_LIST_BOT; y++)
        if (gfx_get(&g, 237, y) == THEME_DARK.surface_alt) found = true;
    CHECK(!found); /* 5 rows fit — no scrollbar */

    setup();
    lib_row_t many[60];
    for (int i = 0; i < 60; i++) {
        many[i].text = "x"; many[i].has_sub = false; many[i].is_current = false;
    }
    screen_library_draw(&g, &THEME_DARK, many, 60, 0, 0, "H", LIB_LEVEL_TRACK);
    found = false;
    for (int y = LIB_LIST_TOP; y < LIB_LIST_BOT; y++)
        if (gfx_get(&g, 237, y) == THEME_DARK.surface_alt) found = true;
    CHECK(found);
}

TEST(all_three_themes_are_distinct_and_readable)
{
    CHECK_EQ(THEME_COUNT, 3);
    for (int i = 0; i < THEME_COUNT; i++) {
        const theme_t *t = ALL_THEMES[i];
        CHECK(t->name != NULL);
        /* Text must not be the same colour as the background it sits on. */
        CHECK(t->text_primary != t->bg);
        CHECK(t->text_secondary != t->bg);
        CHECK(t->accent != t->bg);
    }
    CHECK(THEME_DARK.bg != THEME_IPOD.bg);
}

int main(void)
{
    printf("gfx + library screen\n");
    RUN(rgb565_packing_and_888_conversion);
    RUN(fill_and_read_back);
    RUN(drawing_off_screen_is_safe);
    RUN(clipping_confines_drawing);
    RUN(clear_ignores_the_clip_rect);
    RUN(rect_outline_is_hollow);
    RUN(round_rect_has_cut_corners_and_full_middle);
    RUN(round_rect_clamps_an_oversized_radius);
    RUN(text_lands_in_the_framebuffer);
    RUN(text_respects_the_clip_rect);
    RUN(library_screen_draws_without_escaping_the_buffer);
    RUN(library_visible_rows_is_consistent);
    RUN(scroll_clamp_keeps_selection_visible);
    RUN(scroll_clamp_never_leaves_blank_rows);
    RUN(selected_row_is_highlighted);
    RUN(scrollbar_appears_only_when_needed);
    RUN(all_three_themes_are_distinct_and_readable);
    return TEST_SUMMARY();
}
