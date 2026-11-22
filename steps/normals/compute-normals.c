// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-normals
// Reads:  lidar-positions — F64 interleaved (x, y, z) in Lambert-93 metres.
// Params: {"k": int (default 16), "min_neighbors": int (default 3)}
// Writes: lidar-normals — F32 interleaved (nx, ny, nz), length 3*N.
//         lidar-spacing — F32 per-point local spacing in metres
//                         (median k-NN distance, length N).
//                         Used by the disc renderer to scale per-point hex
//                         instances so coverage stays roughly uniform across
//                         density variations.
//
// Per-point surface normal via k-NN PCA. For each point p:
//   1. kd-tree k-nearest-neighbours query against all points.
//   2. Build the 3×3 covariance matrix of (neighbours - centroid).
//   3. Smallest-eigenvalue eigenvector via Cardano + null-space — that's
//      the surface normal direction.
//   4. Flip if z < 0 so all normals point upward (v3 uses L93: +z is up,
//      vs bench-v2 which used +y).
// Spacing per point = sqrt(dist_sq of the floor(k/2)-th neighbour) — matches
// the legacy SDK's lidar-3d-environment-viewer/src/geometry/compute-normals.js
// definition (median k-NN distance is robust to outliers vs nearest-neighbour
// distance). Points without enough neighbours get default normal (0,0,1)
// and default spacing = 1.0 m.
//
// Algorithm matches bench-v2 lib/pipeline/steps/compute-normals.js exactly.
// kd-tree + PCA implementations live in steps/normals/{kd_tree,pca}.{h,c}.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/step.h"

#include "kd_tree.h"
#include "normals-steps.h"
#include "pca.h"

static int parse_params(const char* params_str, int* k_out, int* min_neighbors_out) {
    *k_out = 16;
    *min_neighbors_out = 3;
    if (!params_str) {
        return 0;
    }
    json_error_t je;
    json_t* params = json_loads(params_str, 0, &je);
    if (!params) {
        return -1;
    }
    json_t* j_k = json_object_get(params, "k");
    if (json_int_or_default(j_k, 1, 1024, "k", k_out) != 0) {
        json_decref(params); return -1;
    }
    json_t* j_min = json_object_get(params, "min_neighbors");
    if (json_int_or_default(j_min, 1, 1024, "min_neighbors", min_neighbors_out) != 0) {
        json_decref(params); return -1;
    }
    json_decref(params);
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) {
        return -1;
    }
    size_t N = pos_bytes / (3 * sizeof(double));

    int k = 16;
    int min_neighbors = 3;
    if (parse_params(step_get_params(ctx), &k, &min_neighbors) != 0) {
        return -1;
    }

    // Allocate output up-front; fill incrementally. Use ownership-transfer
    // step_write_output_owned at the end so the buffers go straight to the
    // store without an extra copy.
    // Frama-C audit finding NORM-2: bound 3*N*sizeof(float) before malloc to
    // avoid size_t wrap on absurd N. SIZE_MAX / 12 is the per-axis threshold
    // (3 floats per point × 4 bytes per float = 12 bytes per point).
    if (N > SIZE_MAX / (3 * sizeof(float))) {
        return -1;
    }
    float* normals = malloc(3 * N * sizeof(float));
    float* spacing = safe_malloc_array(N, sizeof(float));
    if ((!normals || !spacing) && N > 0) {
        free(normals);
        free(spacing);
        return -1;
    }

    // Edge case: not enough points to form even one neighbourhood. Default
    // every normal to upward + spacing to 1.0 m and exit cleanly.
    if (N < (size_t)min_neighbors) {
        for (size_t i = 0; i < N; i++) {
            normals[i * 3 + 0] = 0.0f;
            normals[i * 3 + 1] = 0.0f;
            normals[i * 3 + 2] = 1.0f;
            spacing[i] = 1.0f;
        }
        if (step_write_output_owned(ctx, "lidar-normals", (uint8_t*)normals,
                                     3 * N * sizeof(float)) != 0) {
            free(spacing);
            return -1;
        }
        return step_write_output_owned(ctx, "lidar-spacing", (uint8_t*)spacing,
                                       N * sizeof(float));
    }

    kd_tree_t tree;
    if (kd_build(&tree, positions, N) != 0) {
        free(normals);
        free(spacing);
        return -1;
    }

    kd_neighbor_t* neighbors = malloc((size_t)k * sizeof(kd_neighbor_t));
    if (!neighbors) {
        kd_free(&tree);
        free(normals);
        free(spacing);
        return -1;
    }

    // Median index in the sorted-ascending neighbour list. Matches the
    // legacy SDK's `Math.floor(k / 2)`.
    const int median_idx = k / 2;

    for (size_t i = 0; i < N; i++) {
        double q[3] = {positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]};
        int n_nbrs = 0;
        if (kd_knn(&tree, q, k, neighbors, &n_nbrs) != 0 || n_nbrs < min_neighbors) {
            normals[i * 3 + 0] = 0.0f;
            normals[i * 3 + 1] = 0.0f;
            normals[i * 3 + 2] = 1.0f;
            spacing[i] = 1.0f;
            continue;
        }

        // Spacing — sqrt(dist_sq of the median neighbour). kd_knn returns
        // ascending by dist_sq, so neighbors[0] is the query point itself
        // (dist_sq = 0 since the tree contains the same positions); the
        // median is over real neighbours. Clamp the index to n_nbrs - 1
        // for cases where fewer than k neighbours were returned.
        int idx = median_idx < n_nbrs ? median_idx : n_nbrs - 1;
        spacing[i] = (float)sqrt(neighbors[idx].dist_sq);

        // Centroid + covariance over neighbours. Match bench-v2: sum then
        // divide by n at the end (one division pass), no Welford.
        double cx = 0, cy = 0, cz = 0;
        for (int j = 0; j < n_nbrs; j++) {
            const double* p = &positions[(size_t)neighbors[j].idx * 3];
            cx += p[0];
            cy += p[1];
            cz += p[2];
        }
        cx /= n_nbrs;
        cy /= n_nbrs;
        cz /= n_nbrs;

        double c00 = 0, c01 = 0, c02 = 0, c11 = 0, c12 = 0, c22 = 0;
        for (int j = 0; j < n_nbrs; j++) {
            const double* p = &positions[(size_t)neighbors[j].idx * 3];
            double dx = p[0] - cx;
            double dy = p[1] - cy;
            double dz = p[2] - cz;
            c00 += dx * dx;
            c01 += dx * dy;
            c02 += dx * dz;
            c11 += dy * dy;
            c12 += dy * dz;
            c22 += dz * dz;
        }
        double cov[9] = {
            c00 / n_nbrs, c01 / n_nbrs, c02 / n_nbrs,
            c01 / n_nbrs, c11 / n_nbrs, c12 / n_nbrs,
            c02 / n_nbrs, c12 / n_nbrs, c22 / n_nbrs,
        };

        double normal[3];
        if (pca_smallest_eigenvector(cov, normal) != 0) {
            normals[i * 3 + 0] = 0.0f;
            normals[i * 3 + 1] = 0.0f;
            normals[i * 3 + 2] = 1.0f;
            continue;
        }
        // Flip if z < 0 so the normal points up (v3 uses L93: +z is up).
        if (normal[2] < 0.0) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        normals[i * 3 + 0] = (float)normal[0];
        normals[i * 3 + 1] = (float)normal[1];
        normals[i * 3 + 2] = (float)normal[2];
    }

    free(neighbors);
    kd_free(&tree);

    if (step_write_output_owned(ctx, "lidar-normals", (uint8_t*)normals,
                                 3 * N * sizeof(float)) != 0) {
        free(spacing);
        return -1;
    }
    return step_write_output_owned(ctx, "lidar-spacing", (uint8_t*)spacing,
                                   N * sizeof(float));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
};
static const step_write_t writes_arr[] = {
    // F32 interleaved (nx, ny, nz). Same artifact kind as a generic point-column;
    // dtype is implicit per the convention used by laz-extract-disc's writes.
    {.name = "lidar-normals", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
    // F32 per-point spacing (median k-NN distance, metres). Used by the
    // disc renderer in the SDK's point-cloud composer to scale per-point
    // hex instances. Visibility public so the SDK can subscribe.
    {.name = "lidar-spacing", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t compute_normals_def = {
    .name = "compute-normals",
    // v2: added lidar-spacing write (median k-NN distance). lidar-normals
    // bytes are unchanged from v1; the version bump is required because
    // the step's identity-hash inputs (writes set) changed.
    .version = 2,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 2,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
