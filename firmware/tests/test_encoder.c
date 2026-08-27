#include "test.h"
#include "../src/core/encoder.h"

/* Full quadrature cycles. CW per the driver's table: 00 -> 10 -> 11 -> 01 -> 00 */
static const uint8_t cw[4]  = {0b10, 0b11, 0b01, 0b00};
static const uint8_t ccw[4] = {0b01, 0b11, 0b10, 0b00};

static int step_through(encoder_t *e, const uint8_t *seq)
{
    int total = 0;
    for (int i = 0; i < 4; i++) {
        total += encoder_update(e, (seq[i] >> 1) & 1, seq[i] & 1);
    }
    return total;
}

TEST(one_detent_per_full_cycle)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    CHECK_EQ(step_through(&e, cw), +1);
    CHECK_EQ(step_through(&e, cw), +1);
}

TEST(counter_clockwise_is_negative)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    CHECK_EQ(step_through(&e, ccw), -1);
}

TEST(partial_turn_emits_nothing)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    /* Halfway round and back — a knob nudged but not turned. */
    CHECK_EQ(encoder_update(&e, 1, 0), 0);
    CHECK_EQ(encoder_update(&e, 1, 1), 0);
    CHECK_EQ(encoder_update(&e, 1, 0), 0);
    CHECK_EQ(encoder_update(&e, 0, 0), 0);
}

TEST(contact_bounce_does_not_create_steps)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    /* Rattle on the first edge, then complete a real CW cycle. */
    int total = 0;
    for (int i = 0; i < 6; i++) {
        total += encoder_update(&e, 1, 0);
        total += encoder_update(&e, 0, 0);
    }
    CHECK_EQ(total, 0);
    CHECK_EQ(step_through(&e, cw), +1);
}

TEST(repeated_identical_samples_are_ignored)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    for (int i = 0; i < 100; i++) {
        CHECK_EQ(encoder_update(&e, 0, 0), 0);
    }
}

TEST(button_debounces)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    /* Press bounces for 3 ms, then settles. */
    CHECK_EQ(encoder_button(&e, true, 0), BTN_NONE);
    CHECK_EQ(encoder_button(&e, false, 1), BTN_NONE);
    CHECK_EQ(encoder_button(&e, true, 2), BTN_NONE);
    CHECK_EQ(encoder_button(&e, true, 4), BTN_NONE); /* not stable yet */
    CHECK_EQ(encoder_button(&e, true, 8), BTN_PRESS);
    CHECK_EQ(encoder_button(&e, true, 9), BTN_NONE); /* fires once only */
}

TEST(short_press_is_a_click)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    encoder_button(&e, true, 0);
    CHECK_EQ(encoder_button(&e, true, 10), BTN_PRESS);
    encoder_button(&e, false, 100);
    CHECK_EQ(encoder_button(&e, false, 110), BTN_CLICK);
}

TEST(long_press_fires_once_then_release)
{
    encoder_t e;
    encoder_init(&e, false, false, false);
    encoder_button(&e, true, 0);
    CHECK_EQ(encoder_button(&e, true, 10), BTN_PRESS);
    CHECK_EQ(encoder_button(&e, true, 500), BTN_NONE);
    CHECK_EQ(encoder_button(&e, true, 10 + ENC_LONGPRESS_MS), BTN_LONG_PRESS);
    CHECK_EQ(encoder_button(&e, true, 2000), BTN_NONE); /* not repeated */
    encoder_button(&e, false, 2100);
    /* A long press ends in RELEASE, not CLICK — otherwise holding to open a
     * menu would also trigger the click action on the way out. */
    CHECK_EQ(encoder_button(&e, false, 2110), BTN_RELEASE);
}

int main(void)
{
    printf("encoder\n");
    RUN(one_detent_per_full_cycle);
    RUN(counter_clockwise_is_negative);
    RUN(partial_turn_emits_nothing);
    RUN(contact_bounce_does_not_create_steps);
    RUN(repeated_identical_samples_are_ignored);
    RUN(button_debounces);
    RUN(short_press_is_a_click);
    RUN(long_press_fires_once_then_release);
    return TEST_SUMMARY();
}
