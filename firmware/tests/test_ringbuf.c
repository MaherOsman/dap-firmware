#include "test.h"
#include "../src/core/ringbuf.h"
#include <stdlib.h>

static uint8_t store[16];
static ringbuf_t rb;

static void setup(void) { CHECK(rb_init(&rb, store, sizeof(store))); }

TEST(rb_rejects_non_power_of_two)
{
    ringbuf_t r;
    uint8_t s[16];
    CHECK(!rb_init(&r, s, 15));
    CHECK(!rb_init(&r, s, 0));
    CHECK(!rb_init(&r, s, 1));
    CHECK(rb_init(&r, s, 16));
}

TEST(rb_empty_at_start)
{
    setup();
    CHECK(rb_is_empty(&rb));
    CHECK(!rb_is_full(&rb));
    CHECK_EQ(rb_used(&rb), 0);
    CHECK_EQ(rb_free(&rb), 15); /* one slot always reserved */
}

TEST(rb_write_then_read_roundtrip)
{
    setup();
    uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t out[8] = {0};
    CHECK_EQ(rb_write(&rb, in, 8), 8);
    CHECK_EQ(rb_used(&rb), 8);
    CHECK_EQ(rb_read(&rb, out, 8), 8);
    CHECK_EQ(memcmp(in, out, 8), 0);
    CHECK(rb_is_empty(&rb));
}

TEST(rb_write_clamps_at_capacity)
{
    setup();
    uint8_t in[32];
    for (int i = 0; i < 32; i++) in[i] = (uint8_t)i;
    CHECK_EQ(rb_write(&rb, in, 32), 15);
    CHECK(rb_is_full(&rb));
    CHECK_EQ(rb_write(&rb, in, 1), 0);
}

TEST(rb_read_clamps_at_available)
{
    setup();
    uint8_t in[4] = {9, 9, 9, 9};
    uint8_t out[16] = {0};
    rb_write(&rb, in, 4);
    CHECK_EQ(rb_read(&rb, out, 16), 4);
}

TEST(rb_wraps_correctly)
{
    setup();
    uint8_t in[10], out[10];
    for (int i = 0; i < 10; i++) in[i] = (uint8_t)(i + 100);

    rb_write(&rb, in, 10);
    rb_read(&rb, out, 10);        /* tail now at 10 */
    CHECK_EQ(rb_write(&rb, in, 10), 10); /* wraps across the end */
    CHECK_EQ(rb_used(&rb), 10);
    memset(out, 0, sizeof(out));
    CHECK_EQ(rb_read(&rb, out, 10), 10);
    CHECK_EQ(memcmp(in, out, 10), 0);
}

TEST(rb_peek_does_not_consume)
{
    setup();
    uint8_t in[4] = {1, 2, 3, 4}, out[4] = {0};
    rb_write(&rb, in, 4);
    CHECK_EQ(rb_peek(&rb, out, 4), 4);
    CHECK_EQ(rb_used(&rb), 4);
    CHECK_EQ(memcmp(in, out, 4), 0);
    CHECK_EQ(rb_discard(&rb, 2), 2);
    CHECK_EQ(rb_used(&rb), 2);
}

TEST(rb_zero_copy_paths_agree)
{
    setup();
    uint8_t *wp;
    size_t n = rb_write_ptr(&rb, &wp);
    CHECK(n > 0);
    for (size_t i = 0; i < n; i++) wp[i] = (uint8_t)(i + 1);
    rb_write_commit(&rb, n);
    CHECK_EQ(rb_used(&rb), n);

    const uint8_t *rp;
    size_t m = rb_read_ptr(&rb, &rp);
    CHECK_EQ(m, n);
    for (size_t i = 0; i < m; i++) CHECK_EQ(rp[i], (uint8_t)(i + 1));
    rb_read_commit(&rb, m);
    CHECK(rb_is_empty(&rb));
}

/*
 * Randomised producer/consumer soak: the exact pattern that catches the
 * off-by-one wrap bugs a hand-written test misses.
 */
TEST(rb_random_soak_preserves_stream)
{
    uint8_t big[256];
    ringbuf_t r;
    rb_init(&r, big, sizeof(big));

    uint8_t next_write = 0, next_read = 0;
    uint8_t tmp[64];
    srand(1234);

    for (int iter = 0; iter < 20000; iter++) {
        size_t want = (size_t)(rand() % 64);
        for (size_t i = 0; i < want; i++) tmp[i] = (uint8_t)(next_write + i);
        size_t wrote = rb_write(&r, tmp, want);
        next_write = (uint8_t)(next_write + wrote);

        size_t take = (size_t)(rand() % 64);
        size_t got = rb_read(&r, tmp, take);
        for (size_t i = 0; i < got; i++) {
            if (tmp[i] != (uint8_t)(next_read + i)) {
                CHECK_EQ(tmp[i], (uint8_t)(next_read + i));
                return;
            }
        }
        next_read = (uint8_t)(next_read + got);
        CHECK_EQ(rb_used(&r), (size_t)(uint8_t)(next_write - next_read));
    }
    CHECK(true);
}

int main(void)
{
    printf("ringbuf\n");
    RUN(rb_rejects_non_power_of_two);
    RUN(rb_empty_at_start);
    RUN(rb_write_then_read_roundtrip);
    RUN(rb_write_clamps_at_capacity);
    RUN(rb_read_clamps_at_available);
    RUN(rb_wraps_correctly);
    RUN(rb_peek_does_not_consume);
    RUN(rb_zero_copy_paths_agree);
    RUN(rb_random_soak_preserves_stream);
    return TEST_SUMMARY();
}
