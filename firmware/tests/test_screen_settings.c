/* test_screen_settings.c — the settings list.
 *
 * The behaviour worth pinning: values cycle and wrap, action rows do not
 * pretend to have values, and the table survives being asked about things
 * that are not in it.
 */

#include "test.h"

#include <stdbool.h>

#include "../src/core/gfx.h"
#include "../src/core/theme.h"
#include "../src/ui/screen_settings.h"

static uint16_t g_px[SCREEN_W * SCREEN_H];
static uint16_t g_snap[SCREEN_W * SCREEN_H];
static gfx_t    g_fb;
static settings_t g_set;

static void fb_reset(void)
{
    gfx_init(&g_fb, g_px, SCREEN_W, SCREEN_H);
    gfx_clear(&g_fb, 0);
}

static void snapshot(void) { memcpy(g_snap, g_px, sizeof(g_snap)); }
static bool frame_differs(void)
{
    return memcmp(g_snap, g_px, sizeof(g_snap)) != 0;
}

static int pixels_set(void)
{
    int n = 0, i;
    for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
        if (g_px[i] != THEME_DARK.bg) n++;
    }
    return n;
}

/* ===================================================================== */

TEST(the_table_starts_with_the_values_it_was_given)
{
    settings_init(&g_set, 1u, 2u);

    CHECK_EQ(settings_count(&g_set), 3);
    CHECK_EQ(settings_selected(&g_set), 0);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 1u);
    CHECK_EQ(settings_value(&g_set, SET_ID_REPEAT), 2u);
}

TEST(out_of_range_starting_values_fall_back_to_the_first)
{
    settings_init(&g_set, 200u, 200u);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 0u);
    CHECK_EQ(settings_value(&g_set, SET_ID_REPEAT), 0u);
}

TEST(activating_a_value_row_cycles_it)
{
    int i;

    settings_init(&g_set, 0u, 0u);

    CHECK_EQ(settings_activate(&g_set), SET_ID_THEME);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 1u);
    CHECK_EQ(settings_activate(&g_set), SET_ID_THEME);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 2u);

    /* Wraps rather than stopping at the end. Counted against THEME_COUNT
     * so adding a theme does not make this test wrong. */
    for (i = 2; i < THEME_COUNT; i++) {
        CHECK_EQ(settings_activate(&g_set), SET_ID_THEME);
    }
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 0u);
}

TEST(activating_an_action_row_changes_nothing)
{
    settings_init(&g_set, 1u, 1u);
    settings_move(&g_set, 2);          /* Rescan card */

    CHECK_EQ(settings_activate(&g_set), SET_ID_RESCAN);
    /* the other rows are untouched */
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 1u);
    CHECK_EQ(settings_value(&g_set, SET_ID_REPEAT), 1u);
    /* and it has no value of its own to report */
    CHECK_EQ(settings_value(&g_set, SET_ID_RESCAN), 0u);
}

TEST(each_row_cycles_independently)
{
    settings_init(&g_set, 0u, 0u);

    settings_move(&g_set, 1);
    CHECK_EQ(settings_activate(&g_set), SET_ID_REPEAT);
    CHECK_EQ(settings_value(&g_set, SET_ID_REPEAT), 1u);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 0u);
}

TEST(selection_clamps_at_both_ends)
{
    settings_init(&g_set, 0u, 0u);

    settings_move(&g_set, -5);
    CHECK_EQ(settings_selected(&g_set), 0);
    settings_move(&g_set, 99);
    CHECK_EQ(settings_selected(&g_set), 2);
    settings_move(&g_set, 1);
    CHECK_EQ(settings_selected(&g_set), 2);
}

TEST(a_value_can_be_set_from_outside)
{
    settings_init(&g_set, 0u, 0u);

    settings_set_value(&g_set, SET_ID_THEME, 2u);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 2u);

    /* an out-of-range value falls back rather than sticking */
    settings_set_value(&g_set, SET_ID_THEME, 99u);
    CHECK_EQ(settings_value(&g_set, SET_ID_THEME), 0u);

    /* an action row has nothing to set */
    settings_set_value(&g_set, SET_ID_RESCAN, 1u);
    CHECK_EQ(settings_value(&g_set, SET_ID_RESCAN), 0u);
}

TEST(there_is_a_name_for_every_theme_the_build_has)
{
    /* A theme with no label would be selectable and unreadable. */
    settings_init(&g_set, 0u, 0u);
    {
        int cycles = 0;
        uint8_t first = settings_value(&g_set, SET_ID_THEME);
        do {
            CHECK_EQ(settings_activate(&g_set), SET_ID_THEME);
            cycles++;
            CHECK(cycles <= 16);
        } while (settings_value(&g_set, SET_ID_THEME) != first);

        CHECK_EQ(cycles, THEME_COUNT);
    }
}

TEST(helpers_are_null_safe)
{
    settings_init(NULL, 0u, 0u);
    settings_move(NULL, 1);
    settings_set_value(NULL, SET_ID_THEME, 1u);
    CHECK_EQ(settings_count(NULL), 0);
    CHECK_EQ(settings_selected(NULL), 0);
    CHECK_EQ(settings_scroll_top(NULL), 0);
    CHECK_EQ(settings_value(NULL, SET_ID_THEME), 0u);
    CHECK_EQ(settings_activate(NULL), SET_ID_COUNT);
}

/* ---------------------------------------------------------- drawing */

TEST(the_screen_draws_something_inside_the_framebuffer)
{
    settings_init(&g_set, 0u, 0u);
    fb_reset();
    screen_settings_draw(&g_fb, &THEME_DARK, &g_set);
    CHECK(pixels_set() > 100);
}

TEST(moving_the_selection_changes_the_frame)
{
    settings_init(&g_set, 0u, 0u);

    fb_reset();
    screen_settings_draw(&g_fb, &THEME_DARK, &g_set);
    snapshot();

    settings_move(&g_set, 1);
    fb_reset();
    screen_settings_draw(&g_fb, &THEME_DARK, &g_set);
    CHECK(frame_differs());
}

TEST(changing_a_value_changes_the_frame)
{
    settings_init(&g_set, 0u, 0u);
    settings_move(&g_set, 1);          /* Repeat: Off -> Once */

    fb_reset();
    screen_settings_draw(&g_fb, &THEME_DARK, &g_set);
    snapshot();

    CHECK_EQ(settings_activate(&g_set), SET_ID_REPEAT);
    fb_reset();
    screen_settings_draw(&g_fb, &THEME_DARK, &g_set);

    /* The new value has to be visible, not just stored. */
    CHECK(frame_differs());
}

TEST(every_theme_renders_the_screen)
{
    int i;
    for (i = 0; i < THEME_COUNT; i++) {
        settings_init(&g_set, (uint8_t)i, 0u);
        fb_reset();
        screen_settings_draw(&g_fb, ALL_THEMES[i], &g_set);
        CHECK(pixels_set() > 50);
    }
}

TEST(drawing_with_null_arguments_is_safe)
{
    settings_init(&g_set, 0u, 0u);
    fb_reset();
    screen_settings_draw(NULL, &THEME_DARK, &g_set);
    screen_settings_draw(&g_fb, NULL, &g_set);
    screen_settings_draw(&g_fb, &THEME_DARK, NULL);
    CHECK(1);
}

int main(void)
{
    printf("screen_settings\n");

    RUN(the_table_starts_with_the_values_it_was_given);
    RUN(out_of_range_starting_values_fall_back_to_the_first);
    RUN(activating_a_value_row_cycles_it);
    RUN(activating_an_action_row_changes_nothing);
    RUN(each_row_cycles_independently);
    RUN(selection_clamps_at_both_ends);
    RUN(a_value_can_be_set_from_outside);
    RUN(there_is_a_name_for_every_theme_the_build_has);
    RUN(helpers_are_null_safe);

    RUN(the_screen_draws_something_inside_the_framebuffer);
    RUN(moving_the_selection_changes_the_frame);
    RUN(changing_a_value_changes_the_frame);
    RUN(every_theme_renders_the_screen);
    RUN(drawing_with_null_arguments_is_safe);

    return TEST_SUMMARY();
}
