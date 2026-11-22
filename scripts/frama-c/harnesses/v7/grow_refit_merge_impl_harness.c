// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl harness for v7-grow-refit-merge-impl.c. Same pattern as the
// normal-cascade-seed harness: bypass the bench IO by injecting bounded
// abstract buffers, drive v7_grm_compute directly.

#include "v7-grow-refit-merge-impl.h"
#include "kd_tree.h"

#include <__fc_builtin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define N_PTS         8
#define N_SEEDS       3
#define N_POOL        8
#define N_SEED_IDX    4   // exact n_seeds per seed (bounded for EVA)

int main(void) {
    double positions[N_PTS * 3];
    float  point_weights[N_PTS];
    uint16_t psids[N_PTS];
    uint32_t pool_indices[N_POOL];
    v7_grm_input_seed_t seeds[N_SEEDS];
    // Backing buffer for seeds[].seed_indices. The impl iterates
    // `for (i < seeds[pi].n_seeds) seeds[pi].seed_indices[i]`; without
    // a wired buffer + a DEFINITE n_seeds, leaving the pointer field
    // Unknown produces a sure alarm at v7-grow-refit-merge-impl.c:436
    // (\valid_read on seed_indices+i).
    uint32_t seed_idx_buf[N_SEEDS][N_SEED_IDX];

    Frama_C_make_unknown((char*)positions,    sizeof(positions));
    Frama_C_make_unknown((char*)point_weights, sizeof(point_weights));
    Frama_C_make_unknown((char*)psids,        sizeof(psids));
    Frama_C_make_unknown((char*)pool_indices, sizeof(pool_indices));
    Frama_C_make_unknown((char*)seed_idx_buf, sizeof(seed_idx_buf));

    // Bound pool_indices to valid range so the impl's [pool_indices[i]] reads
    // into positions stay in bounds.
    for (size_t i = 0; i < N_POOL; i++) {
        if (pool_indices[i] >= N_PTS) pool_indices[i] = (uint32_t)(i % N_PTS);
    }

    // Initialise `seeds[]` field-by-field. Don't `make_unknown` the whole
    // struct then wire pointers afterwards: Eva loses precision on the
    // pointer-field assignment when the surrounding array was marked
    // Unknown (we observed the wire-up not propagating into v7_grm_compute,
    // leaving the sure alarm in place). Instead, only the unbounded numeric
    // fields go Unknown; pointer + count fields are DEFINITE.
    for (size_t pi = 0; pi < N_SEEDS; pi++) {
        Frama_C_make_unknown((char*)seeds[pi].normal, sizeof(seeds[pi].normal));
        Frama_C_make_unknown((char*)&seeds[pi].d, sizeof(seeds[pi].d));
        Frama_C_make_unknown((char*)&seeds[pi].is_ghost, sizeof(seeds[pi].is_ghost));
        Frama_C_make_unknown((char*)&seeds[pi].ghost_void_id, sizeof(seeds[pi].ghost_void_id));
        Frama_C_make_unknown((char*)&seeds[pi].seed_tilt, sizeof(seeds[pi].seed_tilt));
        seeds[pi].seed_indices = seed_idx_buf[pi];
        seeds[pi].n_seeds      = N_SEED_IDX;     // exact, not Unknown-then-clamp
        // Clamp seed_indices values to the pool range (impl uses them as
        // LOCAL pool indices, e.g. `pool_indices[seed_indices[i]]`-style).
        for (size_t i = 0; i < N_SEED_IDX; i++) {
            if (seed_idx_buf[pi][i] >= N_POOL) {
                seed_idx_buf[pi][i] = (uint32_t)(i % N_POOL);
            }
        }
    }

    kd_tree_t tree;
    if (kd_build(&tree, positions, N_PTS) != 0) return 0;

    v7_grm_params_t params;
    v7_grm_params_default(&params);
    params.up_axis = 2;

    v7_grm_outputs_t out;
    v7_grm_outputs_init(&out);
    (void)v7_grm_compute(positions, point_weights, psids,
                         N_PTS, pool_indices, N_POOL, &tree,
                         seeds, N_SEEDS, &params, &out);
    v7_grm_outputs_free(&out);

    // NULL-input paths.
    v7_grm_outputs_init(&out);
    (void)v7_grm_compute(positions, /*point_weights=*/NULL,
                         /*psids=*/NULL,
                         N_PTS, pool_indices, N_POOL, &tree,
                         seeds, N_SEEDS, &params, &out);
    v7_grm_outputs_free(&out);

    kd_free(&tree);
    return 0;
}
