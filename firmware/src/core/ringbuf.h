/*
 * ringbuf — single-producer / single-consumer lock-free byte ring buffer.
 *
 * This is THE most important data structure in a DAP. The SD card reader
 * (producer, running in the main loop) and the I2S DMA refill (consumer,
 * running in an interrupt) must hand samples to each other without ever
 * taking a lock, because blocking the audio path for even a few hundred
 * microseconds is an audible click.
 *
 * Correctness rules that make this lock-free on Cortex-M:
 *   - Exactly one thread/context writes `head`; exactly one writes `tail`.
 *   - Capacity is a power of two so the wrap is a mask, not a modulo.
 *   - One slot is always left empty so full != empty is unambiguous.
 *   - head/tail are volatile so the compiler cannot cache them across the
 *     interrupt boundary. On M7 you also want a DMB between writing the
 *     data and publishing the index; see RB_PUBLISH_BARRIER below.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RB_PUBLISH_BARRIER
/* On the host this is a compiler barrier. In the STM32 port, define this to
 * __DMB() before including, so the data write is visible before the index. */
#define RB_PUBLISH_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

typedef struct {
    uint8_t *buf;
    size_t   cap;  /* power of two */
    size_t   mask; /* cap - 1 */
    volatile size_t head; /* written by producer */
    volatile size_t tail; /* written by consumer */
} ringbuf_t;

/* cap must be a power of two and >= 2. Returns false otherwise. */
bool   rb_init(ringbuf_t *rb, uint8_t *storage, size_t cap);
void   rb_reset(ringbuf_t *rb);
size_t rb_used(const ringbuf_t *rb);
size_t rb_free(const ringbuf_t *rb);
bool   rb_is_empty(const ringbuf_t *rb);
bool   rb_is_full(const ringbuf_t *rb);

/* Copy up to n bytes in. Returns how many were actually written. */
size_t rb_write(ringbuf_t *rb, const uint8_t *src, size_t n);
/* Copy up to n bytes out. Returns how many were actually read. */
size_t rb_read(ringbuf_t *rb, uint8_t *dst, size_t n);
/* Read without consuming. */
size_t rb_peek(const ringbuf_t *rb, uint8_t *dst, size_t n);
/* Drop up to n bytes without copying. Returns how many were dropped. */
size_t rb_discard(ringbuf_t *rb, size_t n);

/*
 * Zero-copy producer path, for DMA-from-SD straight into the ring:
 *   uint8_t *p; size_t n = rb_write_ptr(&rb, &p);   // contiguous run
 *   ... fill p[0..n) ...
 *   rb_write_commit(&rb, n);
 */
size_t rb_write_ptr(ringbuf_t *rb, uint8_t **out);
void   rb_write_commit(ringbuf_t *rb, size_t n);
/* Zero-copy consumer path, e.g. handing a contiguous run to the I2S DMA. */
size_t rb_read_ptr(const ringbuf_t *rb, const uint8_t **out);
void   rb_read_commit(ringbuf_t *rb, size_t n);

#endif /* RINGBUF_H */
