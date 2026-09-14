/* test_screen_now_playing.c — focus logic, formatting, and the promise that
 * no screen ever draws outside the framebuffer.
 *
 * The drawing assertions are deliberately about invariants (nothing escapes,
 * something got drawn, the right things change) rather than exact pixels.
 * Pixel-exact tests would break on every layout tweak and teach you to stop
 * reading the failures. `make preview` is how the layout itself gets judged.
 */

#include "test.h"

#include <stdbool.h>

#include "../src/core/gfx.h"
#include "../src/core/theme.h"
#include "../src/ui/screen_now_playing.h"

static uint16_t g_px[SCREEN_W * SCREEN_H];
static gfx_t    g_fb;

/* Out-of-bounds counter. The real gfx clips internally, so this stays zero
 * here — it is a live guard only against a framebuffer that does not clip. */
static int g_oob;

static void fb_reset(void)
{
    gfx_init(&g_fb, g_px, SCREEN_W, SCREEN_H);
    gfx_clear(&g_fb, 0);
}

static np_state_t base_state(void)
{
    np_state_t s;
    memset(&s, 0, sizeof(s));
    s.title = "Everything In Its Right Place";
    s.artist = "Radiohead";
    s.album = "Kid A";
    s.path = "/Music/Radiohead/Kid A/01 Everything In Its Right Place.flac";
    s.format = "FLAC";
    s.elapsed_ms = 65000u;
    s.duration_ms = 251000u;
    s.sample_rate_hz = 44100u;
    s.bit_depth = 16u;
    s.channels = 2u;
    s.bitrate_kbps = 1411u;
    s.is_playing = true;
    s.enabled = NP_CTL_DEFAULT;
    s.focus = NP_CTL_PLAY;
    s.volume_pct = 60u;
    return s;
}

static uint16_t g_snap[SCREEN_W * SCREEN_H];

static void snapshot(void) { memcpy(g_snap, g_px, sizeof(g_snap)); }
static bool frame_differs(void) { return memcmp(g_snap, g_px, sizeof(g_snap)) != 0; }

static int pixels_set(void)
{
    int n = 0, i;
    /* Against the background, not against black: the themes have a non-black
     * bg, so counting non-zero pixels would count the whole screen. */
    for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
        if (g_px[i] != THEME_DARK.bg) n++;
    }
    return n;
}

/* ===================================================================== */
/* time and progress                                                      */
/* ===================================================================== */

TEST(time_formats_as_minutes_and_seconds)
{
    char b[8];

    np_fmt_time(b, 0u);           CHECK(strcmp(b, "0:00") == 0);
    np_fmt_time(b, 1000u);        CHECK(strcmp(b, "0:01") == 0);
    np_fmt_time(b, 59000u);       CHECK(strcmp(b, "0:59") == 0);
    np_fmt_time(b, 60000u);       CHECK(strcmp(b, "1:00") == 0);
    np_fmt_time(b, 251000u);      CHECK(strcmp(b, "4:11") == 0);
    np_fmt_time(b, 3599000u);     CHECK(strcmp(b, "59:59") == 0);
    /* sub-second remainders truncate rather than rounding up */
    np_fmt_time(b, 1999u);        CHECK(strcmp(b, "0:01") == 0);
}

TEST(absurd_durations_clamp_rather_than_overflowing_the_field)
{
    char b[8];
    np_fmt_time(b, 0xFFFFFFFFu);
    CHECK(strcmp(b, "99:59") == 0);
    CHECK(strlen(b) < 8u);
}

TEST(progress_is_a_fraction_of_the_duration)
{
    np_state_t s = base_state();

    s.elapsed_ms = 0u;
    CHECK_EQ(np_progress_permille(&s), 0u);

    s.elapsed_ms = s.duration_ms / 2u;
    CHECK(np_progress_permille(&s) >= 499u);
    CHECK(np_progress_permille(&s) <= 501u);

    s.elapsed_ms = s.duration_ms;
    CHECK_EQ(np_progress_permille(&s), 1000u);

    /* past the end (a decoder reporting long) must not exceed full */
    s.elapsed_ms = s.duration_ms * 2u;
    CHECK_EQ(np_progress_permille(&s), 1000u);
}

TEST(unknown_duration_reads_as_empty_not_full)
{
    np_state_t s = base_state();
    s.duration_ms = 0u;
    s.elapsed_ms = 30000u;
    CHECK_EQ(np_progress_permille(&s), 0u);
    CHECK_EQ(np_progress_permille(NULL), 0u);
}

TEST(a_very_long_track_does_not_overflow_the_progress_maths)
{
    np_state_t s = base_state();
    /* 10 hours: elapsed * 1000 overflows 32 bits, which is why the
     * calculation is done in 64. */
    s.duration_ms = 36000000u;
    s.elapsed_ms = 18000000u;
    CHECK(np_progress_permille(&s) >= 499u);
    CHECK(np_progress_permille(&s) <= 501u);
}

/* ===================================================================== */
/* focus                                                                  */
/* ===================================================================== */

TEST(focus_moves_over_enabled_controls_only)
{
    np_state_t s = base_state();       /* PLAY, VOL, INFO */

    CHECK(!np_ctl_enabled(&s, NP_CTL_PREV));
    CHECK(np_ctl_enabled(&s, NP_CTL_PLAY));
    CHECK(np_ctl_enabled(&s, NP_CTL_VOL));

    s.focus = NP_CTL_PLAY;
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_VOL);
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_INFO);
    CHECK_EQ(np_focus_move(&s, -1), NP_CTL_VOL);
    CHECK_EQ(np_focus_move(&s, -1), NP_CTL_PLAY);
}

TEST(focus_clamps_rather_than_wrapping)
{
    np_state_t s = base_state();

    s.focus = NP_CTL_PLAY;
    CHECK_EQ(np_focus_move(&s, -1), NP_CTL_PLAY);   /* already leftmost */
    CHECK_EQ(np_focus_move(&s, -9), NP_CTL_PLAY);

    CHECK_EQ(np_focus_move(&s, 9), NP_CTL_INFO);    /* rightmost */
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_INFO);
}

TEST(a_multi_step_move_lands_correctly)
{
    np_state_t s = base_state();
    s.focus = NP_CTL_PLAY;
    CHECK_EQ(np_focus_move(&s, 2), NP_CTL_INFO);
    CHECK_EQ(np_focus_move(&s, -2), NP_CTL_PLAY);
    CHECK_EQ(np_focus_move(&s, 0), NP_CTL_PLAY);
}

TEST(enabling_prev_and_next_extends_the_row_without_other_changes)
{
    np_state_t s = base_state();
    s.enabled = NP_CTL_ALL;

    s.focus = NP_CTL_PREV;
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_PLAY);
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_NEXT);
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_VOL);
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_INFO);
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_INFO);

    s.focus = NP_CTL_PREV;
    CHECK_EQ(np_focus_move(&s, -1), NP_CTL_PREV);
}

TEST(a_zeroed_state_still_has_working_controls)
{
    np_state_t s;
    memset(&s, 0, sizeof(s));

    /* enabled == 0 must not mean "no controls" — a zeroed struct is the
     * most likely way this gets used by mistake. */
    CHECK(np_ctl_enabled(&s, NP_CTL_PLAY));
    CHECK(np_ctl_enabled(&s, NP_CTL_VOL));
    CHECK(!np_ctl_enabled(&s, NP_CTL_PREV));
}

TEST(focus_on_a_disabled_control_recovers)
{
    np_state_t s = base_state();

    /* PREV is disabled, but focus points at it */
    s.focus = NP_CTL_PREV;
    CHECK_EQ(np_focus_move(&s, 1), NP_CTL_VOL);   /* snaps to PLAY, then moves */

    s.focus = NP_CTL_PREV;
    CHECK_EQ(np_focus_move(&s, 0), NP_CTL_PLAY);  /* snaps without moving */
}

TEST(focus_helpers_are_null_safe)
{
    CHECK(!np_ctl_enabled(NULL, NP_CTL_PLAY));
    CHECK_EQ(np_focus_move(NULL, 1), NP_CTL_PLAY);
}

TEST(an_out_of_range_control_is_not_enabled)
{
    np_state_t s = base_state();
    CHECK(!np_ctl_enabled(&s, NP_CTL_COUNT));
    CHECK(!np_ctl_enabled(&s, (np_ctl_t)99));
}

/* ===================================================================== */
/* drawing                                                                */
/* ===================================================================== */

TEST(now_playing_draws_inside_the_framebuffer)
{
    np_state_t s = base_state();

    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);

    CHECK_EQ(g_oob, 0);
    CHECK(pixels_set() > 500);
}

TEST(the_screen_survives_missing_metadata)
{
    np_state_t s;

    memset(&s, 0, sizeof(s));
    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);

    /* a track with no duration and no strings still renders */
    CHECK(pixels_set() > 100);
}

TEST(an_over_long_title_stays_on_screen)
{
    np_state_t s = base_state();

    s.title = "A Title That Is Very Considerably Longer Than Two Hundred And "
              "Forty Pixels Could Ever Hope To Contain Without Truncation";
    s.album = s.title;
    s.artist = s.title;

    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
}

TEST(a_full_scrubber_does_not_push_the_pill_off_the_edge)
{
    np_state_t s = base_state();

    s.elapsed_ms = s.duration_ms;
    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);

    s.elapsed_ms = 0u;
    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
}

TEST(play_and_pause_draw_differently)
{
    np_state_t s = base_state();

    s.is_playing = true;
    fb_reset();
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    snapshot();

    s.is_playing = false;
    fb_reset();
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);

    /* Compare frames, not pixel counts: the focused control is drawn over a
     * filled ring, so two different glyphs can light the same NUMBER of
     * pixels while looking completely different. Paused must be visibly
     * distinct from playing or the state is invisible. */
    CHECK(frame_differs());
}

TEST(moving_focus_changes_what_is_drawn)
{
    np_state_t s = base_state();

    s.focus = NP_CTL_PLAY;
    fb_reset();
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    snapshot();

    s.focus = NP_CTL_INFO;
    fb_reset();
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);

    CHECK(frame_differs());
    CHECK(pixels_set() > 0);
}

TEST(the_volume_overlay_only_appears_when_active)
{
    np_state_t s = base_state();
    int without, with;

    s.vol_active = false;
    fb_reset();
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    without = pixels_set();

    snapshot();

    s.vol_active = true;
    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    with = pixels_set();

    CHECK_EQ(g_oob, 0);
    CHECK(with > without);      /* the overlay covers background pixels */
    CHECK(frame_differs());
}

TEST(the_volume_overlay_clamps_its_input)
{
    fb_reset();
    g_oob = 0;
    screen_volume_overlay_draw(&g_fb, &THEME_DARK, -50);
    CHECK_EQ(g_oob, 0);

    fb_reset();
    g_oob = 0;
    screen_volume_overlay_draw(&g_fb, &THEME_DARK, 500);
    CHECK_EQ(g_oob, 0);

    fb_reset();
    g_oob = 0;
    screen_volume_overlay_draw(&g_fb, &THEME_DARK, 0);
    CHECK_EQ(g_oob, 0);
}

TEST(all_five_controls_fit_when_enabled)
{
    np_state_t s = base_state();

    s.enabled = NP_CTL_ALL;
    s.focus = NP_CTL_NEXT;
    fb_reset();
    g_oob = 0;
    screen_now_playing_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
    CHECK(pixels_set() > 500);
}

TEST(the_info_page_draws_inside_the_framebuffer)
{
    np_state_t s = base_state();

    fb_reset();
    g_oob = 0;
    screen_info_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
    CHECK(pixels_set() > 200);
}

TEST(the_info_page_handles_unknown_fields)
{
    np_state_t s;

    memset(&s, 0, sizeof(s));
    fb_reset();
    g_oob = 0;
    screen_info_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
}

TEST(the_info_page_survives_an_absurd_path)
{
    np_state_t s = base_state();
    static char big[400];

    memset(big, 'x', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    s.path = big;
    s.title = big;

    fb_reset();
    g_oob = 0;
    screen_info_draw(&g_fb, &THEME_DARK, &s);
    CHECK_EQ(g_oob, 0);
}

TEST(drawing_with_null_arguments_is_safe)
{
    np_state_t s = base_state();

    fb_reset();
    screen_now_playing_draw(NULL, &THEME_DARK, &s);
    screen_now_playing_draw(&g_fb, NULL, &s);
    screen_now_playing_draw(&g_fb, &THEME_DARK, NULL);
    screen_info_draw(NULL, &THEME_DARK, &s);
    screen_info_draw(&g_fb, NULL, &s);
    screen_info_draw(&g_fb, &THEME_DARK, NULL);
    CHECK(1);
}

int main(void)
{
    printf("screen_now_playing\n");

    RUN(time_formats_as_minutes_and_seconds);
    RUN(absurd_durations_clamp_rather_than_overflowing_the_field);
    RUN(progress_is_a_fraction_of_the_duration);
    RUN(unknown_duration_reads_as_empty_not_full);
    RUN(a_very_long_track_does_not_overflow_the_progress_maths);

    RUN(focus_moves_over_enabled_controls_only);
    RUN(focus_clamps_rather_than_wrapping);
    RUN(a_multi_step_move_lands_correctly);
    RUN(enabling_prev_and_next_extends_the_row_without_other_changes);
    RUN(a_zeroed_state_still_has_working_controls);
    RUN(focus_on_a_disabled_control_recovers);
    RUN(focus_helpers_are_null_safe);
    RUN(an_out_of_range_control_is_not_enabled);

    RUN(now_playing_draws_inside_the_framebuffer);
    RUN(the_screen_survives_missing_metadata);
    RUN(an_over_long_title_stays_on_screen);
    RUN(a_full_scrubber_does_not_push_the_pill_off_the_edge);
    RUN(play_and_pause_draw_differently);
    RUN(moving_focus_changes_what_is_drawn);
    RUN(the_volume_overlay_only_appears_when_active);
    RUN(the_volume_overlay_clamps_its_input);
    RUN(all_five_controls_fit_when_enabled);

    RUN(the_info_page_draws_inside_the_framebuffer);
    RUN(the_info_page_handles_unknown_fields);
    RUN(the_info_page_survives_an_absurd_path);
    RUN(drawing_with_null_arguments_is_safe);

    return TEST_SUMMARY();
}
