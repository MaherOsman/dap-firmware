#include "test.h"
#include "../src/core/gfx.h"
#include "../src/core/theme.h"
#include "../src/ui/screen_library.h"

static uint16_t fb[SCREEN_W * SCREEN_H];
static gfx_t g;

static void setup(void) { gfx_init(&g, fb, SCREEN_W, SCREEN_H); gfx_clear(&g, 0); }

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
    gfx_fill_rect(&g, SCREEN_W - 5, SCREEN_H - 5, 100, 100, 0xFFFF);
    gfx_fill_rect(&g, 1000, 1000, 10, 10, 0xFFFF);
    gfx_pixel(&g, -1, 0, 0xFFFF);
    gfx_pixel(&g, 0, -1, 0xFFFF);
    gfx_pixel(&g, SCREEN_W, 0, 0xFFFF);
    gfx_pixel(&g, 0, SCREEN_H, 0xFFFF);
    CHECK_EQ(gfx_get(&g, 0, 0), 0x0000);
    CHECK_EQ(gfx_get(&g, SCREEN_W - 1, SCREEN_H - 1), 0xFFFF); /* the corner rect did land */
}

TEST(clipping_confines_drawing)
{
    setup();
    gfx_clip(&g, 100, 100, 20, 20);
    gfx_fill_rect(&g, 0, 0, SCREEN_W, SCREEN_H, 0xFFFF);
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
    CHECK_EQ(gfx_get(&g, SCREEN_W - 1, SCREEN_H - 1), 0x1234);
    /* And the clip must be restored afterwards. */
    gfx_fill_rect(&g, 0, 0, SCREEN_W, SCREEN_H, 0xFFFF);
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
    gfx_clip(&g, 0, 0, 20, SCREEN_H);
    gfx_text(&g, &font_md, "This is much wider than twenty pixels", 0, 5,
             0xFFFF);
    gfx_clip_reset(&g);
    for (int y = 0; y < 30; y++) {
        for (int x = 20; x < SCREEN_W; x++) {
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
    memset(rows, 0, sizeof(rows));
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
    CHECK_EQ(LIB_VISIBLE, (SCREEN_H - 24 - 15) / 18);
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
    memset(rows, 0, sizeof(rows));
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
    memset(rows, 0, sizeof(rows));
    for (int i = 0; i < 5; i++) {
        rows[i].text = "x"; rows[i].has_sub = false; rows[i].is_current = false;
    }
    setup();
    screen_library_draw(&g, &THEME_DARK, rows, 5, 0, 0, "H", LIB_LEVEL_TRACK);
    bool found = false;
    for (int y = LIB_LIST_TOP; y < LIB_LIST_BOT; y++)
        if (gfx_get(&g, SCREEN_W - 3, y) == THEME_DARK.surface_alt) found = true;
    CHECK(!found); /* 5 rows fit — no scrollbar */

    setup();
    lib_row_t many[60];
    memset(many, 0, sizeof(many));
    for (int i = 0; i < 60; i++) {
        many[i].text = "x"; many[i].has_sub = false; many[i].is_current = false;
    }
    screen_library_draw(&g, &THEME_DARK, many, 60, 0, 0, "H", LIB_LEVEL_TRACK);
    found = false;
    for (int y = LIB_LIST_TOP; y < LIB_LIST_BOT; y++)
        if (gfx_get(&g, SCREEN_W - 3, y) == THEME_DARK.surface_alt) found = true;
    CHECK(found);
}

TEST(every_theme_is_distinct_and_readable)
{
    /* Every theme in the build gets checked, however many there are —
     * pinning the count here just means editing this line each time a
     * theme is added, which teaches you to edit rather than to read. */
    CHECK(THEME_COUNT > 0);
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

TEST(blit_copies_and_clips)
{
    static uint16_t img[4 * 3];
    for (int i = 0; i < 12; i++) img[i] = (uint16_t)(i + 1);

    setup();
    gfx_blit(&g, 10, 20, 4, 3, img);
    CHECK_EQ(gfx_get(&g, 10, 20), 1u);
    CHECK_EQ(gfx_get(&g, 13, 20), 4u);
    CHECK_EQ(gfx_get(&g, 10, 21), 5u);   /* stride is the image width */
    CHECK_EQ(gfx_get(&g, 13, 22), 12u);
    CHECK_EQ(gfx_get(&g, 14, 20), 0u);

    /* Hanging off the corner: the visible part lands, nothing else. */
    setup();
    gfx_blit(&g, SCREEN_W - 2, SCREEN_H - 1, 4, 3, img);
    CHECK_EQ(gfx_get(&g, SCREEN_W - 2, SCREEN_H - 1), 1u);
    CHECK_EQ(gfx_get(&g, SCREEN_W - 1, SCREEN_H - 1), 2u);

    setup();
    gfx_blit(&g, -2, -1, 4, 3, img);
    CHECK_EQ(gfx_get(&g, 0, 0), 7u);     /* row 1, column 2 */
    gfx_blit(&g, 0, 0, 4, 3, NULL);      /* a NULL image is ignored */
}

TEST(veil_moves_pixels_toward_the_colour)
{
    setup();
    gfx_fill_rect(&g, 0, 0, 10, 10, 0xFFFF);
    gfx_veil_rect(&g, 0, 0, 5, 10, 0x0000, 128);
    /* white halfway to black: every channel at half, none spilling over */
    CHECK_EQ(gfx_get(&g, 0, 0), 0x7BEF);
    CHECK_EQ(gfx_get(&g, 5, 0), 0xFFFF);  /* outside the rect: untouched */

    gfx_veil_rect(&g, 5, 0, 5, 10, 0x0000, 256);
    CHECK_EQ(gfx_get(&g, 5, 0), 0x0000);  /* full strength is the colour */

    gfx_veil_rect(&g, 5, 1, 5, 1, 0xFFFF, 0);
    CHECK_EQ(gfx_get(&g, 5, 1), 0x0000);  /* zero does nothing */

    /* Veiling a colour toward itself changes nothing. */
    gfx_fill_rect(&g, 0, 0, 10, 10, 0x1234);
    gfx_veil_rect(&g, 0, 0, 10, 10, 0x1234, 200);
    CHECK_EQ(gfx_get(&g, 3, 3), 0x1234);
}

TEST(band_hash_sees_a_one_pixel_change_only_in_its_own_band)
{
    uint32_t a0, a1, b0, b1;

    setup();
    a0 = gfx_band_hash(&g, 0, 16);
    a1 = gfx_band_hash(&g, 16, 16);
    CHECK_EQ(gfx_band_hash(&g, 0, 16), a0);       /* stable */

    gfx_pixel(&g, SCREEN_W - 1, 20, 0x0001);      /* lowest bit, last column */
    b0 = gfx_band_hash(&g, 0, 16);
    b1 = gfx_band_hash(&g, 16, 16);
    CHECK_EQ(b0, a0);
    CHECK(b1 != a1);

    /* Clamped at the edges rather than reading past the buffer. */
    (void)gfx_band_hash(&g, SCREEN_H - 4, 16);
    (void)gfx_band_hash(&g, -8, 16);
    CHECK(true);
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
    RUN(every_theme_is_distinct_and_readable);
    RUN(blit_copies_and_clips);
    RUN(veil_moves_pixels_toward_the_colour);
    RUN(band_hash_sees_a_one_pixel_change_only_in_its_own_band);
    return TEST_SUMMARY();
}
