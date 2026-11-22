// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-polygons-2 — implementation.
//
// See v7-boundary-polygons-2-impl.h for the algorithm overview.
// Each function below is independently unit-testable; the test binary
// v7-bp2-test.c drives them from synthetic fixtures.

#define _USE_MATH_DEFINES
#define _POSIX_C_SOURCE 200809L

#include "v7-boundary-polygons-2-impl.h"
#include "v7-polygon-primitives.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Tunable defaults (definitions for the extern declarations) ──────

int    V7_BP2_FULL_ENUM_MAX         = 8;
int    V7_BP2_K_RANDOM              = 5;
double V7_BP2_WEIGHT_WSD            = 0.8;
double V7_BP2_WEIGHT_COMPACTNESS    = 0.1;
double V7_BP2_WEIGHT_SPIKE          = 0.1;
double V7_BP2_SPIKE_THRESHOLD_DEG   = 30.0;
double V7_BP2_CORNER_TO_FRAGMENT_RANGE_M = 0.75;  // bp1's anchor-to-contour threshold
int    V7_BP2_OROPT_WINDOW          = 2;     // windowed or-opt reinsertion radius (ordering refinement)

void v7_bp2_params_default(v7_bp2_params_t* p) {
    if (!p) return;
    p->full_enum_max              = 8;
    p->k_random                   = 5;
    p->weight_wsd                 = 0.8;
    p->weight_compactness         = 0.1;
    p->weight_spike               = 0.1;
    p->spike_threshold_deg        = 30.0;
    p->corner_to_fragment_range_m = 0.75;
    p->oropt_window               = 2;
    p->wall_tilt_min_deg          = 72.0;
    p->up_axis                    = 2;
}

void v7_bp2_params_apply(const v7_bp2_params_t* p) {
    if (!p) return;
    V7_BP2_FULL_ENUM_MAX             = p->full_enum_max;
    V7_BP2_K_RANDOM                  = p->k_random;
    V7_BP2_WEIGHT_WSD                = p->weight_wsd;
    V7_BP2_WEIGHT_COMPACTNESS        = p->weight_compactness;
    V7_BP2_WEIGHT_SPIKE              = p->weight_spike;
    V7_BP2_SPIKE_THRESHOLD_DEG       = p->spike_threshold_deg;
    V7_BP2_CORNER_TO_FRAGMENT_RANGE_M = p->corner_to_fragment_range_m;
    V7_BP2_OROPT_WINDOW              = p->oropt_window;
    // wall_tilt_min_deg and up_axis are consumed by the step wrapper,
    // not by compute() — left as fields here for completeness.
}

// ── Geometric primitives ────────────────────────────────────────────
//
// pt_in_poly / pt_edge_dist are in v7-polygon-primitives.{h,c} —
// shared with v7-boundary-polygons (bp1). v7_pt_in_poly and
// v7_pt_edge_dist are thin renames retained only for the test
// harness, which references the bp2_ names. New code must call
// v7_pt_in_poly / v7_pt_edge_dist directly.

// Thin renames retained because v7-bp2-test.c references the bp2_ names
// from the public header. Internal callers use v7_pt_in_poly directly.
bool v7_bp2_pt_in_poly(double pu, double pv,
                        const double* polyU, const double* polyV, int n) {
    return v7_pt_in_poly(pu, pv, polyU, polyV, n);
}
double v7_bp2_pt_edge_dist(double pu, double pv,
                            const double* polyU, const double* polyV, int n) {
    return v7_pt_edge_dist(pu, pv, polyU, polyV, n);
}

double v7_bp2_polygon_area_uv(const double* polyU, const double* polyV, int n) {
    double a = 0;
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        a += polyU[k] * polyV[k1] - polyU[k1] * polyV[k];
    }
    return fabs(a) * 0.5;
}

double v7_bp2_polygon_perimeter_uv(const double* polyU, const double* polyV, int n) {
    double p = 0;
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        double du = polyU[k1] - polyU[k];
        double dv = polyV[k1] - polyV[k];
        p += sqrt(du * du + dv * dv);
    }
    return p;
}

// Compactness = perimeter² / (4π · area). 1.0 for a circle; larger for
// thinner / less compact shapes. Returns +INFINITY for degenerate area.
double v7_bp2_polygon_compactness_uv(const double* polyU, const double* polyV, int n) {
    double a = v7_bp2_polygon_area_uv(polyU, polyV, n);
    if (a < 1e-12) return INFINITY;
    double p = v7_bp2_polygon_perimeter_uv(polyU, polyV, n);
    return (p * p) / (4.0 * M_PI * a);
}

// Standard 2D segment-segment intersection test. Returns true iff the
// closed segments [a-b] and [c-d] share at least one point AND the
// shared point is in the strict interior of both segments (endpoint
// touches don't count, otherwise neighbouring polygon edges would
// always intersect at the shared vertex).
static bool seg_seg_strict_intersect(double ax, double ay, double bx, double by,
                                       double cx, double cy, double dx, double dy) {
    // Use parameterized form. Solve a + t(b-a) = c + s(d-c) for t, s.
    double rx = bx - ax, ry = by - ay;
    double sx = dx - cx, sy = dy - cy;
    double denom = rx * sy - ry * sx;
    if (fabs(denom) < 1e-15) return false; // parallel — treat as no strict crossing
    double t = ((cx - ax) * sy - (cy - ay) * sx) / denom;
    double u = ((cx - ax) * ry - (cy - ay) * rx) / denom;
    const double EPS = 1e-9;
    return (t > EPS && t < 1.0 - EPS && u > EPS && u < 1.0 - EPS);
}

bool v7_bp2_polygon_self_intersects_uv(const double* polyU, const double* polyV, int n) {
    if (n < 4) return false;
    for (int i = 0; i < n; i++) {
        int i1 = (i + 1) % n;
        for (int j = i + 2; j < n; j++) {
            int j1 = (j + 1) % n;
            // Skip the wrap-around adjacency.
            if (i == 0 && j1 == 0) continue;
            if (seg_seg_strict_intersect(polyU[i], polyV[i], polyU[i1], polyV[i1],
                                          polyU[j], polyV[j], polyU[j1], polyV[j1])) {
                return true;
            }
        }
    }
    return false;
}

// Local O(n) self-intersection test around a single moved/inserted vertex.
//
// Tests only the two ring edges incident to `at_idx` — (prev→at) and
// (at→next), cyclic — against every other ring edge, skipping the edges
// ADJACENT to each (those that share a vertex with the incident edge, which
// always "touch" at the shared endpoint and must not count as crossings).
// Reuses seg_seg_strict_intersect for the crossing math: that primitive
// already rejects shared-endpoint touches (strict interior of BOTH
// segments), but we still skip adjacent edges explicitly so a near-collinear
// shared vertex can't tip into a false strict crossing.
//
// This is the O(n) counterpart of the whole-ring O(n²)
// v7_bp2_polygon_self_intersects_uv: when the only edges that changed are the
// two incident to `at_idx`, any NEW self-intersection must involve one of
// those two edges, so testing them against the rest is sufficient.
//
// Returns false for degenerate rings (n < 4 — a triangle's every edge pair is
// adjacent, so no strict crossing is possible) and for out-of-range at_idx.
bool v7_local_edges_cross_uv(const double* U, const double* V, int n, int at_idx) {
    if (n < 4) return false;
    if (at_idx < 0 || at_idx >= n) return false;

    int prev = (at_idx + n - 1) % n;

    // The two incident edges, each as (start vertex index → its successor):
    //   E_prev : prev → at_idx
    //   E_next : at_idx → next
    int inc[2] = { prev, at_idx };

    for (int e = 0; e < 2; e++) {
        int a = inc[e];          // edge start vertex
        int b = (a + 1) % n;     // edge end vertex (== at_idx for E_prev, next for E_next)
        for (int j = 0; j < n; j++) {
            int j1 = (j + 1) % n;
            // Skip the incident edge itself and any edge sharing a vertex with
            // it (adjacency): a shared endpoint is not a crossing.
            if (j == a || j == b || j1 == a || j1 == b) continue;
            if (seg_seg_strict_intersect(U[a], V[a], U[b], V[b],
                                          U[j], V[j], U[j1], V[j1])) {
                return true;
            }
        }
    }
    return false;
}

double v7_bp2_spike_penalty_uv(const double* polyU, const double* polyV, int n,
                                double spike_threshold_deg) {
    if (n < 3) return 0;
    double threshold_rad = spike_threshold_deg * M_PI / 180.0;
    double total = 0;
    for (int i = 0; i < n; i++) {
        int im = (i + n - 1) % n;
        int ip = (i + 1) % n;
        double aU = polyU[im] - polyU[i];
        double aV = polyV[im] - polyV[i];
        double bU = polyU[ip] - polyU[i];
        double bV = polyV[ip] - polyV[i];
        double la = sqrt(aU * aU + aV * aV);
        double lb = sqrt(bU * bU + bV * bV);
        if (la < 1e-12 || lb < 1e-12) continue;
        double cos_angle = (aU * bU + aV * bV) / (la * lb);
        if (cos_angle > 1.0) cos_angle = 1.0;
        if (cos_angle < -1.0) cos_angle = -1.0;
        double interior = acos(cos_angle);    // in [0, π]
        // SPIKE = interior near 0 (folded-back). FLAT = interior near π
        // (collinear). We ONLY penalise spike — flat is legitimate.
        if (interior < threshold_rad) {
            double d = threshold_rad - interior;
            total += d * d;
        }
    }
    return total;
}

// ── WSD multi-polygon ───────────────────────────────────────────────

// Helper: project anchor cycle to UV arrays.
static void project_cycle_uv(const v7_bp2_anchor_t* anchors,
                              const uint32_t* cycle_idx, int n_cycle,
                              const v7_bp2_basis_t* basis,
                              double* outU, double* outV) {
    for (int k = 0; k < n_cycle; k++) {
        const v7_bp2_anchor_t* a = &anchors[cycle_idx[k]];
        v7_bp2_project_uv(a->point, basis, &outU[k], &outV[k]);
    }
}

double v7_bp2_wsd_multi(const double* group_pts, size_t n_group, const float* group_w,
                        const double* nearby_pts, size_t n_nearby, const float* nearby_w,
                        const v7_bp2_basis_t* basis,
                        const double* outerU, const double* outerV, int n_outer,
                        const double* const* holesU, const double* const* holesV,
                        const int* holes_n, int n_holes,
                        bool include_foreign_in) {
    if (n_outer < 3) return INFINITY;
    double wsd = 0;
    // Group points: correct iff inside outer AND outside every hole.
    for (size_t i = 0; i < n_group; i++) {
        double pu, pv;
        v7_bp2_project_uv(&group_pts[i*3], basis, &pu, &pv);
        bool in_outer = v7_pt_in_poly(pu, pv, outerU, outerV, n_outer);
        bool in_any_hole = false;
        for (int h = 0; h < n_holes; h++) {
            if (v7_pt_in_poly(pu, pv, holesU[h], holesV[h], holes_n[h])) {
                in_any_hole = true; break;
            }
        }
        bool correct = in_outer && !in_any_hole;
        if (!correct) {
            double w = group_w ? group_w[i] : 1.0;
            if (w <= 0) continue;
            // Distance to the polygon edge that "should" contain this point.
            // For now, distance to the outer boundary (close-enough proxy).
            double d_outer = v7_pt_edge_dist(pu, pv, outerU, outerV, n_outer);
            double d_min = d_outer;
            for (int h = 0; h < n_holes; h++) {
                double dh = v7_pt_edge_dist(pu, pv, holesU[h], holesV[h], holes_n[h]);
                if (dh < d_min) d_min = dh;
            }
            wsd += w * d_min;
        }
    }
    // Nearby points: correct iff outside outer OR inside some hole.
    if (include_foreign_in) {
        for (size_t i = 0; i < n_nearby; i++) {
            double pu, pv;
            v7_bp2_project_uv(&nearby_pts[i*3], basis, &pu, &pv);
            bool in_outer = v7_pt_in_poly(pu, pv, outerU, outerV, n_outer);
            bool in_any_hole = false;
            for (int h = 0; h < n_holes; h++) {
                if (v7_pt_in_poly(pu, pv, holesU[h], holesV[h], holes_n[h])) {
                    in_any_hole = true; break;
                }
            }
            bool correct = !in_outer || in_any_hole;
            if (!correct) {
                double w = nearby_w ? nearby_w[i] : 1.0;
                if (w <= 0) continue;
                double d_outer = v7_pt_edge_dist(pu, pv, outerU, outerV, n_outer);
                double d_min = d_outer;
                for (int h = 0; h < n_holes; h++) {
                    double dh = v7_pt_edge_dist(pu, pv, holesU[h], holesV[h], holes_n[h]);
                    if (dh < d_min) d_min = dh;
                }
                wsd += w * d_min;
            }
        }
    }
    return wsd;
}

// ── Cost functions ──────────────────────────────────────────────────

double v7_bp2_cost_geometric(const v7_bp2_anchor_t* anchors,
                             const uint32_t* cycle_idx, int n_cycle,
                             const v7_bp2_basis_t* basis,
                             double weight_compactness,
                             double weight_spike,
                             double spike_threshold_deg) {
    if (n_cycle < 3) return INFINITY;
    double* uU = (double*)malloc(n_cycle * sizeof(double));
    double* uV = (double*)malloc(n_cycle * sizeof(double));
    if (!uU || !uV) { free(uU); free(uV); return INFINITY; }
    project_cycle_uv(anchors, cycle_idx, n_cycle, basis, uU, uV);
    double cost;
    if (v7_bp2_polygon_self_intersects_uv(uU, uV, n_cycle)) {
        cost = INFINITY;
    } else {
        double comp = v7_bp2_polygon_compactness_uv(uU, uV, n_cycle);
        double spike = v7_bp2_spike_penalty_uv(uU, uV, n_cycle, spike_threshold_deg);
        cost = weight_compactness * comp + weight_spike * spike;
    }
    free(uU); free(uV);
    return cost;
}

double v7_bp2_cost_functional(const v7_bp2_anchor_t* anchors,
                              const uint32_t* cycle_idx, int n_cycle,
                              const v7_bp2_basis_t* basis,
                              const double* group_pts, size_t n_group, const float* group_w,
                              const double* nearby_pts, size_t n_nearby, const float* nearby_w,
                              const double* const* holesU, const double* const* holesV,
                              const int* holes_n, int n_holes,
                              bool include_foreign_in,
                              double weight_wsd,
                              double weight_compactness,
                              double weight_spike,
                              double spike_threshold_deg) {
    if (n_cycle < 3) return INFINITY;
    double* uU = (double*)malloc(n_cycle * sizeof(double));
    double* uV = (double*)malloc(n_cycle * sizeof(double));
    if (!uU || !uV) { free(uU); free(uV); return INFINITY; }
    project_cycle_uv(anchors, cycle_idx, n_cycle, basis, uU, uV);
    double cost;
    if (v7_bp2_polygon_self_intersects_uv(uU, uV, n_cycle)) {
        cost = INFINITY;
    } else {
        double wsd = v7_bp2_wsd_multi(group_pts, n_group, group_w,
                                       nearby_pts, n_nearby, nearby_w,
                                       basis, uU, uV, n_cycle,
                                       holesU, holesV, holes_n, n_holes,
                                       include_foreign_in);
        double comp = v7_bp2_polygon_compactness_uv(uU, uV, n_cycle);
        double spike = v7_bp2_spike_penalty_uv(uU, uV, n_cycle, spike_threshold_deg);
        cost = weight_wsd * wsd + weight_compactness * comp + weight_spike * spike;
    }
    free(uU); free(uV);
    return cost;
}

// Per-component classification was removed 2026-05-21 — replaced by
// subset enumeration in v7_bp2_compute (see "Top-level compute"
// section at the bottom of this file + journal entry of same date).
// The cost function decides which components belong via direct
// competition between candidate subsets, instead of a fragile
// orientation-WSD vote per component.

// ── TSP solver: full enumeration for small n, multi-start local search else ─

// 2-opt move: reverse the sub-segment [i+1 .. j] in cycle.
static void reverse_segment(uint32_t* cycle, int n, int i, int j) {
    (void)n;
    while (i < j) {
        uint32_t t = cycle[i]; cycle[i] = cycle[j]; cycle[j] = t;
        i++; j--;
    }
}

// 2-opt + or-opt local search to convergence.
// Always works on cycle in place. cost_eval is called many times.
static double local_search_cost_eval(const v7_bp2_cost_ctx_t* ctx,
                                       v7_bp2_mode_t mode,
                                       const uint32_t* cycle, int n) {
    if (mode == V7_BP2_MODE_GEOMETRIC) {
        return v7_bp2_cost_geometric(ctx->anchors, cycle, n,
                                      &ctx->basis,
                                      ctx->weight_compactness,
                                      ctx->weight_spike,
                                      ctx->spike_threshold_deg);
    } else {
        return v7_bp2_cost_functional(ctx->anchors, cycle, n, &ctx->basis,
                                       ctx->group_pts, ctx->n_group, ctx->group_w,
                                       ctx->nearby_pts, ctx->n_nearby, ctx->nearby_w,
                                       ctx->holesU, ctx->holesV, ctx->holes_n, ctx->n_holes,
                                       ctx->include_foreign_in,
                                       ctx->weight_wsd,
                                       ctx->weight_compactness,
                                       ctx->weight_spike,
                                       ctx->spike_threshold_deg);
    }
}

static double local_search(uint32_t* cycle, int n,
                            v7_bp2_mode_t mode,
                            const v7_bp2_cost_ctx_t* ctx) {
    double cur_cost = local_search_cost_eval(ctx, mode, cycle, n);
    bool improved = true;
    int max_outer = 100;  // hard cap to avoid pathologies
    while (improved && max_outer-- > 0) {
        improved = false;
        // 2-opt: reverse sub-segments [i+1 .. j].
        for (int i = 0; i + 1 < n; i++) {
            for (int j = i + 2; j < n; j++) {
                // Skip the rotation that would just rotate the whole cycle.
                if (i == 0 && j == n - 1) continue;
                reverse_segment(cycle, n, i + 1, j);
                double c = local_search_cost_eval(ctx, mode, cycle, n);
                if (c + 1e-12 < cur_cost) {
                    cur_cost = c;
                    improved = true;
                } else {
                    // revert
                    reverse_segment(cycle, n, i + 1, j);
                }
            }
        }
        // or-opt: take vertex at position i, move to position j.
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (j == i || j == i - 1 || (i == 0 && j == n - 1)) continue;
                // RE-READ v EACH ITERATION: a prior accepted move at this i
                // already mutated cycle in place, so a single capture outside
                // the j loop would re-insert the stale value and produce
                // duplicate cornerIds (see investigation 2026-05-29).
                uint32_t v = cycle[i];
                // Build candidate: remove cycle[i], insert before cycle[j].
                uint32_t* cand = (uint32_t*)malloc(n * sizeof(uint32_t));
                if (!cand) break;
                int p = 0;
                for (int k = 0; k < n; k++) {
                    if (k == i) continue;
                    // Insert v just before position j (where positions are
                    // measured in the ORIGINAL cycle indexing).
                    if (k == j) cand[p++] = v;
                    cand[p++] = cycle[k];
                }
                if (j == n) cand[p++] = v;  // insert at end
                if (p != n) { free(cand); continue; }
                double c = local_search_cost_eval(ctx, mode, cand, n);
                if (c + 1e-12 < cur_cost) {
                    memcpy(cycle, cand, n * sizeof(uint32_t));
                    cur_cost = c;
                    improved = true;
                }
                free(cand);
            }
        }
    }
    return cur_cost;
}

// Mulberry32 for deterministic random initial cycles.
static uint32_t mulberry32(uint32_t* state) {
    uint32_t z = (*state += 0x6D2B79F5u);
    z = (z ^ (z >> 15)) * (z | 1u);
    z ^= z + (z ^ (z >> 7)) * (z | 61u);
    return z ^ (z >> 14);
}

static void shuffle_cycle(uint32_t* cycle, int n, uint32_t seed) {
    uint32_t state = seed;
    for (int i = n - 1; i > 0; i--) {
        uint32_t r = mulberry32(&state) % (uint32_t)(i + 1);
        uint32_t t = cycle[i]; cycle[i] = cycle[r]; cycle[r] = t;
    }
}

// Nearest-neighbour seed: start at `start_idx` (an index INTO cycle, i.e.
// position 0..n-1 in the subset_idx array), greedily append the nearest
// unvisited anchor in plane UV distance.
static void nearest_neighbor_seed(uint32_t* cycle, int n,
                                    const v7_bp2_anchor_t* anchors,
                                    const v7_bp2_basis_t* basis,
                                    int start_pos) {
    if (n < 2) return;
    uint32_t* remaining = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!remaining) return;
    memcpy(remaining, cycle, n * sizeof(uint32_t));
    // Swap remaining[0] and remaining[start_pos] so we start there.
    if (start_pos > 0 && start_pos < n) {
        uint32_t t = remaining[0]; remaining[0] = remaining[start_pos]; remaining[start_pos] = t;
    }
    cycle[0] = remaining[0];
    int rem_n = n - 1;
    for (int i = 0; i < rem_n; i++) remaining[i] = remaining[i + 1];

    for (int k = 1; k < n; k++) {
        const v7_bp2_anchor_t* prev = &anchors[cycle[k - 1]];
        double pu, pv; v7_bp2_project_uv(prev->point, basis, &pu, &pv);
        int best = 0;
        double best_d = INFINITY;
        for (int i = 0; i < rem_n; i++) {
            const v7_bp2_anchor_t* a = &anchors[remaining[i]];
            double au, av; v7_bp2_project_uv(a->point, basis, &au, &av);
            double du = au - pu, dv = av - pv;
            double d = du*du + dv*dv;
            if (d < best_d) { best_d = d; best = i; }
        }
        cycle[k] = remaining[best];
        for (int i = best; i < rem_n - 1; i++) remaining[i] = remaining[i + 1];
        rem_n--;
    }
    free(remaining);
}

// Centroid-angle seed: order anchors by atan2 around the group centroid.
// `centroid_uv[2]` precomputed by the caller.
static void centroid_angle_seed(uint32_t* cycle, int n,
                                  const v7_bp2_anchor_t* anchors,
                                  const v7_bp2_basis_t* basis,
                                  double cu, double cv) {
    typedef struct { uint32_t idx; double angle; } pair_t;
    pair_t* pairs = (pair_t*)malloc(n * sizeof(pair_t));
    if (!pairs) return;
    for (int i = 0; i < n; i++) {
        const v7_bp2_anchor_t* a = &anchors[cycle[i]];
        double au, av; v7_bp2_project_uv(a->point, basis, &au, &av);
        pairs[i].idx = cycle[i];
        pairs[i].angle = atan2(av - cv, au - cu);
    }
    // Simple insertion sort by angle (n is small).
    for (int i = 1; i < n; i++) {
        pair_t cur = pairs[i];
        int j = i;
        while (j > 0 && pairs[j - 1].angle > cur.angle) { pairs[j] = pairs[j - 1]; j--; }
        pairs[j] = cur;
    }
    for (int i = 0; i < n; i++) cycle[i] = pairs[i].idx;
    free(pairs);
}

// Exact best cycle by full permutation enumeration (Heap's algorithm).
// Cost is non-metric AND non-decomposable per-edge (WSD depends on the
// full polygon, not per-edge), so we just enumerate all (n-1)!/2 cycles
// and keep the best. n must be ≤ V7_BP2_FULL_ENUM_MAX (8! = 40320 evals).
static int full_enum_best_cycle(const v7_bp2_anchor_t* anchors,
                                 const uint32_t* subset_idx, int n,
                                 v7_bp2_mode_t mode,
                                 const v7_bp2_cost_ctx_t* ctx,
                                 uint32_t* out_cycle,
                                 double* out_cost) {
    (void)anchors;
    if (n < 2) return -1;
    if (n == 2) {
        out_cycle[0] = subset_idx[0]; out_cycle[1] = subset_idx[1];
        *out_cost = local_search_cost_eval(ctx, mode, out_cycle, 2);
        return 0;
    }
    if (n == 3) {
        // Only one Hamiltonian cycle exists for n=3.
        out_cycle[0] = subset_idx[0];
        out_cycle[1] = subset_idx[1];
        out_cycle[2] = subset_idx[2];
        *out_cost = local_search_cost_eval(ctx, mode, out_cycle, 3);
        return 0;
    }

    // Enumerate permutations of [1..n-1] (fixing position 0).
    // Cycle is (subset_idx[0], subset_idx[perm[1]], ..., subset_idx[perm[n-1]]).
    uint32_t* perm = (uint32_t*)malloc(n * sizeof(uint32_t));
    uint32_t* best = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!perm || !best) { free(perm); free(best); return -1; }
    for (int i = 0; i < n; i++) perm[i] = subset_idx[i];
    // Heap's algorithm for permutation enumeration on perm[1..n-1].
    double best_c = INFINITY;
    int* c = (int*)calloc(n, sizeof(int));
    if (!c) { free(perm); free(best); return -1; }
    // Initial perm.
    {
        double cc = local_search_cost_eval(ctx, mode, perm, n);
        if (cc < best_c) { best_c = cc; memcpy(best, perm, n * sizeof(uint32_t)); }
    }
    int i = 1;
    while (i < n) {
        if (c[i] < i) {
            int swap_with = (i % 2 == 0) ? 1 : c[i];
            uint32_t t = perm[swap_with]; perm[swap_with] = perm[i]; perm[i] = t;
            double cc = local_search_cost_eval(ctx, mode, perm, n);
            if (cc < best_c) { best_c = cc; memcpy(best, perm, n * sizeof(uint32_t)); }
            c[i]++; i = 1;
        } else { c[i] = 0; i++; }
    }
    memcpy(out_cycle, best, n * sizeof(uint32_t));
    *out_cost = best_c;
    free(perm); free(best); free(c);
    return 0;
}

int v7_bp2_best_cycle(const v7_bp2_anchor_t* anchors,
                      const uint32_t* subset_idx, int n_idx,
                      v7_bp2_mode_t mode,
                      const v7_bp2_cost_ctx_t* ctx,
                      uint32_t* out_cycle,
                      double* out_cost) {
    if (n_idx < 3) {
        if (n_idx > 0) memcpy(out_cycle, subset_idx, n_idx * sizeof(uint32_t));
        if (out_cost) *out_cost = INFINITY;
        return -1;
    }

    if (n_idx <= V7_BP2_FULL_ENUM_MAX) {
        return full_enum_best_cycle(anchors, subset_idx, n_idx, mode, ctx, out_cycle, out_cost);
    }

    // Multi-start local search. Seeds: NN(0), NN(n/2), centroid-angle,
    // K_RANDOM random cycles.
    int n_seeds = 2 + 1 + V7_BP2_K_RANDOM;
    uint32_t* best = (uint32_t*)malloc(n_idx * sizeof(uint32_t));
    if (!best) return -1;
    double best_c = INFINITY;

    // Compute centroid of group points if we have them — falls back to anchor
    // centroid for GEOMETRIC mode.
    double cu = 0, cv = 0;
    if (mode == V7_BP2_MODE_FUNCTIONAL && ctx->n_group > 0) {
        for (size_t i = 0; i < ctx->n_group; i++) {
            double pu, pv;
            v7_bp2_project_uv(&ctx->group_pts[i*3], &ctx->basis, &pu, &pv);
            cu += pu; cv += pv;
        }
        cu /= (double)ctx->n_group; cv /= (double)ctx->n_group;
    } else {
        for (int i = 0; i < n_idx; i++) {
            const v7_bp2_anchor_t* a = &anchors[subset_idx[i]];
            double au, av; v7_bp2_project_uv(a->point, &ctx->basis, &au, &av);
            cu += au; cv += av;
        }
        cu /= (double)n_idx; cv /= (double)n_idx;
    }

    uint32_t* cycle = (uint32_t*)malloc(n_idx * sizeof(uint32_t));
    if (!cycle) { free(best); return -1; }

    for (int s = 0; s < n_seeds; s++) {
        memcpy(cycle, subset_idx, n_idx * sizeof(uint32_t));
        if (s == 0) {
            nearest_neighbor_seed(cycle, n_idx, anchors, &ctx->basis, 0);
        } else if (s == 1) {
            nearest_neighbor_seed(cycle, n_idx, anchors, &ctx->basis, n_idx / 2);
        } else if (s == 2) {
            centroid_angle_seed(cycle, n_idx, anchors, &ctx->basis, cu, cv);
        } else {
            shuffle_cycle(cycle, n_idx, (uint32_t)(0x12345 + s));
        }
        double c = local_search(cycle, n_idx, mode, ctx);
        if (c < best_c) {
            best_c = c;
            memcpy(best, cycle, n_idx * sizeof(uint32_t));
        }
    }

    memcpy(out_cycle, best, n_idx * sizeof(uint32_t));
    if (out_cost) *out_cost = best_c;
    free(best); free(cycle);
    return 0;
}

// ── Ordering: arc-length walk + windowed or-opt (bp3, 2026-05-30) ────
//
// The contour already encodes the boundary order; bp3 uses it. We order
// the selected anchors by their arc-length position along the closed
// contour, then a windowed or-opt refinement under COMPACTNESS only
// disambiguates near-ties. No WSD (it rewards weaving along LIDAR scanline
// stripes → zigzag), no multi-start. See journal
// 2026-05-30-bp3-contour-arclength-ordering for the full rationale.

// Arc-length of the nearest point on the closed contour to (pu,pv).
// `out_perp` returns a signed perpendicular measure at the nearest segment
// (deterministic tie-break when two anchors share an arc-length position).
static double contour_arclen(double pu, double pv,
                             const double* polyU, const double* polyV, int n,
                             double* out_perp) {
    double best_d2 = INFINITY, best_s = 0.0, best_perp = 0.0, s = 0.0;
    for (int i = 0; i < n; i++) {
        int i1 = (i + 1) % n;
        double ax = polyU[i],  ay = polyV[i];
        double bx = polyU[i1], by = polyV[i1];
        double abx = bx - ax, aby = by - ay;
        double L2 = abx * abx + aby * aby;
        double t = (L2 < 1e-18) ? 0.0 : ((pu - ax) * abx + (pv - ay) * aby) / L2;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        double px = ax + t * abx, py = ay + t * aby;
        double dx = pu - px, dy = pv - py;
        double d2 = dx * dx + dy * dy;
        double seglen = sqrt(L2);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_s  = s + t * seglen;
            best_perp = abx * (pv - ay) - aby * (pu - ax);  // signed cross
        }
        s += seglen;
    }
    if (out_perp) *out_perp = best_perp;
    return best_s;
}

// Order the cycle (anchor indices) by arc-length along the contour.
static void arclength_walk(uint32_t* cyc, int n,
                           const double* aU, const double* aV,
                           const double* polyU, const double* polyV, int npoly) {
    if (n < 2 || npoly < 2) return;
    typedef struct { uint32_t idx; double s; double perp; } key_t;
    key_t* keys = (key_t*)malloc(n * sizeof(key_t));
    if (!keys) return;
    for (int i = 0; i < n; i++) {
        double perp;
        double s = contour_arclen(aU[cyc[i]], aV[cyc[i]], polyU, polyV, npoly, &perp);
        keys[i].idx = cyc[i]; keys[i].s = s; keys[i].perp = perp;
    }
    // Insertion sort by (arc-length, perp, id) — deterministic, n is small.
    for (int i = 1; i < n; i++) {
        key_t cur = keys[i];
        int j = i;
        while (j > 0) {
            key_t pv = keys[j - 1];
            bool less;
            if      (cur.s    < pv.s    - 1e-9) less = true;
            else if (cur.s    > pv.s    + 1e-9) less = false;
            else if (cur.perp < pv.perp - 1e-9) less = true;
            else if (cur.perp > pv.perp + 1e-9) less = false;
            else                                less = (cur.idx < pv.idx);
            if (!less) break;
            keys[j] = keys[j - 1]; j--;
        }
        keys[j] = cur;
    }
    for (int i = 0; i < n; i++) cyc[i] = keys[i].idx;
    free(keys);
}

// Compactness of a cycle given as anchor indices into aU/aV.
static double cycle_compactness(const uint32_t* cyc, int n,
                                const double* aU, const double* aV) {
    if (n < 3) return INFINITY;
    double area2 = 0.0, perim = 0.0;
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        double ux = aU[cyc[k]],  vy = aV[cyc[k]];
        double ux1 = aU[cyc[k1]], vy1 = aV[cyc[k1]];
        area2 += ux * vy1 - ux1 * vy;
        double du = ux1 - ux, dv = vy1 - vy;
        perim += sqrt(du * du + dv * dv);
    }
    double area = fabs(area2) * 0.5;
    if (area < 1e-12) return INFINITY;
    return (perim * perim) / (4.0 * M_PI * area);
}

static bool cycle_self_intersects(const uint32_t* cyc, int n,
                                   const double* aU, const double* aV,
                                   double* scratchU, double* scratchV) {
    for (int k = 0; k < n; k++) { scratchU[k] = aU[cyc[k]]; scratchV[k] = aV[cyc[k]]; }
    return v7_bp2_polygon_self_intersects_uv(scratchU, scratchV, n);
}

// Windowed or-opt: relocate one vertex up to `window` positions away, greedy
// accept on strict compactness improvement, self-intersection guard on
// accepted moves only. Converges to a local optimum (no accepted move) or
// the 100-sweep backstop. Deterministic given the seed.
static void windowed_oropt(uint32_t* cyc, int n,
                           const double* aU, const double* aV,
                           int window) {
    if (n < 4 || window < 1) return;  // n=3 → unique cycle, nothing to refine
    uint32_t* trial = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    uint32_t* rest  = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    double*   sU    = (double*)malloc((size_t)n * sizeof(double));
    double*   sV    = (double*)malloc((size_t)n * sizeof(double));
    if (!trial || !rest || !sU || !sV) { free(trial); free(rest); free(sU); free(sV); return; }

    double cur = cycle_compactness(cyc, n, aU, aV);
    bool improved = true;
    int guard = 100;
    while (improved && guard-- > 0) {
        improved = false;
        for (int i = 0; i < n; i++) {
            uint32_t x = cyc[i];
            // Compacted order without position i.
            int m = 0;
            for (int s = 0; s < n; s++) if (s != i) rest[m++] = cyc[s];
            int lo = i - window; if (lo < 0) lo = 0;
            int hi = i + window; if (hi > n - 1) hi = n - 1;
            for (int t = lo; t <= hi; t++) {
                if (t == i) continue;                 // identity reinsertion
                for (int s = 0; s < t; s++)       trial[s]     = rest[s];
                trial[t] = x;
                for (int s = t; s < n - 1; s++)   trial[s + 1] = rest[s];
                double c = cycle_compactness(trial, n, aU, aV);
                if (c + 1e-12 < cur &&
                    !cycle_self_intersects(trial, n, aU, aV, sU, sV)) {
                    memcpy(cyc, trial, (size_t)n * sizeof(uint32_t));
                    cur = c;
                    improved = true;
                    break;                            // accept; re-scan from this i
                }
            }
            if (improved) break;                      // restart the sweep
        }
    }
    free(trial); free(rest); free(sU); free(sV);
}

// ── Output lifecycle ────────────────────────────────────────────────

void v7_bp2_output_init(v7_bp2_output_t* out) {
    if (!out) return;
    out->polygons = NULL;
    out->n_polygons = 0;
}

void v7_bp2_output_free(v7_bp2_output_t* out) {
    if (!out) return;
    if (out->polygons) {
        for (size_t i = 0; i < out->n_polygons; i++) {
            free(out->polygons[i].corner_ids);
        }
        free(out->polygons);
    }
    out->polygons = NULL;
    out->n_polygons = 0;
}

// ── Top-level compute (bp3) ─────────────────────────────────────────
//
// One polygon per contour. Each contour's anchor subset is collected by
// distance to any contour edge (≤ V7_BP2_CORNER_TO_FRAGMENT_RANGE_M).
// The subset is fed to best_cycle in FUNCTIONAL mode; the resulting
// cycle becomes the polygon. Contours with <3 in-range anchors are
// skipped silently — a plane with no valid contour returns +1.

int v7_bp2_compute(const v7_bp2_inputs_t* in, v7_bp2_output_t* out) {
    v7_bp2_output_init(out);
    if (!in || in->n_anchors < 3) return +1;
    if (!in->contours || in->n_contours == 0) return +1;

    // Pre-project anchors to plane UV once — reused across every contour.
    double* anchorU = (double*)malloc(in->n_anchors * sizeof(double));
    double* anchorV = (double*)malloc(in->n_anchors * sizeof(double));
    if (!anchorU || !anchorV) { free(anchorU); free(anchorV); return -1; }
    for (size_t a = 0; a < in->n_anchors; a++) {
        v7_bp2_project_uv(in->anchors[a].point, &in->basis,
                          &anchorU[a], &anchorV[a]);
    }

    // Ordering no longer uses the FUNCTIONAL WSD cost / multi-start TSP — see
    // the "Ordering" section above and journal 2026-05-30. We order anchors by
    // arc-length along the contour, then refine with windowed or-opt under
    // compactness. group/nearby points are still read below for the emitted
    // WSD diagnostics only.

    // Upper bound on emitted polygons = one per contour. Allocate now;
    // shrink later by realloc.
    out->polygons = (v7_bp2_polygon_t*)calloc(in->n_contours, sizeof(*out->polygons));
    if (!out->polygons) { free(anchorU); free(anchorV); return -1; }
    out->n_polygons = 0;

    const double range = V7_BP2_CORNER_TO_FRAGMENT_RANGE_M;

    uint32_t* subset = (uint32_t*)malloc(in->n_anchors * sizeof(uint32_t));
    if (!subset) { v7_bp2_output_free(out); free(anchorU); free(anchorV); return -1; }

    for (size_t ci = 0; ci < in->n_contours; ci++) {
        const v7_bp2_contour_t* c = &in->contours[ci];
        if (!c->polyU || !c->polyV || c->n_pts < 3) continue;

        // Phase 1: collect anchors within `range` of any edge of this contour.
        size_t n_sub = 0;
        for (size_t a = 0; a < in->n_anchors; a++) {
            double d = v7_pt_edge_dist(anchorU[a], anchorV[a],
                                       c->polyU, c->polyV, c->n_pts);
            if (d <= range) subset[n_sub++] = (uint32_t)a;
        }
        if (n_sub < 3) continue;

        // Phase 2: order the subset by ARC-LENGTH along this contour, then a
        // windowed or-opt refinement under compactness disambiguates near-ties.
        uint32_t* cycle = (uint32_t*)malloc(n_sub * sizeof(uint32_t));
        if (!cycle) continue;
        memcpy(cycle, subset, n_sub * sizeof(uint32_t));
        arclength_walk(cycle, (int)n_sub, anchorU, anchorV,
                       c->polyU, c->polyV, c->n_pts);
        windowed_oropt(cycle, (int)n_sub, anchorU, anchorV, V7_BP2_OROPT_WINDOW);
        // Reject a degenerate / self-intersecting result (should not happen
        // post-refinement; the windowed search keeps the polygon simple).
        {
            double* vU = (double*)malloc(n_sub * sizeof(double));
            double* vV = (double*)malloc(n_sub * sizeof(double));
            bool bad = (!vU || !vV);
            if (!bad) {
                for (size_t i = 0; i < n_sub; i++) { vU[i] = anchorU[cycle[i]]; vV[i] = anchorV[cycle[i]]; }
                bad = v7_bp2_polygon_self_intersects_uv(vU, vV, (int)n_sub);
            }
            free(vU); free(vV);
            if (bad) { free(cycle); continue; }
        }

        // Phase 3: emit polygon for this contour.
        v7_bp2_polygon_t* pg = &out->polygons[out->n_polygons];
        pg->corner_ids = (uint32_t*)malloc(n_sub * sizeof(uint32_t));
        if (!pg->corner_ids) { free(cycle); continue; }
        for (size_t i = 0; i < n_sub; i++) {
            pg->corner_ids[i] = in->anchors[cycle[i]].id;
        }
        pg->n_corners = n_sub;
        pg->kind = c->is_hole ? V7_BP2_KIND_INNER : V7_BP2_KIND_OUTER;

        // Diagnostics — recompute on the chosen cycle.
        double* uU = (double*)malloc(n_sub * sizeof(double));
        double* uV = (double*)malloc(n_sub * sizeof(double));
        if (uU && uV) {
            for (size_t i = 0; i < n_sub; i++) {
                uU[i] = anchorU[cycle[i]];
                uV[i] = anchorV[cycle[i]];
            }
            pg->wsd = v7_bp2_wsd_multi(in->group_pts, in->n_group_pts, in->group_weights,
                                       in->nearby_pts, in->n_nearby_pts, in->nearby_weights,
                                       &in->basis,
                                       uU, uV, (int)n_sub,
                                       NULL, NULL, NULL, 0,
                                       !in->is_wall);
            pg->compactness = v7_bp2_polygon_compactness_uv(uU, uV, (int)n_sub);
            pg->spike_penalty = v7_bp2_spike_penalty_uv(uU, uV, (int)n_sub,
                                                       V7_BP2_SPIKE_THRESHOLD_DEG);
        }
        free(uU); free(uV); free(cycle);
        out->n_polygons++;
    }

    free(subset);
    free(anchorU); free(anchorV);

    if (out->n_polygons == 0) {
        free(out->polygons);
        out->polygons = NULL;
        return +1;
    }
    // Shrink polygon array to fit.
    if (out->n_polygons < in->n_contours) {
        v7_bp2_polygon_t* shrunk = (v7_bp2_polygon_t*)realloc(
            out->polygons, out->n_polygons * sizeof(*out->polygons));
        if (shrunk) out->polygons = shrunk;
    }
    return 0;
}
