/*
 * encoder — quadrature decoding + button debounce for the Bourns PEC11R.
 *
 * Pure logic: you feed it the raw A/B pin levels and a millisecond tick, it
 * gives you detents and button events. That means the whole thing is
 * testable on a PC, and on the STM32 you can drive it either from a timer
 * ISR polling GPIO or from EXTI — the module does not care.
 *
 * The PEC11R is a 24-detent, 24-pulse encoder: it passes through a full
 * quadrature cycle between detents and rests at the 00 state. So we emit one
 * step per *full cycle*, not per edge, or the menu scrolls 4x too fast.
 *
 * The table-driven decoder below also rejects bounce implicitly: illegal
 * transitions (both pins changing at once) produce 0, so contact chatter
 * cannot manufacture phantom steps.
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_NONE = 0,
    BTN_PRESS,      /* fired on debounced press */
    BTN_RELEASE,    /* fired on debounced release */
    BTN_CLICK,      /* short press-and-release */
    BTN_LONG_PRESS  /* fired once, while still held */
} btn_event_t;

typedef struct {
    uint8_t  state;       /* last AB pair */
    int8_t   accum;       /* sub-detent accumulator */
    /* button */
    bool     raw;         /* last raw level (true == pressed) */
    bool     stable;      /* debounced level */
    uint32_t edge_ms;     /* when raw last changed */
    uint32_t press_ms;    /* when stable press began */
    bool     long_fired;
} encoder_t;

#define ENC_DEBOUNCE_MS   5u
#define ENC_LONGPRESS_MS  600u

void encoder_init(encoder_t *e, bool a, bool b, bool pressed);

/*
 * Feed the current pin levels. Returns -1, 0 or +1 detents.
 * Call this often — from a 1 kHz timer is plenty for a hand-turned knob.
 */
int encoder_update(encoder_t *e, bool a, bool b);

/* Feed the button level plus the current millisecond counter. */
btn_event_t encoder_button(encoder_t *e, bool pressed, uint32_t now_ms);

#endif /* ENCODER_H */
