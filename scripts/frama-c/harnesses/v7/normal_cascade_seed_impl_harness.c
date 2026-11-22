// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl harness for v7-normal-cascade-seed-impl.c — bypasses the
// bench's IO chain (where Eva widens at the F32→F64 conversion loop
// and never reaches the impl) by driving v7_seed_compute directly with
// `Frama_C_make_unknown` bounded buffers.
//
// Build: not built by CMake; driven by the audit runner:
//   scripts/frama-c/audit-module.sh --key v7_seed_impl \
//     --sources steps/v7/v7-normal-cascade-seed-impl.c \
//     --harness scripts/frama-c/harnesses/v7/normal_cascade_seed_impl_harness.c

#include "v7-normal-cascade-seed-impl.h"

#include <__fc_builtin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Bounded sizes — small enough for Eva to unroll, large enough to
// exercise the impl's loop body.
#define N_PTS    8
#define N_SEGS   4

int main(void) {
    double positions[N_PTS * 3];
    float  curvatures[N_PTS];
    float  point_weights[N_PTS];
    // pt_indices are valid pointers into positions[]; allocate them
    // explicitly so the impl's `(segments + i)->pt_indices + j` reads
    // are provably valid_read. Each segment shares the same index
    // buffer for simplicity — the impl only reads, doesn't compare
    // segments' index ranges.
    uint32_t pt_indices[N_PTS];
    v7_seed_input_segment_t segs[N_SEGS];

    Frama_C_make_unknown((char*)positions,    sizeof(positions));
    Frama_C_make_unknown((char*)curvatures,   sizeof(curvatures));
    Frama_C_make_unknown((char*)point_weights, sizeof(point_weights));
    // Bound pt_indices to valid range over positions[].
    for (size_t i = 0; i < N_PTS; i++) pt_indices[i] = (uint32_t)i;

    for (size_t i = 0; i < N_SEGS; i++) {
        segs[i].source_segment_id = (uint32_t)i;
        segs[i].pt_indices = pt_indices;
        segs[i].n_pts = N_PTS;
    }

    v7_seed_params_t params;
    v7_seed_params_default(&params);
    params.up_axis = 2;  // bench-v2 frame; any 0..2 is valid

    v7_seed_outputs_t out;
    v7_seed_outputs_init(&out);
    (void)v7_seed_compute(positions, curvatures, point_weights,
                          N_PTS, segs, N_SEGS, &params, &out);
    v7_seed_outputs_free(&out);

    // Also exercise the NULL-input paths the docstring permits.
    v7_seed_outputs_init(&out);
    (void)v7_seed_compute(positions, /*curvatures=*/NULL,
                          /*point_weights=*/NULL,
                          N_PTS, segs, N_SEGS, &params, &out);
    v7_seed_outputs_free(&out);

    return 0;
}
