// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-polygons-2 — bp3 implementation. Algorithmic rewrite of
// boundary-polygons (bp1) and bp2's classify/subset-enum top-level.
//
// History (see journal entries for full context):
//   bp1 (boundary-polygons.c, in production) — balance walk + arc-length
//     ordering + D.7 permute+delete. Fragile FP coin-flip on contour
//     index rounding; silent centroid-angle fallback when walk fails.
//   bp2 (this file, May 21) — TSP via components → 2^K subset enum.
//     Sound architecture, untenable perf (22 min/plane on dahouet K=12).
//   bp3 (this file, May 24, supersedes bp2 top-level) — topology
//     delegated to the grid-fill contour (closed-by-construction outer
//     + holes from v7-boundary-signals' RG_SECT_PLANE_CONTOURS).
//
// Algorithm (per plane):
//   Phase 1 — anchors-per-contour: for each upstream contour, collect
//             anchors within V7_BP2_CORNER_TO_FRAGMENT_RANGE_M of any
//             contour edge. An anchor may belong to multiple contours
//             (a corner where outer meets a hole gets two polygons —
//             one per side — and that's geometrically valid).
//   Phase 2 — per-contour ordering: order the anchor subset by ARC-LENGTH
//             along the contour (the topology the contour already encodes),
//             then a windowed or-opt refinement under COMPACTNESS only
//             (self-intersect → reject) disambiguates near-ties. No WSD, no
//             multi-start — WSD rewards weaving along LIDAR scanline stripes
//             (zigzag). See journal 2026-05-30-bp3-contour-arclength-ordering.
//             (best_cycle / FUNCTIONAL WSD-TSP retained for unit fixtures +
//             diagnostics, but OFF the ordering path.)
//   Phase 3 — emit one v7_bp2_polygon_t per contour with ≥3 anchors,
//             kind=OUTER for the perimeter contour, kind=INNER for
//             each hole.
//
// No component merging, no subset enum, no orientation classification —
// the contour already encodes all three. Lower layers (best_cycle, WSD,
// cost fn, spike penalty, geometric primitives) carry over from bp2
// unchanged.

#ifndef V3_STEPS_V7_BOUNDARY_POLYGONS_2_IMPL_H
#define V3_STEPS_V7_BOUNDARY_POLYGONS_2_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Public types ────────────────────────────────────────────────────

typedef struct v7_bp2_anchor {
    uint32_t id;                 // corner pool-id (informational)
    double   point[3];           // world position (or building-local — caller's choice, must be consistent across anchors + group + nearby)
    uint8_t  source;             // 1=RI, 2=RE, 3=PM, 4=concave, 5=convex, 8=coplanar
} v7_bp2_anchor_t;

// Plane basis. e1, e2 unit, normal = e1 × e2 (right-handed).
typedef struct v7_bp2_basis {
    double e1[3];
    double e2[3];
    double normal[3];
} v7_bp2_basis_t;

// One contour (closed loop) from v7-boundary-signals' grid-fill output.
// Points are in plane UV (already projected through the basis upstream).
typedef struct v7_bp2_contour {
    const double* polyU;   // closed loop — vertex i+1 wraps to 0 at i=n-1
    const double* polyV;
    int           n_pts;
    bool          is_hole; // false = outer perimeter, true = interior hole
} v7_bp2_contour_t;

typedef struct v7_bp2_inputs {
    const v7_bp2_anchor_t* anchors;
    size_t                 n_anchors;

    const double* group_pts;     // [n_group_pts * 3]
    size_t        n_group_pts;
    const float*  group_weights; // optional; if NULL, all weights = 1
    const double* nearby_pts;    // [n_nearby_pts * 3]
    size_t        n_nearby_pts;
    const float*  nearby_weights;// optional

    v7_bp2_basis_t basis;

    // Topology source. One polygon emitted per contour with ≥3 anchors
    // within V7_BP2_CORNER_TO_FRAGMENT_RANGE_M of any contour edge. NULL
    // or n_contours==0 → returns +1 (no polygons produced).
    const v7_bp2_contour_t* contours;
    size_t                  n_contours;

    // Wall planes skip the "nearby-inside" precision term in WSD (a wall
    // legitimately runs alongside neighbours' nearby points).
    bool is_wall;
} v7_bp2_inputs_t;

typedef enum v7_bp2_polygon_kind {
    V7_BP2_KIND_OUTER = 1,
    V7_BP2_KIND_INNER = 2,
} v7_bp2_polygon_kind_t;

typedef struct v7_bp2_polygon {
    uint32_t* corner_ids;        // owned — pool ids in cycle order
    size_t    n_corners;
    v7_bp2_polygon_kind_t kind;
    // Diagnostics — filled by compute, ignored on input.
    double    wsd;
    double    compactness;
    double    spike_penalty;
} v7_bp2_polygon_t;

typedef struct v7_bp2_output {
    v7_bp2_polygon_t* polygons;
    size_t            n_polygons;
} v7_bp2_output_t;

typedef enum v7_bp2_mode {
    V7_BP2_MODE_GEOMETRIC  = 1,
    V7_BP2_MODE_FUNCTIONAL = 2,
} v7_bp2_mode_t;

// ── Top-level compute ───────────────────────────────────────────────
// Returns 0 on success, -1 on allocation failure, +1 when no polygon
// can be produced for this plane (e.g. < 3 anchors). Output is always
// initialised on return; caller frees via v7_bp2_output_free.
int v7_bp2_compute(const v7_bp2_inputs_t* in, v7_bp2_output_t* out);

void v7_bp2_output_init(v7_bp2_output_t* out);
void v7_bp2_output_free(v7_bp2_output_t* out);

// ── Primitives exposed for unit testing ─────────────────────────────

// Project a 3D world point into the plane's (u,v) basis.
static inline void v7_bp2_project_uv(const double p[3],
                                       const v7_bp2_basis_t* b,
                                       double* out_u, double* out_v) {
    *out_u = p[0]*b->e1[0] + p[1]*b->e1[1] + p[2]*b->e1[2];
    *out_v = p[0]*b->e2[0] + p[1]*b->e2[1] + p[2]*b->e2[2];
}

// Polygon is a closed cycle of (u, v) vertices.
// pt_in_poly: standard ray-cast inside test.
// pt_edge_dist: closest distance from (pu, pv) to any edge.
bool   v7_bp2_pt_in_poly(double pu, double pv,
                          const double* polyU, const double* polyV, int n);
double v7_bp2_pt_edge_dist(double pu, double pv,
                            const double* polyU, const double* polyV, int n);

double v7_bp2_polygon_area_uv      (const double* polyU, const double* polyV, int n);
double v7_bp2_polygon_perimeter_uv (const double* polyU, const double* polyV, int n);
double v7_bp2_polygon_compactness_uv(const double* polyU, const double* polyV, int n);

// Self-intersection — O(n²) segment-pair test. Adjacent segments and
// rotation-adjacent segments are skipped.
/*@ requires n <= 0 || (\valid_read(polyU + (0 .. n - 1))
                     && \valid_read(polyV + (0 .. n - 1)));
    assigns \nothing;
*/
bool   v7_bp2_polygon_self_intersects_uv(const double* polyU, const double* polyV, int n);

// Local O(n) self-intersection test: does either ring edge incident to
// vertex `at_idx` — (prev→at) or (at→next), cyclic — strictly cross any
// NON-ADJACENT ring edge? Reuses the same crossing primitive as the whole-ring
// test above. Use after moving/inserting one vertex (MERGE/SPLIT geometry
// guard) to check just the changed edges instead of rescanning the whole ring.
// Returns false for degenerate rings (n < 4) or out-of-range at_idx.
/*@ requires n <= 0 || (\valid_read(U + (0 .. n - 1))
                     && \valid_read(V + (0 .. n - 1)));
    assigns \nothing;
*/
bool   v7_local_edges_cross_uv(const double* U, const double* V, int n, int at_idx);

// Spike penalty: sum over interior angles of max(0, threshold - angle)².
// Only acute corners (angle < spike_threshold_deg, e.g. 30°) are
// penalised. Flat corners (angle near 180°) yield 0.
/*@ requires n <= 0 || (\valid_read(polyU + (0 .. n - 1))
                     && \valid_read(polyV + (0 .. n - 1)));
    assigns \nothing;
*/
double v7_bp2_spike_penalty_uv(const double* polyU, const double* polyV, int n,
                                double spike_threshold_deg);

// Multi-polygon WSD. group inside outer AND outside every hole = correct.
// nearby outside outer OR inside some hole = correct. Sums weighted
// edge-distance for each wrong-side point.
//
// `include_foreign_in` matches v2 semantics (false for walls).
double v7_bp2_wsd_multi(const double* group_pts, size_t n_group, const float* group_w,
                        const double* nearby_pts, size_t n_nearby, const float* nearby_w,
                        const v7_bp2_basis_t* basis,
                        const double* outerU, const double* outerV, int n_outer,
                        // holes is array of (holeU, holeV, n) tuples — flat double arrays.
                        // For now (K=1) callers pass n_holes=0 and the args are ignored.
                        const double* const* holesU, const double* const* holesV,
                        const int* holes_n, int n_holes,
                        bool include_foreign_in);

// Cycle cost: cycle is an array of indices into anchors[].
// Cost includes self-intersection check (+∞ if intersecting).
double v7_bp2_cost_geometric(const v7_bp2_anchor_t* anchors,
                             const uint32_t* cycle_idx, int n_cycle,
                             const v7_bp2_basis_t* basis,
                             double weight_compactness,
                             double weight_spike,
                             double spike_threshold_deg);

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
                              double spike_threshold_deg);

// TSP best cycle through `n_idx` of the `n_anchors` total anchors
// (the cycle visits the indices given by `subset_idx`, in some order).
// `out_cycle` must have room for at least n_idx uint32_t.
// Returns 0 on success, -1 on error.
typedef struct v7_bp2_cost_ctx {
    // For GEOMETRIC mode:
    const v7_bp2_anchor_t* anchors;
    v7_bp2_basis_t         basis;
    double weight_compactness;
    double weight_spike;
    double spike_threshold_deg;
    // For FUNCTIONAL mode (in addition to the above):
    const double* group_pts;  size_t n_group;  const float* group_w;
    const double* nearby_pts; size_t n_nearby; const float* nearby_w;
    const double* const* holesU; const double* const* holesV;
    const int*   holes_n;     int    n_holes;
    bool   include_foreign_in;
    double weight_wsd;
} v7_bp2_cost_ctx_t;

int v7_bp2_best_cycle(const v7_bp2_anchor_t* anchors,
                      const uint32_t* subset_idx, int n_idx,
                      v7_bp2_mode_t mode,
                      const v7_bp2_cost_ctx_t* ctx,
                      uint32_t* out_cycle,
                      double* out_cost);

// ── Tunable constants (defaults defined in impl.c) ──────────────────
//
// These can be overridden via the test harness for fuzz-tuning. The
// agent has authority to tune within ±50% of the defaults during
// autonomous execution; outside that, see the scope doc.

extern int    V7_BP2_FULL_ENUM_MAX;        //  8  — full-permutation enum upper bound; above this falls through to multi-start local search
extern int    V7_BP2_K_RANDOM;             //  5  — multi-start random seeds
extern double V7_BP2_WEIGHT_WSD;           // 0.8
extern double V7_BP2_WEIGHT_COMPACTNESS;   // 0.1
extern double V7_BP2_WEIGHT_SPIKE;         // 0.1
extern double V7_BP2_SPIKE_THRESHOLD_DEG;  // 30.0
extern double V7_BP2_CORNER_TO_FRAGMENT_RANGE_M; // 0.75 — anchor-to-contour-edge distance threshold (matches bp1)
extern int    V7_BP2_OROPT_WINDOW;         //  2  — windowed or-opt reinsertion radius (positions). Ordering refinement under compactness; see journal 2026-05-30-bp3-contour-arclength-ordering

// ── Params (step-level overrides) ───────────────────────────────────
//
// `v7_bp2_params_t` is the public, single-source-of-truth struct that
// the step wrapper parses from its JSON params and applies before
// calling v7_bp2_compute. v7_bp2_params_apply() copies the values into
// the V7_BP2_* globals above, which is what the algorithm actually
// reads — keeps the algorithm code simple at the cost of a brief
// non-thread-safe window inside the step call (single-threaded per
// step invocation, so safe in our pipeline).

typedef struct v7_bp2_params {
    // TSP
    int     full_enum_max;             // 8
    int     k_random;                  // 5
    // Cost weights
    double  weight_wsd;                // 0.8
    double  weight_compactness;        // 0.1
    double  weight_spike;              // 0.1
    double  spike_threshold_deg;       // 30.0
    // Anchor-to-contour assignment
    double  corner_to_fragment_range_m;// 0.75
    // Ordering refinement (windowed or-opt under compactness)
    int     oropt_window;              // 2 — reinsertion radius in positions
    // Plane / wall handling (used by the step wrapper, not by compute())
    double  wall_tilt_min_deg;         // 72.0 — matches bp1
    int32_t up_axis;                   // 2 (z-up)
} v7_bp2_params_t;

void v7_bp2_params_default(v7_bp2_params_t* p);
void v7_bp2_params_apply(const v7_bp2_params_t* p);

#ifdef __cplusplus
}
#endif
#endif
