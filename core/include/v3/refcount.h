// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_REFCOUNT_H
#define V3_REFCOUNT_H

#include <stdatomic.h>
#include <stdbool.h>

// Race-safe reference counter.
//
// Memory ordering rationale:
//   acquire uses relaxed — a new ref doesn't need to synchronize with anything.
//   release uses acq_rel — the thread that brings count to 0 sees everything
//     any previous ref-holder did (via release of their modifications), and
//     its subsequent cleanup is serialized after them.

typedef struct refcount {
    atomic_uint v;
} refcount_t;

static inline void refcount_init(refcount_t* r, unsigned initial) {
    atomic_init(&r->v, initial);
}

static inline void refcount_acquire(refcount_t* r) {
    atomic_fetch_add_explicit(&r->v, 1, memory_order_relaxed);
}

// Returns true iff this call brought the count to 0 (caller owns destruction).
static inline bool refcount_release(refcount_t* r) {
    unsigned prev = atomic_fetch_sub_explicit(&r->v, 1, memory_order_acq_rel);
    return prev == 1;
}

static inline unsigned refcount_get(refcount_t* r) {
    return atomic_load_explicit(&r->v, memory_order_relaxed);
}

#endif
