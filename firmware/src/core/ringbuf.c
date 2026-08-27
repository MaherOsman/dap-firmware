#include "ringbuf.h"
#include <string.h>

static bool is_pow2(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

bool rb_init(ringbuf_t *rb, uint8_t *storage, size_t cap)
{
    if (!rb || !storage || !is_pow2(cap)) {
        return false;
    }
    rb->buf  = storage;
    rb->cap  = cap;
    rb->mask = cap - 1;
    rb->head = 0;
    rb->tail = 0;
    return true;
}

void rb_reset(ringbuf_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
}

size_t rb_used(const ringbuf_t *rb)
{
    return (rb->head - rb->tail) & rb->mask;
}

size_t rb_free(const ringbuf_t *rb)
{
    /* One slot reserved so head==tail always means empty. */
    return rb->mask - rb_used(rb);
}

bool rb_is_empty(const ringbuf_t *rb) { return rb->head == rb->tail; }
bool rb_is_full(const ringbuf_t *rb) { return rb_free(rb) == 0; }

size_t rb_write(ringbuf_t *rb, const uint8_t *src, size_t n)
{
    size_t space = rb_free(rb);
    if (n > space) {
        n = space;
    }
    size_t head  = rb->head;
    size_t first = rb->cap - head;
    if (first > n) {
        first = n;
    }
    memcpy(rb->buf + head, src, first);
    memcpy(rb->buf, src + first, n - first);
    RB_PUBLISH_BARRIER();
    rb->head = (head + n) & rb->mask;
    return n;
}

size_t rb_peek(const ringbuf_t *rb, uint8_t *dst, size_t n)
{
    size_t avail = rb_used(rb);
    if (n > avail) {
        n = avail;
    }
    size_t tail  = rb->tail;
    size_t first = rb->cap - tail;
    if (first > n) {
        first = n;
    }
    memcpy(dst, rb->buf + tail, first);
    memcpy(dst + first, rb->buf, n - first);
    return n;
}

size_t rb_discard(ringbuf_t *rb, size_t n)
{
    size_t avail = rb_used(rb);
    if (n > avail) {
        n = avail;
    }
    RB_PUBLISH_BARRIER();
    rb->tail = (rb->tail + n) & rb->mask;
    return n;
}

size_t rb_read(ringbuf_t *rb, uint8_t *dst, size_t n)
{
    size_t got = rb_peek(rb, dst, n);
    rb_discard(rb, got);
    return got;
}

size_t rb_write_ptr(ringbuf_t *rb, uint8_t **out)
{
    size_t head = rb->head;
    size_t run  = rb->cap - head;       /* to end of storage */
    size_t space = rb_free(rb);
    if (run > space) {
        run = space;
    }
    *out = rb->buf + head;
    return run;
}

void rb_write_commit(ringbuf_t *rb, size_t n)
{
    RB_PUBLISH_BARRIER();
    rb->head = (rb->head + n) & rb->mask;
}

size_t rb_read_ptr(const ringbuf_t *rb, const uint8_t **out)
{
    size_t tail = rb->tail;
    size_t run  = rb->cap - tail;
    size_t avail = rb_used(rb);
    if (run > avail) {
        run = avail;
    }
    *out = rb->buf + tail;
    return run;
}

void rb_read_commit(ringbuf_t *rb, size_t n)
{
    RB_PUBLISH_BARRIER();
    rb->tail = (rb->tail + n) & rb->mask;
}
