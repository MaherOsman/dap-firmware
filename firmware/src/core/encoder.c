#include "encoder.h"

/*
 * Gray-code transition table, indexed by (previous << 2) | current, where
 * each 2-bit state is (A << 1) | B.
 *
 *   0 = illegal / no movement (both pins changed, i.e. a missed sample or
 *       contact bounce — deliberately ignored)
 *  +1 = one quadrature step in the direction we call "clockwise"
 *  -1 = one step counter-clockwise
 *
 * Clockwise here is the sequence 00 -> 10 -> 11 -> 01 -> 00. If the knob
 * turns the wrong way on real hardware, swap the A and B wires (or negate
 * the return value) — do not rewrite the table.
 */
static const int8_t k_quad[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

void encoder_init(encoder_t *e, bool a, bool b, bool pressed)
{
    e->state      = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    e->accum      = 0;
    e->raw        = pressed;
    e->stable     = pressed;
    e->edge_ms    = 0;
    e->press_ms   = 0;
    e->long_fired = false;
}

int encoder_update(encoder_t *e, bool a, bool b)
{
    uint8_t cur = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
    if (cur == e->state) {
        return 0;
    }
    int8_t d = k_quad[(e->state << 2) | cur];
    e->state = cur;
    if (d == 0) {
        return 0; /* illegal transition — ignore, keep the accumulator */
    }

    e->accum = (int8_t)(e->accum + d);

    /* One detent == one full 4-step quadrature cycle, and the PEC11R rests
     * at 00, so only emit when we land back on the detent position. */
    if (e->accum >= 4 && cur == 0) {
        e->accum = 0;
        return +1;
    }
    if (e->accum <= -4 && cur == 0) {
        e->accum = 0;
        return -1;
    }
    /* Guard against unbounded drift if a state is ever missed. */
    if (e->accum > 8) {
        e->accum = 8;
    }
    if (e->accum < -8) {
        e->accum = -8;
    }
    return 0;
}

btn_event_t encoder_button(encoder_t *e, bool pressed, uint32_t now_ms)
{
    if (pressed != e->raw) {
        e->raw = pressed;
        e->edge_ms = now_ms;
        return BTN_NONE;
    }

    /* Level held long enough to be real? */
    if (pressed != e->stable && (now_ms - e->edge_ms) >= ENC_DEBOUNCE_MS) {
        e->stable = pressed;
        if (pressed) {
            e->press_ms   = now_ms;
            e->long_fired = false;
            return BTN_PRESS;
        }
        /* Released: a click only if we never promoted it to a long press. */
        return e->long_fired ? BTN_RELEASE : BTN_CLICK;
    }

    if (e->stable && !e->long_fired &&
        (now_ms - e->press_ms) >= ENC_LONGPRESS_MS) {
        e->long_fired = true;
        return BTN_LONG_PRESS;
    }
    return BTN_NONE;
}
