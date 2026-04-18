/*
    PiFmRds - FM/RDS transmitter for the Raspberry Pi
    Copyright (C) 2014 Christophe Jacquet, F8FTK

    ring_spsc.h: header-only, power-of-two, single-producer /
    single-consumer lock-free ring of `float`.

    Correctness model: classic SPSC Lamport ring. The producer writes
    to `head` and reads `tail`; the consumer writes to `tail` and reads
    `head`. Loads of the opposite cursor use acquire semantics and
    stores use release semantics so the compiler cannot reorder the
    payload writes past the head update (and vice-versa).

    On ARMv7+ / AArch64 we use C11 stdatomic.h. On ARMv6 stdatomic is
    available but some toolchains compile it to libatomic calls that
    aren't ideal for a hot loop; fall back to the gcc __sync_* built-ins
    which expand to a DMB on v6/v7 and a bare load/store plus barrier
    on v8.

    Header-only: define RING_SPSC_IMPLEMENTATION in exactly one TU.
    The self-test program (#ifdef RING_SPSC_TEST) is compiled when
    RING_SPSC_TEST is defined (`make ring_spsc_test`).

    Released under the GPLv3.
*/

#ifndef RING_SPSC_H
#define RING_SPSC_H

#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_ARCH) && __ARM_ARCH < 7
#  define RING_SPSC_USE_SYNC_BUILTINS 1
#else
#  define RING_SPSC_USE_SYNC_BUILTINS 0
#  include <stdatomic.h>
#endif

typedef struct ring_spsc {
    float   *buf;
    size_t   cap;      /* power of two */
    size_t   mask;     /* cap - 1 */
#if RING_SPSC_USE_SYNC_BUILTINS
    volatile size_t head;
    volatile size_t tail;
#else
    _Atomic size_t head;
    _Atomic size_t tail;
#endif
} ring_spsc_t;

/* cap must be a power of two. Returns 0 on success, -1 on bad args. */
static inline int ring_spsc_init(ring_spsc_t *r, float *storage, size_t cap) {
    if (r == NULL || storage == NULL || cap == 0 || (cap & (cap - 1)) != 0) return -1;
    r->buf  = storage;
    r->cap  = cap;
    r->mask = cap - 1;
#if RING_SPSC_USE_SYNC_BUILTINS
    r->head = 0;
    r->tail = 0;
    __sync_synchronize();
#else
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
#endif
    return 0;
}

static inline size_t ring_spsc_load_head(const ring_spsc_t *r) {
#if RING_SPSC_USE_SYNC_BUILTINS
    size_t v = r->head;
    __sync_synchronize();
    return v;
#else
    return atomic_load_explicit(&r->head, memory_order_acquire);
#endif
}

static inline size_t ring_spsc_load_tail(const ring_spsc_t *r) {
#if RING_SPSC_USE_SYNC_BUILTINS
    size_t v = r->tail;
    __sync_synchronize();
    return v;
#else
    return atomic_load_explicit(&r->tail, memory_order_acquire);
#endif
}

static inline void ring_spsc_store_head(ring_spsc_t *r, size_t v) {
#if RING_SPSC_USE_SYNC_BUILTINS
    __sync_synchronize();
    r->head = v;
#else
    atomic_store_explicit(&r->head, v, memory_order_release);
#endif
}

static inline void ring_spsc_store_tail(ring_spsc_t *r, size_t v) {
#if RING_SPSC_USE_SYNC_BUILTINS
    __sync_synchronize();
    r->tail = v;
#else
    atomic_store_explicit(&r->tail, v, memory_order_release);
#endif
}

/* Number of elements currently in the ring (consistent snapshot). */
static inline size_t ring_spsc_size(const ring_spsc_t *r) {
    size_t h = ring_spsc_load_head(r);
    size_t t = ring_spsc_load_tail(r);
    return h - t;
}

static inline size_t ring_spsc_free(const ring_spsc_t *r) {
    return r->cap - ring_spsc_size(r);
}

/* Producer side: push up to `n` samples. Returns the actual number
 * written. Never blocks. */
static inline size_t ring_spsc_push(ring_spsc_t *r, const float *src, size_t n) {
    size_t head = ring_spsc_load_head(r);
    size_t tail = ring_spsc_load_tail(r);
    size_t free = r->cap - (head - tail);
    if (n > free) n = free;
    for (size_t i = 0; i < n; i++) {
        r->buf[(head + i) & r->mask] = src[i];
    }
    ring_spsc_store_head(r, head + n);
    return n;
}

/* Consumer side: pop up to `n` samples. Returns the actual number
 * read. Never blocks. */
static inline size_t ring_spsc_pop(ring_spsc_t *r, float *dst, size_t n) {
    size_t head = ring_spsc_load_head(r);
    size_t tail = ring_spsc_load_tail(r);
    size_t avail = head - tail;
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++) {
        dst[i] = r->buf[(tail + i) & r->mask];
    }
    ring_spsc_store_tail(r, tail + n);
    return n;
}

#endif /* RING_SPSC_H */


/* ------------------------------------------------------------------------- *
 * Self-test -- build with `-DRING_SPSC_TEST` on the command line (or via the
 * `ring_spsc_test` Makefile target). Pure in-process, no threads; verifies
 * wrap-around, short push/pop, and full/empty transitions.
 * ------------------------------------------------------------------------- */
#ifdef RING_SPSC_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    enum { CAP = 8 };
    float storage[CAP];
    ring_spsc_t r;
    if (ring_spsc_init(&r, storage, CAP) != 0) {
        fprintf(stderr, "init failed\n"); return 1;
    }
    if (ring_spsc_size(&r) != 0 || ring_spsc_free(&r) != CAP) {
        fprintf(stderr, "fresh ring should be empty\n"); return 1;
    }

    /* Push 5, pop 3, push 6 (wraps), pop 8 -- ensure bytes match. */
    float input[] = { 1,2,3,4,5, 6,7,8,9,10,11 };
    if (ring_spsc_push(&r, input, 5) != 5) { fprintf(stderr, "push5\n"); return 1; }

    float out[16] = {0};
    if (ring_spsc_pop(&r, out, 3) != 3) { fprintf(stderr, "pop3\n"); return 1; }
    if (out[0] != 1 || out[1] != 2 || out[2] != 3) {
        fprintf(stderr, "bad pop3: %f %f %f\n", out[0], out[1], out[2]); return 1;
    }

    if (ring_spsc_push(&r, input + 5, 6) != 6) { fprintf(stderr, "push6 (wrap)\n"); return 1; }
    if (ring_spsc_size(&r) != 8) { fprintf(stderr, "should be full\n"); return 1; }

    memset(out, 0, sizeof(out));
    if (ring_spsc_pop(&r, out, 8) != 8) { fprintf(stderr, "pop8\n"); return 1; }
    const float expected[] = {4,5,6,7,8,9,10,11};
    for (int i = 0; i < 8; i++) {
        if (out[i] != expected[i]) {
            fprintf(stderr, "pos %d: got %f want %f\n", i, out[i], expected[i]);
            return 1;
        }
    }

    /* Overflow: push more than capacity returns the clamped count. */
    if (ring_spsc_push(&r, input, 11) != 8) { fprintf(stderr, "overflow clamp\n"); return 1; }
    /* Underflow: pop more than available returns the clamped count. */
    ring_spsc_pop(&r, out, 16);
    if (ring_spsc_pop(&r, out, 1) != 0) { fprintf(stderr, "underflow clamp\n"); return 1; }

    printf("ring_spsc self-test passed\n");
    return 0;
}
#endif
