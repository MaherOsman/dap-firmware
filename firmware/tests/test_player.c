#include "test.h"
#include "../src/core/player.h"

TEST(does_not_play_until_prerolled)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100 * 180, 44100);
    CHECK_EQ(p.state, PLAYER_BUFFERING);

    player_tick(&p, 10);
    CHECK(!player_output_enabled(&p));
    player_tick(&p, 74);
    CHECK(!player_output_enabled(&p)); /* one percent short — still silent */
    player_tick(&p, 75);
    CHECK(player_output_enabled(&p));
    CHECK_EQ(p.state, PLAYER_PLAYING);
}

TEST(short_file_starts_even_if_preroll_never_fills)
{
    player_t p;
    player_init(&p);
    player_open(&p, 1000, 44100);
    player_set_exhausted(&p, true); /* whole file already read, still < 75% */
    player_tick(&p, 20);
    CHECK_EQ(p.state, PLAYER_PLAYING);
}

TEST(refill_policy)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100 * 180, 44100);

    /* While buffering, keep reading until preroll. */
    CHECK(player_needs_refill(&p, 10));
    CHECK(!player_needs_refill(&p, 80));

    player_tick(&p, 90);
    CHECK_EQ(p.state, PLAYER_PLAYING);
    CHECK(!player_needs_refill(&p, 60));
    CHECK(player_needs_refill(&p, 49)); /* dropped under half — go read */

    /* Nothing left to read: stop asking. */
    player_set_exhausted(&p, true);
    CHECK(!player_needs_refill(&p, 5));
}

TEST(pause_resumes_without_reprerolling)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100 * 10, 44100);
    player_tick(&p, 100);
    CHECK_EQ(p.state, PLAYER_PLAYING);

    player_play_pause(&p);
    CHECK_EQ(p.state, PLAYER_PAUSED);
    CHECK(!player_output_enabled(&p));

    player_play_pause(&p);
    CHECK_EQ(p.state, PLAYER_PLAYING); /* no gap on resume */
}

TEST(position_and_progress)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100 * 100, 44100);
    player_tick(&p, 100);

    player_frames_consumed(&p, 44100 * 25);
    CHECK_EQ(player_position_ms(&p), 25000);
    CHECK_EQ(player_progress_pct(&p), 25);

    /* Consuming past the end clamps rather than reporting 137%. */
    player_frames_consumed(&p, 44100 * 500);
    CHECK_EQ(player_progress_pct(&p), 100);
}

TEST(position_is_zero_for_unopened_player)
{
    player_t p;
    player_init(&p);
    CHECK_EQ(player_position_ms(&p), 0);
    CHECK_EQ(player_progress_pct(&p), 0);
}

TEST(eof_only_after_buffer_drains)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100, 44100);
    player_tick(&p, 100);
    player_set_exhausted(&p, true);

    player_tick(&p, 30);
    CHECK_EQ(p.state, PLAYER_PLAYING); /* tail of the file still queued */
    player_tick(&p, 0);
    CHECK_EQ(p.state, PLAYER_EOF);
}

TEST(underruns_counted_only_while_playing)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100, 44100);
    player_underrun(&p);            /* still buffering — not an underrun */
    CHECK_EQ(p.underruns, 0);
    player_tick(&p, 100);
    player_underrun(&p);
    player_underrun(&p);
    CHECK_EQ(p.underruns, 2);
}

TEST(volume_clamps)
{
    player_t p;
    player_init(&p);
    CHECK_EQ(player_set_volume(&p, +200), 100);
    CHECK_EQ(player_set_volume(&p, -500), 0);
    CHECK_EQ(player_set_volume(&p, +7), 7);
}

TEST(stop_resets_position)
{
    player_t p;
    player_init(&p);
    player_open(&p, 44100, 44100);
    player_tick(&p, 100);
    player_frames_consumed(&p, 22050);
    player_stop(&p);
    CHECK_EQ(p.state, PLAYER_STOPPED);
    CHECK_EQ(player_position_ms(&p), 0);
    CHECK(!player_output_enabled(&p));
}

int main(void)
{
    printf("player\n");
    RUN(does_not_play_until_prerolled);
    RUN(short_file_starts_even_if_preroll_never_fills);
    RUN(refill_policy);
    RUN(pause_resumes_without_reprerolling);
    RUN(position_and_progress);
    RUN(position_is_zero_for_unopened_player);
    RUN(eof_only_after_buffer_drains);
    RUN(underruns_counted_only_while_playing);
    RUN(volume_clamps);
    RUN(stop_resets_position);
    return TEST_SUMMARY();
}
