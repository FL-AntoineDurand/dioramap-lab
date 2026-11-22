// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-corner-pool.c.
//
// Exercises the pool mutators that maintain the CANONICAL-SOURCE invariant
// (#254-263): v7p_upsert (create + point-merge + super-merge),
// v7p_entry_register_plane (per-plane source append + cache update), and
// v7p_recompute_entry (re-derive cache from source_per_plane[]). After each
// mutation we call v7p_entry_validate — its `assert(prio(e->source) ==
// max prio(source_per_plane[]))` is LIVE under the audit (NDEBUG unset), so
// Eva turns the invariant into a checked assertion: a sure alarm here would
// mean the cache drifted from the canonical store (the exact bug class that
// diverged v7_boundary_merge_test).
//
// We feed concrete, mixed-priority sources (so the max-priority logic is
// genuinely exercised: COPLANAR=0 < CONVEX=1 < POLYGON_MERGE=2 < RI=4) at
// controlled positions (near = merge, far = distinct).
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh v7_corner_pool
//
// See docs/quality/README.md.

#include "v7-corner-pool.h"

#include <stddef.h>
#include <stdint.h>

// Upsert with all optional float[3] inputs disabled — keeps the call site
// inside the contract's `!has_* ` preconditions.
static v7p_entry_t* upsert_plain(v7p_pool_t* pool, double x, double y, double z,
                                 uint8_t source, uint32_t plane_id, double merge_dist) {
    return v7p_upsert(pool, x, y, z, source, /*name=*/NULL,
                      plane_id, /*has_plane_id=*/true,
                      /*has_dirs=*/false, NULL, NULL,
                      /*has_com_bisector=*/false, NULL,
                      V7P_AUTHORITY_FULL,
                      /*has_axis=*/false, NULL,
                      /*has_ridge_id=*/false, 0,
                      merge_dist);
}

int main(void) {
    v7p_pool_t pool = {0};

    // Two distinct corners far apart, different starting sources.
    v7p_entry_t* a = upsert_plain(&pool, 0.0,  0.0,  0.0,  V7P_SRC_CONVEX,             0, 0.5);
    v7p_entry_t* b = upsert_plain(&pool, 10.0, 10.0, 10.0, V7P_SRC_RIDGE_INTERSECTION, 1, 0.5);

    // Register more per-plane sources of mixed priority on `a`: a lower
    // (COPLANAR=0) then a higher (RI=4) — the cache must end at RI.
    if (a) {
        v7p_entry_register_plane(a, 1, (uint8_t)V7P_SRC_COPLANAR);
        v7p_entry_register_plane(a, 2, (uint8_t)V7P_SRC_RIDGE_INTERSECTION);
        v7p_entry_validate(a);   // Eva-checked: prio(a->source) == max prio
    }

    // Register a same-or-lower source on `b`, then recompute, then validate.
    if (b) {
        v7p_entry_register_plane(b, 3, (uint8_t)V7P_SRC_CONCAVE);
        v7p_recompute_entry(b);
        v7p_entry_validate(b);
    }

    // Near-duplicate of `a` → point-merge / super-merge path; the merged
    // entry must still satisfy the invariant.
    v7p_entry_t* c = upsert_plain(&pool, 0.01, 0.0, 0.0, V7P_SRC_POLYGON_MERGE, 4, 0.5);
    if (c) v7p_entry_validate(c);

    v7p_pool_free(&pool);
    return 0;
}
