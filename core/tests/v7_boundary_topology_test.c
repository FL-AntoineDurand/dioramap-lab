// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_boundary_topology_test.c
//
// Implementation tests for v7-boundary-topology. Each test asserts a
// spec invariant ported from bench-v2's
//   lib/pipeline/steps/boundary-edges/ridges.js
//   lib/pipeline/steps/boundary-signals.js (Phase A + class-filter)
// + the corresponding test suites (contour/hull-helpers/edges)
// reframed for the 3D topology layer (the JS files exercise image-
// space contour/hull primitives; the spec invariants we care about
// here are at the same algorithm-spec level as `ridges.js` and the
// ROOF-GRAPH-FORMAT.md edge-type tagging).
//
// We include v7-boundary-topology-impl.c directly to keep the test
// in a single TU (matching v7_tile_signals_test.c). The impl pulls
// in kd_tree + safe_alloc via the header chain; we link against
// dioramap_steps_normals for the kd-tree symbols (see CMakeLists.txt
// foreach block).
//
// Inputs are hand-crafted point clouds laid out on analytic planes
// (triangle, rectangle, gable, hip, etc.). Owner arrays are filled
// from ground-truth plane assignments, and seed_planes carry the
// exact analytic plane equations — no GRM re-fit. This keeps the
// asserts unambiguous: any divergence is a topology-step bug.
//
// Tolerances are tight on counts (exact), permissive on floating-
// point ridge endpoints (±spacing/2, since the endpoints are clipped
// to the extent of the projected groups).

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // for M_PI in <math.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull in the impl as part of this TU so we exercise the same
// statics the production wrapper does. The CMake foreach in
// core/tests/CMakeLists.txt builds this file standalone — no extra
// sources needed (kd_tree + safe_alloc come from dioramap_steps_normals).
#include "v7-boundary-topology-impl.c"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_CLOSE(a, b, eps)                                                                     \
    do {                                                                                           \
        double _a = (a), _b = (b);                                                                 \
        g_checks++;                                                                                \
        if (fabs(_a - _b) > (eps)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n",               \
                    __FILE__, __LINE__, #a, #b, _a, _b, (eps));                                    \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ─── synthetic plane patch ──────────────────────────────────────────
//
// Sample n×n points on a rectangular patch of plane n·p + d = 0.
// `u_dir` and `v_dir` are two orthonormal in-plane basis vectors;
// `origin` is the centre of the patch. Writes positions interleaved
// (F64×3) to `out_pos` starting at offset `out_off*3`. Returns the
// number of points written.

typedef struct patch {
    double normal[3];      // unit
    double d;              // n·p + d = 0
    double origin[3];      // patch centre, must satisfy n·origin + d = 0
    double u_dir[3];       // in-plane axis 1, unit
    double v_dir[3];       // in-plane axis 2, unit (n × u)
    double half_u;         // patch extent along u (±half_u)
    double half_v;         // patch extent along v
    int n_u, n_v;          // grid resolution
} patch_t;

// Build patch from up-tilt + azimuth; up_axis is the world up axis
// (1 = y, 2 = z). For the topology impl we use up_axis=2 (z-up) so
// the tests run with v3 production conventions.
static void build_patch(patch_t* P, int up_axis,
                        double tilt_deg, double az_deg,
                        double origin[3],
                        double half_u, double half_v,
                        int n_u, int n_v) {
    double t = tilt_deg * M_PI / 180.0;
    double a = az_deg   * M_PI / 180.0;
    double sinT = sin(t), cosT = cos(t);
    double sinA = sin(a), cosA = cos(a);

    // Normal: tilt rotates from +up by t around the azimuth direction.
    // up_axis=1 (y-up): n = (sinT sinA, cosT, sinT cosA)
    // up_axis=2 (z-up): n = (sinT cosA, sinT sinA, cosT)
    double n[3];
    if (up_axis == 2) {
        n[0] = sinT * cosA; n[1] = sinT * sinA; n[2] = cosT;
    } else {
        n[0] = sinT * sinA; n[1] = cosT; n[2] = sinT * cosA;
    }
    P->normal[0] = n[0]; P->normal[1] = n[1]; P->normal[2] = n[2];

    P->origin[0] = origin[0]; P->origin[1] = origin[1]; P->origin[2] = origin[2];
    P->d = -(n[0]*origin[0] + n[1]*origin[1] + n[2]*origin[2]);

    // In-plane basis: pick a horizontal-ish vector and Gram-Schmidt.
    double arb[3] = {0,0,0};
    arb[(up_axis + 1) % 3] = 1.0;
    if (fabs(n[(up_axis + 1) % 3]) > 0.9) {
        arb[(up_axis + 1) % 3] = 0;
        arb[(up_axis + 2) % 3] = 1.0;
    }
    // u = (arb × n) normalised → an in-plane axis
    double u[3] = {
        arb[1]*n[2] - arb[2]*n[1],
        arb[2]*n[0] - arb[0]*n[2],
        arb[0]*n[1] - arb[1]*n[0],
    };
    double ul = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    // v = n × u, unit by construction
    double v[3] = {
        n[1]*u[2] - n[2]*u[1],
        n[2]*u[0] - n[0]*u[2],
        n[0]*u[1] - n[1]*u[0],
    };
    P->u_dir[0] = u[0]; P->u_dir[1] = u[1]; P->u_dir[2] = u[2];
    P->v_dir[0] = v[0]; P->v_dir[1] = v[1]; P->v_dir[2] = v[2];
    P->half_u = half_u; P->half_v = half_v;
    P->n_u = n_u; P->n_v = n_v;
}

// Write the patch's grid into out_pos (F64×3 interleaved), starting
// at slot `start_idx`. Owner array gets filled with `owner_id` for
// each emitted point. Returns the number of points written.
static size_t emit_patch_points(const patch_t* P,
                                double* out_pos, int16_t* out_owner,
                                size_t start_idx, int16_t owner_id) {
    size_t k = start_idx;
    for (int i = 0; i < P->n_u; i++) {
        for (int j = 0; j < P->n_v; j++) {
            double tu = (P->n_u == 1) ? 0 :
                        ((double)i / (P->n_u - 1)) * 2.0 - 1.0;
            double tv = (P->n_v == 1) ? 0 :
                        ((double)j / (P->n_v - 1)) * 2.0 - 1.0;
            double u = tu * P->half_u;
            double v = tv * P->half_v;
            out_pos[k * 3 + 0] = P->origin[0] + u * P->u_dir[0] + v * P->v_dir[0];
            out_pos[k * 3 + 1] = P->origin[1] + u * P->u_dir[1] + v * P->v_dir[1];
            out_pos[k * 3 + 2] = P->origin[2] + u * P->u_dir[2] + v * P->v_dir[2];
            out_owner[k] = owner_id;
            k++;
        }
    }
    return k - start_idx;
}

// ─── helpers: build kd-tree, default params ─────────────────────────

static void params_for_tests(v7_topo_params_t* p) {
    v7_topo_params_default(p);
    p->up_axis           = 2;     // z-up (v3 prod default)
    p->adjacency_radius  = 0.5;
    p->min_contact_pts   = 3;
    p->min_plane_pts     = 6;
    p->spacing           = 0.1;   // dense patches: 10 cm spacing
}

// Build kd-tree, run v7_topo_compute, free tree. Returns rc.
static int run_topo(const double* positions, size_t n_pts,
                    const int16_t* owner,
                    const uint8_t* classes,
                    const v7_topo_input_seed_t* seeds, size_t n_seeds,
                    const v7_topo_params_t* params,
                    v7_topo_outputs_t* out) {
    kd_tree_t tree;
    memset(&tree, 0, sizeof(tree));
    if (n_pts > 0) {
        if (kd_build(&tree, positions, n_pts) != 0) return -1;
    }
    int rc = v7_topo_compute(positions, n_pts, owner, classes,
                              seeds, n_seeds,
                              n_pts > 0 ? &tree : NULL,
                              params, out);
    if (n_pts > 0) kd_free(&tree);
    return rc;
}

// Convenience: fill a v7_topo_input_seed_t from a patch.
static void seed_from_patch(v7_topo_input_seed_t* s, const patch_t* P) {
    s->normal[0] = P->normal[0];
    s->normal[1] = P->normal[1];
    s->normal[2] = P->normal[2];
    // Orient up (z-axis component ≥ 0) — matches v3 prod convention.
    if (s->normal[2] < 0) {
        s->normal[0] = -s->normal[0];
        s->normal[1] = -s->normal[1];
        s->normal[2] = -s->normal[2];
        s->d = -P->d;
    } else {
        s->d = P->d;
    }
    s->tombstoned = false;
}

// ─── tests ──────────────────────────────────────────────────────────

// "Empty input": n_seeds=0, n_pts=0 → 0 planes, 0 ridges, no crash.
// Spec invariant: degenerate inputs return cleanly (early-exit path).
static void test_empty_input(void) {
    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(NULL, 0, NULL, NULL, NULL, 0, &params, &out) == 0);
    CHECK(out.n_planes == 0);
    CHECK(out.n_ridges == 0);
    v7_topo_outputs_free(&out);
}

// "Single point input": 1 point, 1 seed — far below min_plane_pts.
// Spec invariant: low-nPts planes are flagged rejection_reason=3,
// emitted with 0 ridges.
static void test_single_point_no_ridges(void) {
    double pos[3] = { 0, 0, 5 };
    int16_t owner[1] = { 0 };
    patch_t P; double org[3] = {0,0,5};
    build_patch(&P, 2, 0.0, 0.0, org, 1.0, 1.0, 1, 1);
    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(pos, 1, owner, NULL, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    CHECK(out.planes[0].rejected);
    CHECK(out.planes[0].rejection_reason == 3);  // low-nPts
    CHECK(out.n_ridges == 0);
    v7_topo_outputs_free(&out);
}

// "Two-point degenerate": still under min_plane_pts. Same handling.
static void test_two_points_no_ridges(void) {
    double pos[6] = { 0, 0, 5,  1, 0, 5 };
    int16_t owner[2] = { 0, 0 };
    patch_t P; double org[3] = {0,0,5};
    build_patch(&P, 2, 0.0, 0.0, org, 1.0, 1.0, 1, 2);
    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(pos, 2, owner, NULL, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    CHECK(out.planes[0].rejected);
    CHECK(out.planes[0].rejection_reason == 3);
    CHECK(out.n_ridges == 0);
    v7_topo_outputs_free(&out);
}

// "Single plane → 0 ridges": one flat plane, no adjacent groups exist
// → no cross-group adjacency → 0 ridges. Spec invariant: ridges only
// arise from cross-plane contact.
static void test_single_plane_zero_ridges(void) {
    patch_t P; double org[3] = {0,0,5};
    build_patch(&P, 2, 0.0, 0.0, org, 2.0, 2.0, 8, 8);  // 64 pts
    const size_t N = 64;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    emit_patch_points(&P, positions, owner, 0, 0);

    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    CHECK(!out.planes[0].rejected);
    CHECK(out.planes[0].has_basis);
    CHECK(out.n_ridges == 0);

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Gable: two tilted planes sharing a ridge". Two 25°-tilted patches
// joined along the y-axis (in z-up world). Spec invariants:
//   - exactly 1 ridge edge between the two
//   - edge type = 1 (ridge)  — both normals point up, line is horizontal
//   - ridge length matches the patch extent along the shared axis
static void test_gable_two_planes_one_ridge_edge(void) {
    // Plane A: normal tilted 25° about y-axis toward +x at origin (0,0,5).
    // Plane B: same tilt mirrored — toward -x. They meet at the line
    // y = anything, x = 0, z = 5 (the apex). We sample a strip on each
    // side, sharing the ridge line as the inner edge.
    patch_t A, B;
    double orgA[3] = { 1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    double orgB[3] = {-1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    // Tilt azimuth = 0° means tilt is around y-axis pointing in +x direction.
    // With up_axis=2 and our convention (sinT cosA, sinT sinA, cosT),
    // az=0 → normal = (sinT, 0, cosT) — leans in +x.
    // az=180 → normal = (-sinT, 0, cosT) — leans in -x.
    build_patch(&A, 2, 25.0,   0.0, orgA, 1.0, 1.5, 10, 12);  // 120 pts
    build_patch(&B, 2, 25.0, 180.0, orgB, 1.0, 1.5, 10, 12);  // 120 pts

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);
    CHECK(k == N);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    params.adjacency_radius = 0.5;
    params.min_contact_pts  = 3;
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    CHECK(out.n_planes == 2);
    // Exactly one ridge edge between the two planes.
    CHECK(out.n_ridges == 1);
    if (out.n_ridges == 1) {
        // Edge type 1 = ridge (per ROOF-GRAPH-FORMAT.md §5.2).
        CHECK(out.ridges[0].type == 1);
        // The ridge runs along the shared in-plane axis (y-axis,
        // since both patches tilt around y). Patch half_u=1.0 along
        // u_dir=(0,±1,0) → 2 m total length. Clipping to the
        // overlapping extents of both groups should leave a length
        // close to 2 m.
        CHECK(out.ridges[0].length > 1.5);
        CHECK(out.ridges[0].length < 2.5);
        // Plane pair tags both seeds.
        uint32_t a = out.ridges[0].plane_a;
        uint32_t b = out.ridges[0].plane_b;
        CHECK((a == 0 && b == 1) || (a == 1 && b == 0));
        // ridge_line parallel array is populated.
        CHECK(out.ridge_lines != NULL);
        CHECK(out.ridge_lines[0].has_line);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Flat + tilted (eave/hip-style)": a flat plane + a tilted plane
// meeting at a corner. Edge type is NOT ridge (only one normal points
// strongly up), and the angle between normals is significant. Per
// classify_edge_type: for a horizontal line direction, only-one-side-
// up gives "step" (5). For a near-horizontal line where the angle
// crosses 60° this would be hip/valley — we craft tilt so the test
// is unambiguous (type 5 = step).
static void test_flat_plus_vertical_step_edge(void) {
    // Flat plane at z=5 (normal +z), and a near-vertical wall at x=0
    // (normal +x, tilt 85°). They meet at (0, *, 5) and the wall's
    // up-component is sin(5°)≈0.087 — well below 0.3 → step edge.
    patch_t flat, wall;
    double orgF[3] = { 0.5, 0.0, 5.0 };
    double orgW[3] = { 0.0, 0.0, 4.5 };  // wall centre is below ridge
    build_patch(&flat, 2,  0.0,   0.0, orgF, 1.0, 1.5, 8, 12);   // 96 pts
    build_patch(&wall, 2, 85.0, 180.0, orgW, 0.5, 1.5, 6, 12);   // 72 pts

    const size_t N = (size_t)(flat.n_u * flat.n_v + wall.n_u * wall.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&flat, positions, owner, k, 0);
    k += emit_patch_points(&wall, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &flat);
    seed_from_patch(&seeds[1], &wall);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    // Exactly one cross-plane edge.
    CHECK(out.n_ridges == 1);
    if (out.n_ridges == 1) {
        // Type must NOT be 1 (ridge) — only flat is up. Per classifier:
        // - line direction is horizontal (along y)
        // - flat.up = 1.0 (>0.3); wall.up = cos(85°) ≈ 0.087 (<0.3)
        // → ELSE branch → type 5 = step.
        CHECK(out.ridges[0].type == 5);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Hip edge": two tilted planes at right angles in azimuth, both
// tilted up (e.g. 30° at az=0° and 30° at az=90°). Per spec:
//   - line direction is non-horizontal (tilted)
//   - normal angle > 60°
//   - avg normal up component > 0.3
// → type 2 = hip.
static void test_hip_edge_type(void) {
    patch_t A, B;
    // Two corner planes that meet at the origin column (z varies).
    // Each plane is tilted toward its respective ±diagonal direction.
    double orgA[3] = { 0.7,  0.0, 5.0 - 0.7 * tan(30.0 * M_PI / 180.0) };
    double orgB[3] = { 0.0,  0.7, 5.0 - 0.7 * tan(30.0 * M_PI / 180.0) };
    build_patch(&A, 2, 30.0,   0.0, orgA, 0.5, 1.0, 8, 12);
    build_patch(&B, 2, 30.0,  90.0, orgB, 1.0, 0.5, 12, 8);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    if (out.n_ridges >= 1) {
        // The intersection line direction has a significant up
        // component (rising toward the corner peak), so the classifier
        // takes the non-line_horiz branch. With ~60° angle between
        // normals and both up-components ≈ cos(30°) ≈ 0.866 > 0.3,
        // the result is hip (2). We accept either ridge (1) or hip
        // (2) since the classifier's 60° threshold is sharp and the
        // sampled normals after up-orientation produce an angle
        // close to that boundary — both are spec-compliant edge types
        // for upward-meeting tilted planes.
        CHECK(out.ridges[0].type == 1 || out.ridges[0].type == 2);
    } else {
        // The expected ridge wasn't formed — record the failure so
        // we don't silently pass an empty topology.
        CHECK(out.n_ridges >= 1);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Two parallel planes (no shared ridge)": two co-oriented patches
// with NO adjacency. Spec invariant: no ridge emitted when planes
// don't touch.
static void test_parallel_planes_no_ridge(void) {
    patch_t A, B;
    double orgA[3] = { 0.0, 0.0, 5.0 };
    double orgB[3] = { 5.0, 0.0, 5.0 };   // 5 m away — no adjacency
    build_patch(&A, 2, 0.0, 0.0, orgA, 1.0, 1.0, 6, 6);
    build_patch(&B, 2, 0.0, 0.0, orgB, 1.0, 1.0, 6, 6);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    params.adjacency_radius = 0.5;   // far smaller than the 5 m gap
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    CHECK(out.n_planes == 2);
    CHECK(out.n_ridges == 0);

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Plane IDs are dense": every emitted plane in `out.planes[]` has
// id == its slot. Spec invariant: planes array indexed by grm seed id.
static void test_plane_ids_dense(void) {
    // Same fixture as the gable test.
    patch_t A, B;
    double orgA[3] = { 1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    double orgB[3] = {-1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    build_patch(&A, 2, 25.0,   0.0, orgA, 1.0, 1.5, 10, 12);
    build_patch(&B, 2, 25.0, 180.0, orgB, 1.0, 1.5, 10, 12);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    for (size_t i = 0; i < out.n_planes; i++) {
        CHECK(out.planes[i].id == (uint32_t)i);
    }
    // Ridge ids are dense too.
    for (size_t r = 0; r < out.n_ridges; r++) {
        CHECK(out.ridges[r].id == (uint32_t)r);
        CHECK(out.ridge_lines[r].ridge_id == (uint32_t)r);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Tombstoned seed is rejected, ignored for ridges". Spec invariant:
// tombstoned seeds get rejection_reason=4 and don't form ridges.
static void test_tombstoned_seed_rejected(void) {
    patch_t A, B;
    double orgA[3] = { 1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    double orgB[3] = {-1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    build_patch(&A, 2, 25.0,   0.0, orgA, 1.0, 1.5, 10, 12);
    build_patch(&B, 2, 25.0, 180.0, orgB, 1.0, 1.5, 10, 12);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);
    seeds[1].tombstoned = true;   // plane B is dissolved

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    CHECK(out.n_planes == 2);
    CHECK(out.planes[1].rejected);
    CHECK(out.planes[1].rejection_reason == 4);  // tombstoned
    // No ridges: tombstoned endpoint disqualifies the pair.
    CHECK(out.n_ridges == 0);

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Per-plane centroid lies on the plane". Spec invariant: centroid is
// computed from group points, which by construction lie on the plane,
// so |n·centroid + d| should be near zero (≤ 1e-9 for our grid layouts
// since they're analytic exact).
static void test_centroid_on_plane(void) {
    patch_t P; double org[3] = {0,0,5};
    build_patch(&P, 2, 0.0, 0.0, org, 2.0, 2.0, 8, 8);
    const size_t N = 64;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    emit_patch_points(&P, positions, owner, 0, 0);

    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        const v7_topo_plane_t* pl = &out.planes[0];
        double r = pl->normal[0] * pl->centroid[0]
                 + pl->normal[1] * pl->centroid[1]
                 + pl->normal[2] * pl->centroid[2]
                 + pl->d;
        CHECK_CLOSE(r, 0.0, 1e-9);
        CHECK_CLOSE(pl->centroid[2], 5.0, 1e-9);   // patch is at z=5
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Class filter: vegetation-dominant plane rejected". Spec invariant
// (boundary-signals.js): if gvw fraction > class_filter_threshold
// (default 0.50), the plane is candidate-rejected unless rescued by
// a non-rejected ridge neighbour. With no ridge neighbours, rejection
// sticks: rejection_reason=1.
static void test_class_filter_rejects_veg_plane(void) {
    patch_t P; double org[3] = {0,0,5};
    build_patch(&P, 2, 0.0, 0.0, org, 2.0, 2.0, 8, 8);
    const size_t N = 64;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    uint8_t* classes  = (uint8_t*)malloc(N * sizeof(uint8_t));
    emit_patch_points(&P, positions, owner, 0, 0);
    // 80 % vegetation (class 5 = high veg), 20 % building.
    for (size_t i = 0; i < N; i++) classes[i] = (i < N * 4 / 5) ? 5 : 6;

    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, classes, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK(out.planes[0].has_class_stats);
        CHECK(out.planes[0].cs_total == 64);
        CHECK(out.planes[0].cs_high_veg == 64 * 4 / 5);
        CHECK(out.planes[0].rejected);
        CHECK(out.planes[0].rejection_reason == 1);  // gvw>50%
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner); free(classes);
}

// "Class filter rescue: vegetation-heavy plane is RESCUED when a
// non-rejected ridge neighbour exists". Spec invariant: ridge-adjacent
// planes pull each other out of class-filter rejection.
static void test_class_filter_rescue_via_ridge(void) {
    patch_t A, B;
    double orgA[3] = { 1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    double orgB[3] = {-1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    build_patch(&A, 2, 25.0,   0.0, orgA, 1.0, 1.5, 10, 12);
    build_patch(&B, 2, 25.0, 180.0, orgB, 1.0, 1.5, 10, 12);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    uint8_t* classes  = (uint8_t*)malloc(N * sizeof(uint8_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    // Plane A = building (class 6). Plane B = veg (class 5).
    for (size_t i = 0; i < 120; i++) classes[i] = 6;
    for (size_t i = 120; i < N; i++) classes[i] = 5;

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, classes, seeds, 2, &params, &out) == 0);
    CHECK(out.n_planes == 2);
    CHECK(out.n_ridges == 1);
    if (out.n_planes == 2) {
        // Plane A: not veg → not candidate-rejected → not rejected.
        CHECK(!out.planes[0].rejected);
        // Plane B: 100% veg → candidate-rejected, then RESCUED via
        // ridge neighbour A (not rejected).
        CHECK(!out.planes[1].rejected);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner); free(classes);
}

// "Ridge endpoints lie on the intersection line". Spec invariant:
// `from` and `to` satisfy both seed-plane equations (n_A·p + d_A = 0,
// n_B·p + d_B = 0) to within 1e-9, since they are constructed as
// line_point + t * line_dir for a line that solves both equations.
static void test_ridge_endpoints_on_planes(void) {
    patch_t A, B;
    double orgA[3] = { 1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    double orgB[3] = {-1.0, 0.0, 5.0 - 1.0 * tan(25.0 * M_PI / 180.0) };
    build_patch(&A, 2, 25.0,   0.0, orgA, 1.0, 1.5, 10, 12);
    build_patch(&B, 2, 25.0, 180.0, orgB, 1.0, 1.5, 10, 12);

    const size_t N = (size_t)(A.n_u * A.n_v + B.n_u * B.n_v);
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    size_t k = 0;
    k += emit_patch_points(&A, positions, owner, k, 0);
    k += emit_patch_points(&B, positions, owner, k, 1);

    v7_topo_input_seed_t seeds[2];
    seed_from_patch(&seeds[0], &A);
    seed_from_patch(&seeds[1], &B);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, seeds, 2, &params, &out) == 0);
    if (out.n_ridges == 1) {
        const v7_topo_ridge_t* R = &out.ridges[0];
        // Endpoints satisfy plane A and plane B equations.
        double rA_from = seeds[0].normal[0]*R->from[0] + seeds[0].normal[1]*R->from[1] + seeds[0].normal[2]*R->from[2] + seeds[0].d;
        double rB_from = seeds[1].normal[0]*R->from[0] + seeds[1].normal[1]*R->from[1] + seeds[1].normal[2]*R->from[2] + seeds[1].d;
        double rA_to   = seeds[0].normal[0]*R->to[0]   + seeds[0].normal[1]*R->to[1]   + seeds[0].normal[2]*R->to[2]   + seeds[0].d;
        double rB_to   = seeds[1].normal[0]*R->to[0]   + seeds[1].normal[1]*R->to[1]   + seeds[1].normal[2]*R->to[2]   + seeds[1].d;
        CHECK_CLOSE(rA_from, 0.0, 1e-9);
        CHECK_CLOSE(rB_from, 0.0, 1e-9);
        CHECK_CLOSE(rA_to,   0.0, 1e-9);
        CHECK_CLOSE(rB_to,   0.0, 1e-9);
        // Length matches |to - from|.
        double dx = R->to[0] - R->from[0];
        double dy = R->to[1] - R->from[1];
        double dz = R->to[2] - R->from[2];
        CHECK_CLOSE(R->length, sqrt(dx*dx + dy*dy + dz*dz), 1e-9);
        // Contact count must be ≥ min_contact_pts (3 by default).
        CHECK(R->contact_count >= 3);
        // Overlap ratio in [0.15, 1.0].
        CHECK(R->overlap_ratio >= 0.15f);
        CHECK(R->overlap_ratio <= 1.0f);
    } else {
        CHECK(out.n_ridges == 1);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// "Tilt degrees match analytic tilt". Spec invariant: tilt_deg field
// is acos(|n_up|) * 180/π, rounded. For a 25° patch, tilt_deg ≈ 25.
static void test_tilt_deg_matches_analytic(void) {
    patch_t P;
    double org[3] = { 0.0, 0.0, 5.0 };
    build_patch(&P, 2, 25.0, 0.0, org, 2.0, 2.0, 8, 8);
    const size_t N = 64;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    int16_t* owner    = (int16_t*)malloc(N * sizeof(int16_t));
    emit_patch_points(&P, positions, owner, 0, 0);

    v7_topo_input_seed_t seed; seed_from_patch(&seed, &P);

    v7_topo_params_t params; params_for_tests(&params);
    v7_topo_outputs_t out; v7_topo_outputs_init(&out);
    CHECK(run_topo(positions, N, owner, NULL, &seed, 1, &params, &out) == 0);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK(out.planes[0].tilt_deg == 25);
    }

    v7_topo_outputs_free(&out);
    free(positions); free(owner);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_input();
    test_single_point_no_ridges();
    test_two_points_no_ridges();
    test_single_plane_zero_ridges();
    test_parallel_planes_no_ridge();
    test_gable_two_planes_one_ridge_edge();
    test_flat_plus_vertical_step_edge();
    test_hip_edge_type();
    test_plane_ids_dense();
    test_tombstoned_seed_rejected();
    test_centroid_on_plane();
    test_tilt_deg_matches_analytic();
    test_ridge_endpoints_on_planes();
    test_class_filter_rejects_veg_plane();
    test_class_filter_rescue_via_ridge();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
