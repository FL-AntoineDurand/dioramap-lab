// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_refcount`.
//
// refcount.h is header-only (static inline atomics). We compile the
// header through a TU that calls each public function. Eva's atomics
// support is limited (the operations show as opaque externs); the goal
// is RTE-level proof only: every signed/unsigned operation is within
// range, no UB, no OOB.
//
// Concurrency semantics (load-acquire / release-store correctness) is
// out of scope for Eva — that's TSan territory and is wired in the
// CMake DIORAMAP_TSAN option.

#include "v3/refcount.h"
#include <__fc_builtin.h>

int main(void) {
    refcount_t r;
    unsigned init = Frama_C_unsigned_int_interval(0, 1000000);
    refcount_init(&r, init);

    // Acquire/release loop with non-deterministic count.
    unsigned n = Frama_C_unsigned_int_interval(0, 8);
    for (unsigned i = 0; i < n; i++) {
        refcount_acquire(&r);
    }
    (void)refcount_get(&r);
    for (unsigned i = 0; i < n; i++) {
        (void)refcount_release(&r);
    }
    (void)refcount_get(&r);

    // Boundary: release on a 1-count ref takes the "last reference"
    // path (returns true). Stand-alone to exercise the == 1 comparison.
    refcount_t r2;
    refcount_init(&r2, 1);
    if (refcount_release(&r2)) {
        // owner-of-destruction branch reached
        (void)0;
    }

    return 0;
}
