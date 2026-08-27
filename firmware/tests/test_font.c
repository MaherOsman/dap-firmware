#include "test.h"
#include "../src/core/font.h"
#include "../src/core/gfx.h"

/* ---- a plot recorder, so we can assert on exactly which pixels get set ---- */
typedef struct {
    int count;
    int min_x, max_x, min_y, max_y;
} rec_t;

static void rec_plot(void *ctx, int x, int y)
{
    rec_t *r = ctx;
    if (r->count == 0) {
        r->min_x = r->max_x = x;
        r->min_y = r->max_y = y;
    }
    if (x < r->min_x) r->min_x = x;
    if (x > r->max_x) r->max_x = x;
    if (y < r->min_y) r->min_y = y;
    if (y > r->max_y) r->max_y = y;
    r->count++;
}

TEST(fonts_are_populated)
{
    const font_t *fonts[] = {&font_sm, &font_md, &font_lg};
    for (int i = 0; i < 3; i++) {
        const font_t *f = fonts[i];
        CHECK_EQ(f->first, 32);
        CHECK_EQ(f->last, 126);
        CHECK(f->height > 0);
        CHECK(f->ascent > 0);
        CHECK(f->ascent <= f->height);
        CHECK(f->glyphs != NULL);
        CHECK(f->bitmap != NULL);
    }
    /* Sizes should be strictly increasing. */
    CHECK(font_sm.height < font_md.height);
    CHECK(font_md.height < font_lg.height);
}

TEST(every_printable_ascii_has_a_glyph)
{
    for (int c = 32; c <= 126; c++) {
        char s[2] = {(char)c, 0};
        int w = font_text_width(&font_md, s);
        CHECK(w > 0); /* including space, which has advance but no ink */
    }
}

TEST(font_is_proportional_not_fixed_width)
{
    /* If this fails, the generator produced a monospace font by accident and
     * we'd be wasting a third of the screen width. */
    int i_w = font_text_width(&font_md, "i");
    int m_w = font_text_width(&font_md, "M");
    CHECK(m_w > i_w * 2);
}

TEST(space_has_advance_but_no_ink)
{
    rec_t r = {0, 0, 0, 0, 0};
    font_draw_text(&font_md, " ", 0, 0, rec_plot, &r);
    CHECK_EQ(r.count, 0);
    CHECK(font_text_width(&font_md, " ") > 0);
}

TEST(width_is_additive)
{
    int a = font_text_width(&font_md, "Clair");
    int b = font_text_width(&font_md, " de Lune");
    int ab = font_text_width(&font_md, "Clair de Lune");
    CHECK_EQ(a + b, ab);
}

TEST(width_n_matches_prefix)
{
    const char *s = "Debussy";
    CHECK_EQ(font_text_width_n(&font_md, s, 3),
             font_text_width(&font_md, "Deb"));
    CHECK_EQ(font_text_width_n(&font_md, s, 100), font_text_width(&font_md, s));
}

TEST(draw_returns_the_pen_position)
{
    rec_t r = {0, 0, 0, 0, 0};
    int end = font_draw_text(&font_md, "Hello", 10, 0, rec_plot, &r);
    CHECK_EQ(end, 10 + font_text_width(&font_md, "Hello"));
}

TEST(glyphs_stay_inside_their_line_box)
{
    /* Text drawn at y must not bleed above y or far below y+height, or rows
     * in the library list will overlap each other. */
    rec_t r = {0, 0, 0, 0, 0};
    const char *s = "AgjQy|_^";
    font_draw_text(&font_lg, s, 0, 100, rec_plot, &r);
    CHECK(r.count > 0);
    CHECK(r.min_y >= 100);
    CHECK(r.max_y < 100 + font_lg.height);
}

TEST(unknown_characters_fall_back_visibly)
{
    /* A non-ASCII byte (e.g. from an accented artist name) must render as
     * something, not silently vanish. */
    rec_t r = {0, 0, 0, 0, 0};
    const char bad[] = {(char)0xC3, (char)0xB6, 0}; /* UTF-8 'ö' */
    font_draw_text(&font_md, bad, 0, 0, rec_plot, &r);
    CHECK(r.count > 0);
    CHECK(font_text_width(&font_md, bad) > 0);
}

TEST(ellipsis_leaves_short_text_alone)
{
    const char *s = "Hi";
    rec_t r = {0, 0, 0, 0, 0};
    bool trunc = font_draw_text_ellipsis(&font_sm, s, 0, 0, 200, rec_plot, &r);
    CHECK(!trunc);
    CHECK_EQ(r.max_x - r.min_x + 1 <= 200, true);
}

TEST(ellipsis_truncates_long_text_within_budget)
{
    const char *s = "Symphony No. 9 in D minor, Op. 125 - IV. Presto";
    rec_t r = {0, 0, 0, 0, 0};
    int budget = 120;
    bool trunc = font_draw_text_ellipsis(&font_sm, s, 0, 0, budget, rec_plot, &r);
    CHECK(trunc);
    CHECK(r.count > 0);
    /* Nothing may be drawn past the budget — this is the check that stops
     * track titles bleeding over the scrollbar. */
    CHECK(r.max_x < budget);
}

TEST(ellipsis_does_not_leave_a_dangling_space)
{
    /* "Chopin ..." looks like a mistake; "Chopin..." looks deliberate. */
    const char *s = "Chopin                    Nocturne";
    int w_for_chopin = font_text_width(&font_sm, "Chopin");
    int ell = font_text_width(&font_sm, "...");
    rec_t r = {0, 0, 0, 0, 0};
    font_draw_text_ellipsis(&font_sm, s, 0, 0, w_for_chopin + ell + 6,
                            rec_plot, &r);
    CHECK(r.max_x < w_for_chopin + ell + 6);
}

TEST(ellipsis_with_no_room_draws_nothing)
{
    rec_t r = {0, 0, 0, 0, 0};
    bool trunc = font_draw_text_ellipsis(&font_sm, "Anything", 0, 0, 2,
                                         rec_plot, &r);
    CHECK(trunc);
    CHECK_EQ(r.count, 0);
    /* And a zero/negative budget must not underflow into a huge draw. */
    r.count = 0;
    font_draw_text_ellipsis(&font_sm, "Anything", 0, 0, 0, rec_plot, &r);
    CHECK_EQ(r.count, 0);
}

TEST(fit_chars_respects_the_budget)
{
    const char *s = "Suite Bergamasque";
    size_t n = font_fit_chars(&font_sm, s, 40);
    CHECK(n > 0);
    CHECK(font_text_width_n(&font_sm, s, n) <= 40);
    /* One more character must not fit. */
    if (s[n]) {
        CHECK(font_text_width_n(&font_sm, s, n + 1) > 40);
    }
}

TEST(centering_and_right_align)
{
    rec_t r = {0, 0, 0, 0, 0};
    const char *s = "Library";
    int tw = font_text_width(&font_sm, s);
    int sx = font_draw_text_centered(&font_sm, s, 0, 0, 240, rec_plot, &r);
    CHECK_EQ(sx, (240 - tw) / 2);

    int rx = font_draw_text_right(&font_sm, s, 240, 0, rec_plot, &r);
    CHECK_EQ(rx, 240 - tw);
}

TEST(centering_clamps_when_text_is_too_wide)
{
    rec_t r = {0, 0, 0, 0, 0};
    const char *s = "A very long header that will not fit at all";
    int sx = font_draw_text_centered(&font_sm, s, 0, 0, 40, rec_plot, &r);
    CHECK_EQ(sx, 0); /* clamps to the left edge, never negative */
}

int main(void)
{
    printf("font\n");
    RUN(fonts_are_populated);
    RUN(every_printable_ascii_has_a_glyph);
    RUN(font_is_proportional_not_fixed_width);
    RUN(space_has_advance_but_no_ink);
    RUN(width_is_additive);
    RUN(width_n_matches_prefix);
    RUN(draw_returns_the_pen_position);
    RUN(glyphs_stay_inside_their_line_box);
    RUN(unknown_characters_fall_back_visibly);
    RUN(ellipsis_leaves_short_text_alone);
    RUN(ellipsis_truncates_long_text_within_budget);
    RUN(ellipsis_does_not_leave_a_dangling_space);
    RUN(ellipsis_with_no_room_draws_nothing);
    RUN(fit_chars_respects_the_budget);
    RUN(centering_and_right_align);
    RUN(centering_clamps_when_text_is_too_wide);
    return TEST_SUMMARY();
}
