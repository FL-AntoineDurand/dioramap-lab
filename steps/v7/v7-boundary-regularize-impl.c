// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-regularize — pure compute, R.0 + R.0b + R.1 single-corner.

#define _POSIX_C_SOURCE 200809L

#include "v7-boundary-regularize-impl.h"
#include "v3/safe_alloc.h"
#include "v7-corner-pool.h"                // T1: v7p_solve_member_position (constrained-meet)
#include "v7-boundary-polygons-2-impl.h"   // T2: v7_local_edges_cross_uv (pre-commit self-int)
#include "v7-roof-graph-format.h"          // rg_plane_type_t (authoritative plane type)

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>          // clock_gettime for BR_PROFILE
#include <inttypes.h>      // PRIu64 for profile output

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WALL_TILT_MIN_DEG     72.0
#define NEARBY_OUTSIDE_MIN_M  0.20   // foreign-in points within this of plane → skipped
#define EPS                   1e-9

// ── Defaults ──────────────────────────────────────────────────────

void v7_br_params_default(v7_br_params_t* p) {
    if (!p) return;
    // Default lowered from 1000 → 200 (2026-05-30). Ablation matrix on
    // dahouet-bldgonly showed 90% of total cost improvement is reached by
    // iter ~19; cap=200 finishes at <2% worse cost than cap=1000 for 5×
    // less wall time. Override via pipeline param `maxIterations` if a
    // specific case actually needs more. See
    // v3/docs/plan/BOUNDARY-REGULARIZE-PROFILE-PLAN.md and the report at
    // /tmp/br-ablation/report.html for the data.
    p->max_iterations              = 200;
    p->nudge_range_m               = 0.15;
    p->nudge_step_m                = 0.03;
    p->angle_snap_thresh_deg       = 20.0;
    p->use_wsd_cost                = true;
    p->lambda_wsd                  = 1.0;
    p->wsd_sigma_m                 = 1.0;
    p->use_angle_cost              = true;
    p->lambda_angle                = 2.0;
    p->angle_sigma_deg             = 5.0;
    p->use_neighbor_angle_cost     = true;
    p->lambda_neighbor_angle       = 2.0;
    p->use_horizontal_edge_cost    = true;
    p->lambda_horizontal_edge      = 1.0;
    p->horizontal_edge_sigma_deg   = 15.0;
    p->use_trust_weighting         = true;
    p->use_extended_angles         = false;
    p->use_proximity_merge         = true;
    p->proximity_merge_dist_m      = 0.30;
    p->use_collinear_removal       = true;
    p->collinear_removal_threshold_deg = 10.0;
    p->use_ridge_merge             = true;
    // 0.50 m gap: dahouet's clean on-crease corner pairs sit ~0.4 m apart
    // (the two plane polygons don't meet exactly on the ridge); 0.30 m
    // caught only 1 such pair, 0.50 catches the genuine on-ridge cases
    // without the >0.8 m aggressive merges seen at 1.0 m. coplanar 0.30 m
    // = "lies on the crease line". See R.4 audit 2026-06-12.
    p->ridge_merge_dist_m          = 0.50;
    p->ridge_merge_coplanar_m      = 0.30;
    p->use_joint_moves             = true;
    p->joint_samples_per_edge      = 30;
    p->joint_move_seed             = 0;
    p->use_horiz_coplanar_cost     = true;
    p->lambda_horiz_coplanar       = 1.0;
    p->horiz_coplanar_sigma_m      = 0.10;
    p->use_vert_coplanar_cost      = true;
    p->lambda_vert_coplanar        = 1.0;
    p->vert_coplanar_sigma_m       = 0.10;
    p->use_collinear_cost          = true;
    p->lambda_collinear            = 1.0;
    p->collinear_sigma_m           = 0.10;
    p->wsd_normalize               = false;  // dead-end: ÷Σw destroys coverage (journal
                                             // 2026-06-30); kept off, param machinery
                                             // dormant + uncommitted, excluded from commit.
    p->corner_insert               = true;   // #25 in-loop corner insertion (default ON)
    p->lambda_compact              = 0.0;    // weighted compactness (scale-coupled, kept off)
    p->compact_gate                = false;  // gated marginal-compactness rule (opt-in via pipeline)
    p->compact_gate_eps            = 0.05;   // "nothing on WSD" floor (WSD units)
    p->cost_shape                  = 0;
    p->r1_acceptance_strategy      = 0;  // GLOBAL_BEST — matches v2
    p->up_axis                     = 1;
}

// ── Outputs lifecycle ─────────────────────────────────────────────

void v7_br_outputs_init(v7_br_outputs_t* o) { if (o) memset(o, 0, sizeof(*o)); }

void v7_br_outputs_free(v7_br_outputs_t* o) {
    if (!o) return;
    free(o->dof);
    free(o->corner_positions);
    if (o->corner_planes) {
        for (size_t i = 0; i < o->n_corner_planes; i++) free(o->corner_planes[i].plane_ids);
        free(o->corner_planes);
    }
    if (o->polygon_paths) {
        for (size_t i = 0; i < o->n_polygon_paths; i++) free(o->polygon_paths[i].corner_ids);
        free(o->polygon_paths);
    }
    if (o->trajectories) {
        for (size_t i = 0; i < o->n_trajectories; i++) free(o->trajectories[i].steps);
        free(o->trajectories);
    }
    memset(o, 0, sizeof(*o));
}

// ── id maps ───────────────────────────────────────────────────────

typedef struct { uint32_t* idx; uint32_t cap; } id_map_t;
static int id_map_build(id_map_t* m, uint32_t maxp1) {
    m->cap = maxp1;
    if (maxp1 == 0) { m->idx = NULL; return 0; }
    m->idx = (uint32_t*)safe_malloc_array(maxp1, sizeof(uint32_t));
    if (!m->idx) return -1;
    for (uint32_t i = 0; i < maxp1; i++) m->idx[i] = UINT32_MAX;
    return 0;
}
static void id_map_free(id_map_t* m) { free(m->idx); m->idx = NULL; m->cap = 0; }
static inline uint32_t id_lk(const id_map_t* m, uint32_t id) {
    if (!m->idx || id >= m->cap) return UINT32_MAX;
    return m->idx[id];
}
static inline void id_set(id_map_t* m, uint32_t id, uint32_t idx) {
    if (id < m->cap) m->idx[id] = idx;
}

// ── Plane UV basis (mirrors v2 polygon-wsd planeBasis + projectPoint) ─

typedef struct plane_uv_basis {
    double nx, ny, nz, d;
    double e1x, e1y, e1z;
    double e2x, e2y, e2z;
} plane_uv_basis_t;

static void plane_basis(plane_uv_basis_t* b, const double n[3], double d) {
    b->nx = n[0]; b->ny = n[1]; b->nz = n[2]; b->d = d;
    double ux, uy, uz;
    if (fabs(b->ny) > 0.9) { ux = 1; uy = 0; uz = 0; }
    else                    { ux = 0; uy = 1; uz = 0; }
    double e1x = uy*b->nz - uz*b->ny;
    double e1y = uz*b->nx - ux*b->nz;
    double e1z = ux*b->ny - uy*b->nx;
    double e1l = sqrt(e1x*e1x + e1y*e1y + e1z*e1z);
    if (e1l < EPS) e1l = 1.0;
    e1x /= e1l; e1y /= e1l; e1z /= e1l;
    b->e1x = e1x; b->e1y = e1y; b->e1z = e1z;
    b->e2x = b->ny*e1z - b->nz*e1y;
    b->e2y = b->nz*e1x - b->nx*e1z;
    b->e2z = b->nx*e1y - b->ny*e1x;
}

static inline void project_uv(const plane_uv_basis_t* b, double x, double y, double z,
                                double* u, double* v) {
    double sd = b->nx*x + b->ny*y + b->nz*z + b->d;
    double sx = x - sd*b->nx;
    double sy = y - sd*b->ny;
    double sz = z - sd*b->nz;
    *u = sx*b->e1x + sy*b->e1y + sz*b->e1z;
    *v = sx*b->e2x + sy*b->e2y + sz*b->e2z;
}

// Inverse of project_uv: the 3D point ON the plane for UV coords (u,v). With the
// orthonormal basis {e1,e2,n} and the on-plane constraint s·n = -d,
// s = u·e1 + v·e2 − d·n. (#25 corner insertion places a new corner at mass_uv.)
static inline void uv_to_3d(const plane_uv_basis_t* b, double u, double v, double out[3]) {
    out[0] = u*b->e1x + v*b->e2x - b->d*b->nx;
    out[1] = u*b->e1y + v*b->e2y - b->d*b->ny;
    out[2] = u*b->e1z + v*b->e2z - b->d*b->nz;
}

// Point-in-polygon test on UV (Jordan-curve raycast).
static bool point_in_poly_uv(double pu, double pv,
                              const double* polyU, const double* polyV, uint32_t n) {
    bool inside = false;
    uint32_t j = n - 1;
    for (uint32_t i = 0; i < n; i++) {
        if ((polyV[i] > pv) != (polyV[j] > pv)) {
            double xint = (polyU[j] - polyU[i]) * (pv - polyV[i]) / (polyV[j] - polyV[i]) + polyU[i];
            if (pu < xint) inside = !inside;
        }
        j = i;
    }
    return inside;
}

// Min distance from point to polygon-edge segments (UV).
static double pt_edge_dist_uv(double pu, double pv,
                                const double* polyU, const double* polyV, uint32_t n) {
    double min_d = INFINITY;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t k1 = (k + 1) % n;
        double eu = polyU[k1] - polyU[k];
        double ev = polyV[k1] - polyV[k];
        double len2 = eu*eu + ev*ev;
        if (len2 < 1e-12) continue;
        double t = ((pu - polyU[k]) * eu + (pv - polyV[k]) * ev) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        double dx = pu - (polyU[k] + t*eu);
        double dy = pv - (polyV[k] + t*ev);
        double d = sqrt(dx*dx + dy*dy);
        if (d < min_d) min_d = d;
    }
    return isfinite(min_d) ? min_d : 0.0;
}

// Like pt_edge_dist_uv but also returns the index of the NEAREST polygon edge
// (segment k = vertex k → k+1) and the foot parameter t∈[0,1] along it. The
// realizing-edge attribution is the core of the per-edge fit-misfit metric
// (#25, in-loop corner insertion): each mis-placed point is charged to the one
// edge that "should" have contained it.
static double pt_nearest_edge_uv(double pu, double pv,
                                 const double* polyU, const double* polyV, uint32_t n,
                                 uint32_t* out_e, double* out_t) {
    double min_d = INFINITY; uint32_t best_e = 0; double best_t = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t k1 = (k + 1) % n;
        double eu = polyU[k1] - polyU[k];
        double ev = polyV[k1] - polyV[k];
        double len2 = eu*eu + ev*ev;
        if (len2 < 1e-12) continue;
        double t = ((pu - polyU[k]) * eu + (pv - polyV[k]) * ev) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        double dx = pu - (polyU[k] + t*eu);
        double dy = pv - (polyV[k] + t*ev);
        double d = sqrt(dx*dx + dy*dy);
        if (d < min_d) { min_d = d; best_e = k; best_t = t; }
    }
    if (out_e) *out_e = best_e;
    if (out_t) *out_t = best_t;
    return isfinite(min_d) ? min_d : 0.0;
}

// Proper segment intersection (UV). Used by the corner-insertion simulation's
// self-intersection guard (#25).
static bool seg_cross_uv(double ax, double ay, double bx, double by,
                         double cx, double cy, double dx, double dy) {
    double d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    double d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
    double d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
    double d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}
// Does the UV polygon (lu,lv,ln) self-intersect? O(n²) over non-adjacent edges.
static bool poly_self_int_uv(const double* lu, const double* lv, int ln) {
    for (int i = 0; i < ln; i++) {
        int i1 = (i+1) % ln;
        for (int j = i+1; j < ln; j++) {
            int j1 = (j+1) % ln;
            if (i == j || i1 == j || i == j1 || i1 == j1) continue;  // adjacent/shared vertex
            if (seg_cross_uv(lu[i],lv[i], lu[i1],lv[i1], lu[j],lv[j], lu[j1],lv[j1])) return true;
        }
    }
    return false;
}

// ── Mutable polygon path (file-scope so helpers can take it) ──────
// Cloned from the input polygon_paths into a mutable form (R.0b prunes
// redundant ridge midpoints; the R.1 chase rewrites ids on merge). These
// paths are the SINGLE SOURCE OF TRUTH for per-corner plane membership
// (see planes_of below + design doc §2.4 DECISION UPDATE 2026-06-19).
typedef struct {
    uint32_t plane_id;
    uint32_t* ids;
    uint32_t  n;
    uint32_t  cap;
    bool      changed;
} poly_t;

// ── T7 SPLIT growth-safety: poly_reserve (§4.3a, MG1) ─────────────────────────
// CAPACITY-only grow of a poly_t's ids[] to at least `need` (never touches
// p->n or content — PREPARE-phase safe, see §4.3c). Honors poly_t.cap, which
// until T7 was SET at clone (:~1113) but never READ (the path only ever shrank).
// `need > cap` → realloc to max(need, cap*2). On OOM returns false with the
// original p->ids INTACT, so the caller aborts cleanly (nothing mutated). The
// COMMIT memmove that opens the insert slot relies on this having run first.
static bool poly_reserve(poly_t* p, uint32_t need) {
    if (need <= p->cap) return true;
    uint32_t newcap = p->cap ? p->cap * 2 : 4;
    if (newcap < need) newcap = need;
    uint32_t* nb = (uint32_t*)safe_realloc_array(p->ids, newcap, sizeof(uint32_t));
    if (!nb) return false;           // original p->ids left intact
    p->ids = nb;
    p->cap = newcap;
    return true;
}

// ── Cost-shape saturator ──────────────────────────────────────────


static inline double saturate(double x_sq, double sigma_sq, int shape) {
    switch (shape) {
        case 1: { // huber
            if (x_sq <= sigma_sq) return 0.5 * (x_sq / sigma_sq);
            double x = sqrt(x_sq), sig = sqrt(sigma_sq);
            double v = 0.5 + (x - sig) / (3.0 * sig);
            return v > 1.0 ? 1.0 : v;
        }
        case 2: { // linear
            double v = sqrt(x_sq / sigma_sq);
            return v > 1.0 ? 1.0 : v;
        }
        case 3: { // quadratic-clamp
            double v = x_sq / sigma_sq;
            return v > 1.0 ? 1.0 : v;
        }
        case 0: default: // gaussian
            return 1.0 - exp(-x_sq / (2.0 * sigma_sq));
    }
}


// ── Per-corner state ──────────────────────────────────────────────

typedef struct corner_state {
    double  point[3];
    uint8_t dof;
    double  axis_a[3];        // DOF=1: ridge dir; DOF=2: e1 in plane
    double  axis_b[3];        // DOF=2: e2 in plane
    bool    has_axis_a, has_axis_b;
    bool    moved;
    bool    alive;            // R.2/R.3 set to false when merged/removed
    uint32_t n_planes;
    // A2 (§4.6): in-loop-maintained geometric on-ridge tag set (P4). A realloc-
    // grown set: seeded from corners_in[].ridge_ids (reality-intersected),
    // additively unioned on MERGE_CORNERS, one-way (no removal —
    // a DOF-held corner can't drift off its ridge). Drives the min-DOF coupling.
    uint32_t* ridge_ids;
    uint32_t  n_ridge_ids;
} corner_state_t;

// Per-(corner, polygon) context — used by angle + neighbor cost terms.
typedef struct corner_context {
    uint32_t plane_id;
    uint32_t corner_idx;   // index into corners array
    uint32_t prev_idx;
    uint32_t next_idx;
    double   frozen_target_rad;   // 0 → no target (free); else target angle
    bool     has_target;
} corner_context_t;

// ctx_reserve (§4.3a, MG1): CAPACITY-only grow of the global ctxs[] pool to at
// least `need` records, mirroring poly_reserve. Used in the SPLIT PREPARE phase
// so the COMMIT-phase REBUILD_CTXS cannot overflow. On OOM returns false with
// *ctxs / *cap left INTACT → the caller REJECTs the SPLIT cleanly (no canonical
// store touched). The CTX_RESERVE macro (inside R.1, with the r1_ctx_oom label)
// is the in-loop convenience; this is the fallible-return form for PREPARE.
static bool ctx_reserve(corner_context_t** ctxs, size_t* cap, size_t need) {
    if (need <= *cap) return true;
    size_t newcap = *cap ? *cap * 2 : 4;
    if (newcap < need) newcap = need;
    corner_context_t* nb = (corner_context_t*)safe_realloc_array(*ctxs, newcap, sizeof(corner_context_t));
    if (!nb) return false;           // original pool intact
    *ctxs = nb;
    *cap  = newcap;
    return true;
}

// ── MERGE_CORNER pre-commit self-intersection guard (T5, M5 tier-1) ──
//
// Build the WOULD-BE ring of one plane's polygon path with `lose_id`
// rewritten to `surv_id` and the survivor placed at `surv_pos`, project it
// into the plane's UV basis, locate the survivor vertex, and ask the local
// O(n) edge-cross test (T2) whether the survivor's two incident edges would
// cross any non-adjacent ring edge. Run PRE-COMMIT (before MERGE_CORNERS),
// so a `true` return lets the caller reject with nothing mutated.
//
// Mirrors MERGE_CORNERS' own consecutive+wrap dedup on the substituted ids
// (the survivor may already be elsewhere in this ring, or adjacent to the
// loser) so the would-be ring matches what the merge will actually commit.
// Returns false (no cross) for degenerate rings (n<4 after dedup) — those are
// out of v7_local_edges_cross_uv's domain and the merge collapses them anyway.
// Scratch buffers must each hold >= path_n entries.
static bool merge_would_self_intersect(
        const uint32_t* path_ids, uint32_t path_n,
        uint32_t lose_id, uint32_t surv_id, const double surv_pos[3],
        const id_map_t* corner_map, const corner_state_t* cs,
        const plane_uv_basis_t* basis,
        uint32_t* id_scratch, double* u_scratch, double* v_scratch)
{
    // 1) substitute lose_id→surv_id into a scratch id list.
    uint32_t m = 0;
    for (uint32_t i = 0; i < path_n; i++) {
        uint32_t id = path_ids[i];
        if (id == lose_id) id = surv_id;
        id_scratch[m++] = id;
    }
    // 2) consecutive dedup (same idiom as MERGE_CORNERS), wrap-aware.
    uint32_t w = 0;
    for (uint32_t r = 0; r < m; r++)
        if (w == 0 || id_scratch[w-1] != id_scratch[r]) id_scratch[w++] = id_scratch[r];
    if (w >= 2 && id_scratch[0] == id_scratch[w-1]) w--;
    if (w < 4) return false;   // degenerate ring → out of the O(n) test's domain
    // 3) project to UV; locate the survivor index. (Survivor at surv_pos;
    //    every other corner at its current cs[].point.)
    int surv_idx = -1;
    for (uint32_t i = 0; i < w; i++) {
        uint32_t ci = id_lk(corner_map, id_scratch[i]);
        const double* P = (id_scratch[i] == surv_id) ? surv_pos
                        : (ci != UINT32_MAX ? cs[ci].point : surv_pos);
        if (id_scratch[i] == surv_id) surv_idx = (int)i;
        project_uv(basis, P[0], P[1], P[2], &u_scratch[i], &v_scratch[i]);
    }
    if (surv_idx < 0) return false;   // survivor not in this ring → nothing to test
    return v7_local_edges_cross_uv(u_scratch, v_scratch, (int)w, surv_idx);
}

// ── T7 SPLIT pre-commit self-intersection guard (M5 tier-1, B's UV) ──
//
// Build the WOULD-BE ring of plane B's path with `ins_id` INSERTED at position
// `ins_pos` (between the existing ids[ins_pos-1] and ids[ins_pos]), project it
// into B's UV basis, locate the inserted vertex, and ask the local O(n)
// edge-cross test whether the inserted vertex's two incident edges would cross
// any non-adjacent ring edge. Run PRE-COMMIT (before the path memmove), so a
// `true` return lets the caller REJECT with nothing mutated. The inserted
// corner sits at `ins_pos_3d` (cA's current position — it does NOT move, it is
// already ON the edge by construction). Scratch buffers must each hold >=
// path_n+1 entries. Returns false for degenerate rings (n<4) — the local test's
// domain — and on a missing-id lookup (nothing to test). Mirrors the dedup
// idiom is NOT needed: an INSERT never creates consecutive duplicates unless
// ins_id already equals a neighbour, which cond-A's bracket precludes.
static bool split_would_self_intersect(
        const uint32_t* path_ids, uint32_t path_n,
        uint32_t ins_id, uint32_t ins_pos, const double ins_pos_3d[3],
        const id_map_t* corner_map, const corner_state_t* cs,
        const plane_uv_basis_t* basis,
        uint32_t* id_scratch, double* u_scratch, double* v_scratch)
{
    // 1) build the would-be id ring with ins_id spliced in at ins_pos.
    uint32_t m = 0;
    for (uint32_t i = 0; i < path_n; i++) {
        if (i == ins_pos) id_scratch[m++] = ins_id;
        id_scratch[m++] = path_ids[i];
    }
    if (ins_pos >= path_n) id_scratch[m++] = ins_id;   // append (wrap insert)
    if (m < 4) return false;                            // degenerate → out of domain
    // 2) project to UV; locate the inserted vertex (the FIRST occurrence at
    //    ins_pos — ins_id is, by cond-A construction, new to this ring).
    int ins_idx = -1;
    for (uint32_t i = 0; i < m; i++) {
        uint32_t ci = id_lk(corner_map, id_scratch[i]);
        const double* P;
        if (id_scratch[i] == ins_id && ins_idx < 0) { P = ins_pos_3d; ins_idx = (int)i; }
        else if (ci != UINT32_MAX) P = cs[ci].point;
        else return false;                              // unknown id → cannot test
        project_uv(basis, P[0], P[1], P[2], &u_scratch[i], &v_scratch[i]);
    }
    if (ins_idx < 0) return false;
    return v7_local_edges_cross_uv(u_scratch, v_scratch, (int)m, ins_idx);
}

// Perpendicular distance from point `p` to the 3D SEGMENT a→b (clamped to the
// segment ends, unlike ridge_proj which uses the infinite line). Used by the
// hardened cond-A: cA must lie within τ_in of the actual B edge (u→v), not just
// of the infinite ridge line that the bracket test uses.
static double pt_seg_dist_3d(const double p[3], const double a[3], const double b[3]) {
    double abx = b[0]-a[0], aby = b[1]-a[1], abz = b[2]-a[2];
    double apx = p[0]-a[0], apy = p[1]-a[1], apz = p[2]-a[2];
    double ab2 = abx*abx + aby*aby + abz*abz;
    double t = (ab2 > 1e-18) ? (apx*abx + apy*aby + apz*abz) / ab2 : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    double dx = apx - t*abx, dy = apy - t*aby, dz = apz - t*abz;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

// ── Helpers ───────────────────────────────────────────────────────


static double corner_angle_rad(const double c[3], const double a[3], const double b[3]) {
    double d1x = a[0] - c[0], d1y = a[1] - c[1], d1z = a[2] - c[2];
    double d2x = b[0] - c[0], d2y = b[1] - c[1], d2z = b[2] - c[2];
    double l1 = sqrt(d1x*d1x + d1y*d1y + d1z*d1z);
    double l2 = sqrt(d2x*d2x + d2y*d2y + d2z*d2z);
    if (l1 < EPS) l1 = 1.0;
    if (l2 < EPS) l2 = 1.0;
    double dot = (d1x*d2x + d1y*d2y + d1z*d2z) / (l1 * l2);
    if (dot > 1.0) dot = 1.0; else if (dot < -1.0) dot = -1.0;
    return acos(dot);
}

// ── R.4 ridge-merge geometry helpers ─────────────────────────────
//
// Project point p onto the infinite ridge line (point rp, unit dir rd):
// out_t = signed arc-length along rd; out_perp = perpendicular distance.
static inline void ridge_proj(const double p[3], const double rp[3], const double rd[3],
                                double* out_t, double* out_perp) {
    double wx = p[0] - rp[0], wy = p[1] - rp[1], wz = p[2] - rp[2];
    double t = wx*rd[0] + wy*rd[1] + wz*rd[2];
    double px = rp[0] + t*rd[0], py = rp[1] + t*rd[1], pz = rp[2] + t*rd[2];
    double dx = p[0] - px, dy = p[1] - py, dz = p[2] - pz;
    *out_t = t;
    *out_perp = sqrt(dx*dx + dy*dy + dz*dz);
}

// ── A2 (§4.6): ridge-geometry record + ridge-DOF helper ───────────
//
// One per input ridge: line point (= from), unit direction, segment
// length (tmax; <0 → degenerate/skip), and the two plane ids. Built
// ONCE, hoisted above the R.0 DOF-tag pass so DOF coupling can read it.
typedef struct ridge_geom {
    double rp[3];      // ridge line point (= from)
    double rd[3];      // unit direction
    double tmax;       // segment length; <0 → skip
    uint32_t plane_a, plane_b;
    uint8_t  type;     // 1=ridge 2=hip 3=valley 4=coplanar 5=step (I1, §9)
} ridge_geom_t;

// make-shared ridge-type allowlist (I1, §9): {ridge,hip,valley} are roof–roof
// creases; step (=roof–wall eave) and coplanar (=degenerate near-coplanar seam,
// formerly "flashing") are EXCLUDED — they must not be snapped onto / shared. Used
// by the in-loop snap-in, ridges_incident_to, and the M1 share-guard.
#define RIDGE_TYPE_ALLOWED(t) ((t) == 1 || (t) == 2 || (t) == 3)

// Type 4 = COPLANAR (near-coplanar planes, dihedral < 10° in topology). Its crease
// line nA×nB is ill-conditioned, so it must NOT constrain DOF. NOTE: this is a
// type-4-ONLY exclusion, NOT RIDGE_TYPE_ALLOWED — a step (type 5, roof–wall eave) is
// a real, well-conditioned crease that legitimately pins DOF; only the near-coplanar
// line is degenerate. make-shared excludes step for a topology reason, DOF does not.
#define RIDGE_TYPE_COPLANAR 4u

// push R into ridge_ids[] if not already present (one-way additive set).
// *n / *arr grow by realloc. Returns false
// only on OOM (caller aborts). τ-intersection is the caller's job.
static inline bool ridge_ids_push_unique(uint32_t** arr, uint32_t* n, uint32_t R) {
    for (uint32_t i = 0; i < *n; i++) if ((*arr)[i] == R) return true;
    uint32_t* nb = (uint32_t*)safe_realloc_array(*arr, (*n) + 1, sizeof(uint32_t));
    if (!nb) return false;
    *arr = nb; (*arr)[(*n)++] = R;
    return true;
}

// Reality-intersected ridge-DOF (A2 min-DOF rule, §4.6). Over a corner's
// tagged ridge set, a tag R "counts" iff rg[R].tmax>0 AND the corner
// projects onto R within τ_in perp AND its arc-length t ∈ [0,tmax]. Then:
//   ridge_dof = {>=2 surviving → 0, ==1 → 1, ==0 → 2}.
// On exactly-1 survivor, *out_bind_ri = that ridge index (DOF-1 axis source).
// rg may be NULL (n_ridges==0) → returns 2 (no constraint). Pure, no NaN
// (ridge_proj is sqrt of a sum-of-squares; degenerate rd guarded by tmax>0).
static int ridge_dof_for_corner(const ridge_geom_t* rg, size_t n_ridges,
                                const uint32_t* ridge_ids, uint32_t n_ridge_ids,
                                const double pt[3], double tau_in,
                                size_t* out_bind_ri) {
    if (out_bind_ri) *out_bind_ri = (size_t)-1;
    if (!rg || n_ridge_ids == 0) return 2;
    uint32_t survivors = 0;
    size_t last_ri = (size_t)-1;
    for (uint32_t i = 0; i < n_ridge_ids; i++) {
        size_t ri = ridge_ids[i];
        if (ri >= n_ridges) continue;          // out-of-range tag → ignore
        if (rg[ri].tmax <= 0.0) continue;     // degenerate ridge → ignore tag
        if (rg[ri].type == RIDGE_TYPE_COPLANAR) continue; // coplanar (near-coplanar) seam:
                                              // nA×nB→0, crease direction is noise → does
                                              // NOT constrain DOF. (step type 5 is KEPT.) F2
        double t, perp;
        ridge_proj(pt, rg[ri].rp, rg[ri].rd, &t, &perp);
        if (perp > tau_in) continue;
        if (t < 0.0 || t > rg[ri].tmax) continue;
        survivors++;
        last_ri = ri;
    }
    if (survivors >= 2) return 0;
    if (survivors == 1) { if (out_bind_ri) *out_bind_ri = last_ri; return 1; }
    return 2;
}

// Clamp a DOF-1 slide candidate so the corner's parameter t along its binding
// ridge stays within the clipped segment [0,tmax]. Without this an unanchored
// DOF-1 corner whose WSD cost gradient points along the crease walks off the
// crease one nudge at a time (dahouet RE2+P9C5+RE39 slid ~85 m off the roof).
// The binding ridge is the tagged, allowed (non-coplanar), non-degenerate ridge
// whose direction is PARALLEL to axis_a (axis_a was set from that ridge's rd) —
// found by parallelism, NOT ridge_dof_for_corner, so it still resolves once a
// corner has slid off the segment (ridge_dof_for_corner requires in-segment and
// would stop matching). `cand` is adjusted IN PLACE: if its t falls outside
// [0,tmax] it is shifted back ALONG rd to the nearest endpoint — perp offset
// preserved (bounds tangential travel only; does NOT re-project onto the line,
// that is the snap-in's job). No-op when no binding ridge is found. Shared by
// the single-corner sampler and the joint two-corner sweep.
static void clamp_dof1_slide(const corner_state_t* c, const ridge_geom_t* rg,
                             size_t n_ridges, double cand[3]) {
    if (!c->has_axis_a) return;
    size_t sb = (size_t)-1;
    for (uint32_t i = 0; i < c->n_ridge_ids; i++) {
        size_t ri = c->ridge_ids[i];
        if (ri >= n_ridges) continue;
        if (rg[ri].tmax <= 0.0) continue;
        if (rg[ri].type == RIDGE_TYPE_COPLANAR) continue;
        double d = rg[ri].rd[0]*c->axis_a[0] + rg[ri].rd[1]*c->axis_a[1]
                 + rg[ri].rd[2]*c->axis_a[2];
        if (d > 0.999 || d < -0.999) { sb = ri; break; }
    }
    if (sb == (size_t)-1) return;
    double t, perp;
    ridge_proj(cand, rg[sb].rp, rg[sb].rd, &t, &perp);
    double t_cl = t < 0.0 ? 0.0 : (t > rg[sb].tmax ? rg[sb].tmax : t);
    if (t_cl == t) return;
    double dt = t_cl - t;
    cand[0] += dt * rg[sb].rd[0];
    cand[1] += dt * rg[sb].rd[1];
    cand[2] += dt * rg[sb].rd[2];
}

// planes_of: canonical membership of corner ci = { plane_id of every polys[] path
// (n>=3) whose ids contain corners_in[ci].id }. The paths are the SINGLE SOURCE OF
// TRUTH; this is a pure function of them (no stored copy). Body == the old
// RECONCILE_PLANES_DOWN scan. See design doc §2.4 DECISION UPDATE 2026-06-19.
static uint32_t planes_of(size_t ci, const v7_br_input_corner_t* corners_in,
                          const poly_t* polys, size_t n_polygon_paths,
                          uint32_t* out, uint32_t out_cap) {
    uint32_t id = corners_in[ci].id;
    uint32_t n = 0;
    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        const poly_t* p = &polys[pi];
        if (p->n < 3) continue;
        bool in = false;
        for (uint32_t k = 0; k < p->n; k++)
            if (p->ids[k] == id) { in = true; break; }
        if (!in) continue;
        bool have = false;
        for (uint32_t j = 0; j < n; j++)
            if (out[j] == p->plane_id) { have = true; break; }
        if (have) continue;
        if (n < out_cap) out[n] = p->plane_id;
        n++;
    }
    return n;
}

// Count-only convenience: |planes_of(ci)|. out_cap=0 → pure count (no store).
// Scratch is a fixed local; a corner can belong to at most n_polygon_paths
// planes, but in practice membership is tiny (<= a handful) — 64 is safe.
#define PLANES_OF_SCRATCH 64
static inline uint32_t planes_of_n(size_t ci, const v7_br_input_corner_t* corners_in,
                                   const poly_t* polys, size_t n_polygon_paths) {
    uint32_t scratch[PLANES_OF_SCRATCH];
    return planes_of(ci, corners_in, polys, n_polygon_paths, scratch, PLANES_OF_SCRATCH);
}

// Does corner `ci` belong to plane id `pid`? (derived from the canonical paths)
static inline bool corner_has_plane(size_t ci, uint32_t pid,
                                    const v7_br_input_corner_t* corners_in,
                                    const poly_t* polys, size_t n_polygon_paths) {
    uint32_t scratch[PLANES_OF_SCRATCH];
    uint32_t n = planes_of(ci, corners_in, polys, n_polygon_paths, scratch, PLANES_OF_SCRATCH);
    for (uint32_t i = 0; i < n; i++) if (scratch[i] == pid) return true;
    return false;
}

// Do corners `a` and `b` share any plane id? (derived from the canonical paths)
static inline bool corners_share_plane(size_t a, size_t b,
                                       const v7_br_input_corner_t* corners_in,
                                       const poly_t* polys, size_t n_polygon_paths) {
    uint32_t sa[PLANES_OF_SCRATCH], sb[PLANES_OF_SCRATCH];
    uint32_t na = planes_of(a, corners_in, polys, n_polygon_paths, sa, PLANES_OF_SCRATCH);
    uint32_t nb = planes_of(b, corners_in, polys, n_polygon_paths, sb, PLANES_OF_SCRATCH);
    for (uint32_t i = 0; i < na; i++)
        for (uint32_t j = 0; j < nb; j++)
            if (sa[i] == sb[j]) return true;
    return false;
}

// ── L1: orphan-kill ─────────────────────────────────────────────────────────
// Marks dead every still-alive corner that is in NO live polygon path (n>=3) —
// an "alive but orphaned" corner, e.g. a bystander left behind when a MERGE
// dedup collapsed its last path to n<3 (the corner-197 class). Membership is
// path-derived (planes_of), so such a corner is ALREADY inert in every in-loop
// scan (corner_has_plane / corners_share_plane / the optimizer / T6 / DROP all
// return "no plane" for it) — this sweep only stops it being EMITTED alive with
// stale data. Idempotent; safe to call repeatedly. The survivor of any merge is
// in ≥1 path, so it is never killed.
static void kill_orphaned_corners(corner_state_t* cs, size_t n_corners,
        const v7_br_input_corner_t* corners_in, const poly_t* polys,
        size_t n_polygon_paths) {
    for (size_t ci = 0; ci < n_corners; ci++) {
        if (!cs[ci].alive) continue;
        uint32_t cid = corners_in[ci].id;
        bool in_any = false;
        for (size_t pi = 0; pi < n_polygon_paths && !in_any; pi++) {
            const poly_t* p = &polys[pi];
            for (uint32_t k = 0; k < p->n; k++)
                if (p->ids[k] == cid) { in_any = true; break; }
        }
        if (!in_any) cs[ci].alive = false;
    }
}

// ── T6: DROP_REDUNDANT_ON_RIDGE — all-polys-agree ridge-cleanup (§4.2) ───────
//
// Removes corner `ci` IFF it is a redundant on-ridge midpoint in EVERY polygon
// that contains it (n>=4), and removes it from ALL of them atomically. Returns
// true on a drop, false otherwise (nothing mutated when false).
//
// "Redundant on-ridge midpoint in a path" = the 3-consecutive-share-ONE-ridge
// test: (prev, curr=ci, next) all carry a common ridge tag, read from the
// MAINTAINED cs[].ridge_ids (NOT corners_in[].ridge_ids) so pre-loop and in-loop
// callers share one tag source. Junction-safe: a corner with >=2 ridge tags can
// still share exactly one with both neighbours, so the test keys on a SINGLE
// shared ridge_id existing — which is precisely a colinear-on-one-crease run; a
// genuine kink (where curr is on two creases but neighbours sit on different
// ones) fails to find a common ridge and is KEPT.
//
// All-polys-agree (§4.2): a corner straight in one polygon can be the ENDPOINT
// of a shared edge in another — dropping it there destroys a real shared edge.
// So we KEEP unless every containing path agrees. A containing path with n<4
// (a collapsed-around ring) can't host the drop → treat as disagreement → KEEP
// (conservative).
//
// Membership is path-derived (Phase B): the splices ARE the membership update —
// NO planes_dyn refresh. After a successful drop, cs[ci].alive=false (ci is gone
// from every path by construction). Collateral orphans (a NEIGHBOUR left in no
// path when this splice collapses a ring to n<3) are swept by
// kill_orphaned_corners (L1) — here post-batch and again post-loop.
static bool try_drop_redundant_on_ridge(size_t ci, const v7_br_input_corner_t* corners_in,
        poly_t* polys, size_t n_polygon_paths, const id_map_t* corner_map,
        corner_state_t* cs) {
    if (cs[ci].n_ridge_ids == 0) return false;   // not on any ridge → never redundant-on-ridge
    uint32_t curr_id = corners_in[ci].id;
    bool any_containing = false;
    // Pass 1 (read-only): unanimity test over every containing path.
    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        const poly_t* p = &polys[pi];
        if (p->n < 3) continue;
        // find ci's position in this path (it may appear at most once after dedup)
        uint32_t pos = UINT32_MAX;
        for (uint32_t k = 0; k < p->n; k++) if (p->ids[k] == curr_id) { pos = k; break; }
        if (pos == UINT32_MAX) continue;   // ci not in this path
        any_containing = true;
        // A containing path with n<4 can't host the drop → disagreement → KEEP.
        if (p->n < 4) return false;
        uint32_t prev_id = p->ids[(pos + p->n - 1) % p->n];
        uint32_t next_id = p->ids[(pos + 1) % p->n];
        uint32_t prev_ci = id_lk(corner_map, prev_id);
        uint32_t next_ci = id_lk(corner_map, next_id);
        if (prev_ci == UINT32_MAX || next_ci == UINT32_MAX) return false;  // can't judge → KEEP
        // 3-consecutive-share-ONE-ridge test against the MAINTAINED cs[].ridge_ids.
        bool shared = false;
        for (uint32_t a = 0; a < cs[ci].n_ridge_ids && !shared; a++) {
            uint32_t rid = cs[ci].ridge_ids[a];
            bool in_prev = false, in_next = false;
            for (uint32_t b = 0; b < cs[prev_ci].n_ridge_ids; b++)
                if (cs[prev_ci].ridge_ids[b] == rid) { in_prev = true; break; }
            for (uint32_t b = 0; b < cs[next_ci].n_ridge_ids; b++)
                if (cs[next_ci].ridge_ids[b] == rid) { in_next = true; break; }
            if (in_prev && in_next) shared = true;
        }
        if (!shared) return false;   // this path disagrees → KEEP
    }
    if (!any_containing) return false;   // ci in no path → nothing to drop here

    // Unanimous: splice ci from EVERY path (memmove + consecutive/wrap dedup,
    // mirroring the MERGE_CORNERS idiom). Membership update IS this rewrite.
    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        poly_t* p = &polys[pi];
        bool touched = false;
        uint32_t w = 0;
        for (uint32_t r = 0; r < p->n; r++) {
            if (p->ids[r] == curr_id) { touched = true; continue; }   // drop ci
            p->ids[w++] = p->ids[r];
        }
        if (!touched) continue;
        // consecutive dedup (wrap-aware) — the splice can adjoin equal neighbours.
        uint32_t w2 = 0;
        for (uint32_t r = 0; r < w; r++) {
            if (w2 == 0 || p->ids[w2-1] != p->ids[r]) p->ids[w2++] = p->ids[r];
        }
        if (w2 >= 2 && p->ids[0] == p->ids[w2-1]) w2--;
        p->n = (w2 >= 3) ? w2 : 0;
        p->changed = true;
    }
    cs[ci].alive = false;   // ci is gone from every path by construction
    return true;
}

// ── ridges_incident_to (T4 §4.5(3)) — the corner→ridges locality helper ──
//
// Fills `out[]` with the set of ALLOW-LISTED ridge indices relevant to corner
// `ci`, returning the count. Two relevances, deduped:
//   (1) C's existing on-ridge TAGS  (cs[ci].ridge_ids — ridges C is already on)
//   (2) NEWLY-REACHED ridges: scan only the ridges touching C's planes (via the
//       plane→ridges CSR index), keep those passing rg[R].tmax>0 ∧ perp≤τ_in ∧
//       t∈[0,tmax]. This catches a FREE corner that has SLID onto a crease it
//       isn't tagged on yet (the snap trigger).
// Output is filtered to RIDGE_TYPE_ALLOWED (I1, §9) — step/coplanar excluded.
// `out` must hold up to (n_ridge_ids + Σ ridges-per-plane) entries; callers
// size it to n_ridges (a safe upper bound). Pure / read-only.
static int ridges_incident_to(size_t ci,
                              const ridge_geom_t* rg, size_t n_ridges,
                              const uint32_t* ridge_ids, uint32_t n_ridge_ids,
                              const double pt[3], double tau_in,
                              const uint32_t* planes_ci, uint32_t planes_n_ci,
                              const id_map_t* plane_map,
                              const uint32_t* ridx_off, const uint32_t* ridx,
                              uint32_t* out) {
    (void)ci;
    int n_out = 0;
    // (1) already-tagged ridges (allow-listed).
    for (uint32_t i = 0; i < n_ridge_ids; i++) {
        uint32_t R = ridge_ids[i];
        if (R >= n_ridges) continue;
        if (!RIDGE_TYPE_ALLOWED(rg[R].type)) continue;
        bool dup = false;
        for (int k = 0; k < n_out; k++) if (out[k] == R) { dup = true; break; }
        if (!dup) out[n_out++] = R;
    }
    // (2) newly-reached ridges via the plane→ridges index.
    if (rg && ridx_off && ridx) {
        for (uint32_t p = 0; p < planes_n_ci; p++) {
            uint32_t pi = id_lk(plane_map, planes_ci[p]);
            if (pi == UINT32_MAX) continue;
            for (uint32_t s = ridx_off[pi]; s < ridx_off[pi + 1]; s++) {
                uint32_t R = ridx[s];
                if (R >= n_ridges) continue;
                if (rg[R].tmax <= 0.0) continue;
                if (!RIDGE_TYPE_ALLOWED(rg[R].type)) continue;
                double t, perp;
                ridge_proj(pt, rg[R].rp, rg[R].rd, &t, &perp);
                if (perp > tau_in) continue;
                if (t < 0.0 || t > rg[R].tmax) continue;
                bool dup = false;
                for (int k = 0; k < n_out; k++) if (out[k] == R) { dup = true; break; }
                if (!dup) out[n_out++] = R;
            }
        }
    }
    return n_out;
}

// mulberry32 PRNG step — see https://en.wikipedia.org/wiki/Mulberry32.
// Equivalent to v2's jointRng when jointMoveSeed != 0 (matches the JS
// Math.imul-based path bit-for-bit, since uint32 multiplication wraps
// mod 2^32 in C as well).
static inline double mulberry32_next(uint32_t* state) {
    if (*state == 0) {
        return (double)rand() / (double)RAND_MAX;
    }
    *state += 0x6D2B79F5u;
    uint32_t t = *state;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (double)((t ^ (t >> 14))) / 4294967296.0;
}

static double nearest_canonical_target_deg(double angle_deg, const double* targets, uint32_t n_targets,
                                              double* out_deviation_deg) {
    double best = angle_deg, best_dev = INFINITY;
    for (uint32_t i = 0; i < n_targets; i++) {
        double dev = fabs(angle_deg - targets[i]);
        if (dev < best_dev) { best_dev = dev; best = targets[i]; }
    }
    *out_deviation_deg = best_dev;
    return best;
}

// ── R.7 pair-cost helpers ─────────────────────────────────────────
//
// All three take 4 corner positions and a 2σ² + cost-shape, returning
// a saturated value in [0, 1]. Math matches v2 boundary-regularize.js
// pair*Cost functions exactly (same closed-form eigenvalue derivations).

// Horizontal-coplanar: variance of 4 y-coords / 4 → saturated.
static inline double pair_hcop_cost(const double pa[3], const double pb[3],
                                      const double pc[3], const double pd[3],
                                      double two_sigma_sq, int shape) {
    double mean = (pa[1] + pb[1] + pc[1] + pd[1]) * 0.25;
    double d1 = pa[1] - mean, d2 = pb[1] - mean;
    double d3 = pc[1] - mean, d4 = pd[1] - mean;
    double var_sum = d1*d1 + d2*d2 + d3*d3 + d4*d4;
    return saturate(var_sum * 0.25, two_sigma_sq, shape);
}

// Vertical-coplanar: 2D best-fit line residual in xz.
// sum_perp² = min eigenvalue of [[sxx, sxz], [sxz, szz]] = (trace - √(trace² - 4·det)) / 2
static inline double pair_vcop_cost(const double pa[3], const double pb[3],
                                      const double pc[3], const double pd[3],
                                      double two_sigma_sq, int shape) {
    double mx = (pa[0] + pb[0] + pc[0] + pd[0]) * 0.25;
    double mz = (pa[2] + pb[2] + pc[2] + pd[2]) * 0.25;
    double dx1 = pa[0] - mx, dx2 = pb[0] - mx, dx3 = pc[0] - mx, dx4 = pd[0] - mx;
    double dz1 = pa[2] - mz, dz2 = pb[2] - mz, dz3 = pc[2] - mz, dz4 = pd[2] - mz;
    double sxx = dx1*dx1 + dx2*dx2 + dx3*dx3 + dx4*dx4;
    double szz = dz1*dz1 + dz2*dz2 + dz3*dz3 + dz4*dz4;
    double sxz = dx1*dz1 + dx2*dz2 + dx3*dz3 + dx4*dz4;
    double trace = sxx + szz;
    double det   = sxx * szz - sxz * sxz;
    double disc  = trace * trace - 4.0 * det;
    double root  = disc > 0 ? sqrt(disc) : 0.0;
    double sum_perp_sq = (trace - root) * 0.5;
    return saturate(sum_perp_sq * 0.25, two_sigma_sq, shape);
}

// Collinear: 3D best-fit line residual.
// sum_perp² = trace − λ_max of 3×3 covariance. λ_max via Smith trig
// method on the symmetric matrix:
//   q = trace/3, p² = (a² + b² + c² + 2(sxy² + sxz² + syz²)) / 6,
//   det(B) where B = (cov − q·I)/p,
//   φ = acos(clamp(det(B)/2)) / 3, λ_max = q + 2p·cos(φ).
static inline double pair_collinear_cost(const double pa[3], const double pb[3],
                                            const double pc[3], const double pd[3],
                                            double two_sigma_sq, int shape) {
    double mx = (pa[0] + pb[0] + pc[0] + pd[0]) * 0.25;
    double my = (pa[1] + pb[1] + pc[1] + pd[1]) * 0.25;
    double mz = (pa[2] + pb[2] + pc[2] + pd[2]) * 0.25;
    double dx1 = pa[0] - mx, dx2 = pb[0] - mx, dx3 = pc[0] - mx, dx4 = pd[0] - mx;
    double dy1 = pa[1] - my, dy2 = pb[1] - my, dy3 = pc[1] - my, dy4 = pd[1] - my;
    double dz1 = pa[2] - mz, dz2 = pb[2] - mz, dz3 = pc[2] - mz, dz4 = pd[2] - mz;
    double sxx = dx1*dx1 + dx2*dx2 + dx3*dx3 + dx4*dx4;
    double syy = dy1*dy1 + dy2*dy2 + dy3*dy3 + dy4*dy4;
    double szz = dz1*dz1 + dz2*dz2 + dz3*dz3 + dz4*dz4;
    double sxy = dx1*dy1 + dx2*dy2 + dx3*dy3 + dx4*dy4;
    double sxz = dx1*dz1 + dx2*dz2 + dx3*dz3 + dx4*dz4;
    double syz = dy1*dz1 + dy2*dz2 + dy3*dz3 + dy4*dz4;
    double trace = sxx + syy + szz;
    if (trace < 1e-12) return saturate(0.0, two_sigma_sq, shape);
    double q = trace / 3.0;
    double a = sxx - q, b = syy - q, c = szz - q;
    double p_sq = (a*a + b*b + c*c + 2.0*(sxy*sxy + sxz*sxz + syz*syz)) / 6.0;
    double lambda_max;
    if (p_sq < 1e-18) {
        lambda_max = q;  // triple eigenvalue
    } else {
        double p = sqrt(p_sq);
        double det_B = (a*b*c + 2.0*sxy*sxz*syz - a*syz*syz - b*sxz*sxz - c*sxy*sxy) / (p*p*p);
        double r = det_B * 0.5;
        if (r > 1.0) r = 1.0; else if (r < -1.0) r = -1.0;
        double phi = acos(r) / 3.0;
        lambda_max = q + 2.0 * p * cos(phi);
    }
    double sum_perp_sq = trace - lambda_max;
    if (sum_perp_sq < 0) sum_perp_sq = 0;
    return saturate(sum_perp_sq * 0.25, two_sigma_sq, shape);
}

// Pair-cost function-pointer signature.
typedef double (*pair_cost_fn_t)(const double pa[3], const double pb[3],
                                  const double pc[3], const double pd[3],
                                  double two_sigma_sq, int shape);

// One pairwise edge — corner-id pair (a < b) deduplicated across polygons.
typedef struct pairwise_edge {
    uint32_t a_ci;   // corner index (not id)
    uint32_t b_ci;
} pairwise_edge_t;

// Per-corner-aggregate pairwise cost: average over ACTIVE pair costs
// (pc < ACTIVE_THRESH) for pairs where at least one edge is incident
// to the moved corner. Internal pairs (both incident) counted once;
// external pairs (one incident, one not) counted once. Returns 1.0
// when zero active partners — see v2 boundary-regularize.js for the
// rationale on this fallback.
//
// `incident_mark` is a caller-owned bool[] sized n_pw; the helper sets
// it to true for incident edges, scans pairs, then resets to false.
#define R7_ACTIVE_THRESH 0.95

static double pairwise_cost_for_corner(
    const corner_state_t* cs,
    const pairwise_edge_t* pw, uint32_t n_pw,
    const uint32_t* incident_idxs, uint32_t n_incident,
    bool* incident_mark,
    pair_cost_fn_t pair_cost,
    double two_sigma_sq, int shape
) {
    if (n_pw < 2 || n_incident == 0) return 0.0;
    for (uint32_t i = 0; i < n_incident; i++) {
        incident_mark[incident_idxs[i]] = true;
    }
    double active_sum = 0.0;
    uint32_t active_count = 0;
    // Internal pairs (both incident).
    for (uint32_t i = 0; i < n_incident; i++) {
        for (uint32_t j = i + 1; j < n_incident; j++) {
            uint32_t ei = incident_idxs[i], ej = incident_idxs[j];
            const double* pa = cs[pw[ei].a_ci].point;
            const double* pb = cs[pw[ei].b_ci].point;
            const double* pc = cs[pw[ej].a_ci].point;
            const double* pd = cs[pw[ej].b_ci].point;
            double v = pair_cost(pa, pb, pc, pd, two_sigma_sq, shape);
            if (v < R7_ACTIVE_THRESH) { active_sum += v; active_count++; }
        }
    }
    // External pairs (one incident, one not).
    for (uint32_t i = 0; i < n_incident; i++) {
        uint32_t ei = incident_idxs[i];
        const double* pa = cs[pw[ei].a_ci].point;
        const double* pb = cs[pw[ei].b_ci].point;
        for (uint32_t j = 0; j < n_pw; j++) {
            if (incident_mark[j]) continue;
            const double* pc = cs[pw[j].a_ci].point;
            const double* pd = cs[pw[j].b_ci].point;
            double v = pair_cost(pa, pb, pc, pd, two_sigma_sq, shape);
            if (v < R7_ACTIVE_THRESH) { active_sum += v; active_count++; }
        }
    }
    for (uint32_t i = 0; i < n_incident; i++) incident_mark[incident_idxs[i]] = false;
    if (active_count == 0) return 1.0;
    return active_sum / (double)active_count;
}

// ── Main compute ──────────────────────────────────────────────────

int v7_br_compute(
    const v7_br_input_plane_t* planes, size_t n_planes,
    const v7_br_input_corner_t* corners_in, size_t n_corners,
    const v7_br_input_edge_t* edges, size_t n_edges,
    const v7_br_input_polygon_path_t* polygon_paths, size_t n_polygon_paths,
    const v7_br_input_ridge_t* ridges, size_t n_ridges,
    const v7_br_input_nearby_per_plane_t* nearby, size_t n_nearby,
    const float* positions, size_t n_points,
    const int16_t* owner,
    const float* point_plane_residual,
    double point_plane_dist_threshold,
    const v7_br_params_t* params,
    v7_br_outputs_t* out
) {
    (void)edges; (void)n_edges;       // unused this commit (R.7 + MR-fit need them)
    // ridges now consumed by R.4 ridge-merge (geometry-gated cross-plane merge).
    if (!params || !out) return -1;
    if (n_planes  > 0 && !planes)        return -1;
    if (n_corners > 0 && !corners_in)    return -1;
    if (n_polygon_paths > 0 && !polygon_paths) return -1;
    if (n_points  > 0 && !positions)     return -1;
    if (n_points  > 0 && !owner)         return -1;
    v7_br_outputs_init(out);
    if (n_corners == 0 || n_planes == 0) return 0;

    const double NUDGE = params->nudge_range_m;
    const double STEP  = params->nudge_step_m;
    const double ANGLE_SNAP_THRESH_RAD = params->angle_snap_thresh_deg * M_PI / 180.0;
    const double ANGLE_SIGMA_RAD       = params->angle_sigma_deg * M_PI / 180.0;
    const double ANGLE_TWO_SIGMA_SQ    = 2.0 * ANGLE_SIGMA_RAD * ANGLE_SIGMA_RAD;
    const double HORIZ_SIGMA_RAD       = params->horizontal_edge_sigma_deg * M_PI / 180.0;
    const double HORIZ_TWO_SIGMA_SQ    = 2.0 * HORIZ_SIGMA_RAD * HORIZ_SIGMA_RAD;
    static const double TARGETS_BASE[]     = { 45, 90, 135, 180 };
    static const double TARGETS_EXTENDED[] = { 30, 45, 60, 90, 120, 135, 150, 180 };
    const double* targets   = params->use_extended_angles ? TARGETS_EXTENDED : TARGETS_BASE;
    const uint32_t n_targets = params->use_extended_angles ? 8u : 4u;

    int rc = -1;
    // Hoisted ridge-geometry precompute (A2 §4.6). Declared here (before any
    // `goto cleanup`) and freed ONCE at `cleanup:` so every exit path — early
    // and main — releases it without a double-free. NULL until built below.
    ridge_geom_t* rg = NULL;
    // plane→ridges index (T4 §4.5(3)): CSR layout. ridx_off[pi..pi+1) slices
    // ridx[] to the ridge indices touching plane index pi (each ridge appends
    // its id to BOTH its plane_a and plane_b). Built once after rg[]; powers
    // ridges_incident_to's "newly reached" scan in O(C's planes × ridges/plane)
    // instead of O(all ridges). NULL until built; freed once at cleanup.
    uint32_t* ridx_off = NULL;   // size n_planes+1
    uint32_t* ridx     = NULL;   // size 2*n_ridges (each ridge in 2 plane slots)
    // T4: scratch for ridges_incident_to (snap-in + guard preview). Declared up
    // here (before any `goto`) so the free at r1_xplane_oom can't hit an
    // uninitialized value when an early OOM jumps past the alloc site below.
    uint32_t* ridge_inc_buf = NULL;
    // T5: scratch for the MERGE_CORNER pre-commit self-int guard (would-be ring
    // ids + UV). Sized to n_corners (a safe upper bound on any path length).
    // Declared here so the free at r1_xplane_oom can't hit uninitialized memory.
    uint32_t* mc_id_scratch = NULL;
    double*   mc_u_scratch  = NULL;
    double*   mc_v_scratch  = NULL;
    // ── Profiling instrumentation (gated by BR_PROFILE env) ─────────
    // Phase + per-metric wall-clock counters. Emit summary at end of compute.
    // ~30 ns per clock_gettime call; negligible vs metric evaluations.
    struct { uint64_t ns; uint64_t calls; } br_p_R0 = {0,0}, br_p_R0b = {0,0},
                                              br_p_R1_total = {0,0}, br_p_R1_score = {0,0},
                                              br_p_R2 = {0,0}, br_p_R3 = {0,0},
                                              br_p_MS = {0,0},  // T0 scaffold: make-shared (SHARE/SPLIT) wall-time; 0 until step 2+
                                              br_m_wsd = {0,0}, br_m_angle = {0,0},
                                              br_m_nbr_angle = {0,0}, br_m_horiz_edge = {0,0},
                                              br_m_hcop = {0,0}, br_m_vcop = {0,0},
                                              br_m_collin = {0,0};
    const bool br_prof_on = (getenv("BR_PROFILE") != NULL);
    // Use CLOCK_THREAD_CPUTIME_ID so preemption / scheduler-jitter from other
    // processes doesn't get attributed to our metrics. Regularize is
    // single-threaded; this clock only ticks while this thread is on-CPU.
    // (CLOCK_MONOTONIC_RAW would also count wall-time spent off-CPU and would
    //  bias hot loops on a loaded box.) Wall time is captured once at
    // step-entry/exit separately for the "user-visible duration" view.
    // `_ts = {0}` is purely for Frama-C: clock_gettime's libc stub doesn't
    // model the *tp write so EVA flags reading _ts.tv_sec as uninitialised
    // — a sure alarm at task #87. The zero-init costs ~8 bytes of stack,
    // gets clobbered by clock_gettime, and removes the FP.
    #define BR_NOW(out)  do { struct timespec _ts = {0}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_ts); \
                              (out) = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec; } while (0)
    #define BR_ACCUM(slot, t_start) do { if (br_prof_on) { uint64_t _t1; BR_NOW(_t1); \
                                          (slot).ns += _t1 - (t_start); (slot).calls++; } } while (0)
    // Separate wall-clock anchors for the whole step (so we can report
    // "X cpu / Y wall" — divergence tells us about preemption / waiting).
    uint64_t br_wall_start = 0;
    if (br_prof_on) {
        struct timespec _ts = {0};  // see BR_NOW macro for the Frama-C rationale
        clock_gettime(CLOCK_MONOTONIC_RAW, &_ts);
        br_wall_start = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
    }

    id_map_t plane_map = {0}, corner_map = {0};
    // #25 corner-insertion growth scaffold: corner-parallel arrays are sized to
    // n_cap so the op can mint new corners; corners_in is rebound to an over-sized
    // MUTABLE copy (the 84 reads stay unchanged). corners_owned is freed once, at
    // the single cleanup: convergence label (free(NULL) safe on early error paths).
    enum { N_INSERT_MAX = 64 };
    const size_t n_cap = n_corners + N_INSERT_MAX;
    v7_br_input_corner_t* corners_owned = NULL;
    size_t n_inserted = 0;   // #25: count of corners minted (new id = max_corner+1+this)
    uint32_t* plane_ins_cnt = NULL;   // #25: per-plane insertion budget counter
    uint32_t max_plane = 0, max_corner = 0;
    for (size_t i = 0; i < n_planes;  i++) if (planes[i].id  > max_plane)  max_plane  = planes[i].id;
    for (size_t i = 0; i < n_corners; i++) if (corners_in[i].id > max_corner) max_corner = corners_in[i].id;
    if (id_map_build(&plane_map,  max_plane  + 1) != 0) goto cleanup;
    if (id_map_build(&corner_map, max_corner + 1 + N_INSERT_MAX) != 0) goto cleanup;
    for (size_t i = 0; i < n_planes;  i++) id_set(&plane_map,  planes[i].id,  (uint32_t)i);
    for (size_t i = 0; i < n_corners; i++) id_set(&corner_map, corners_in[i].id, (uint32_t)i);
    // Rebind corners_in to an over-sized mutable copy (shallow: ridge_ids pointers
    // are caller-owned, never freed here). New corners write into corners_owned[ci].
    corners_owned = (v7_br_input_corner_t*)safe_calloc_array(n_cap, sizeof(*corners_owned));
    if (!corners_owned) goto cleanup;
    memcpy(corners_owned, corners_in, n_corners * sizeof(*corners_owned));
    corners_in = corners_owned;
    plane_ins_cnt = (uint32_t*)safe_calloc_array(n_planes ? n_planes : 1, sizeof(uint32_t));
    if (!plane_ins_cnt) goto cleanup;

    // ── Ridge geometry precompute (HOISTED above R.0 — A2 §4.6) ──────
    // For each ridge: line point = from, unit dir, tmax = |to-from|, plus
    // plane ids. `from`/`to` arrive building-local y-up (wrapper swap), same
    // frame as cs[].point. Degenerate (near-zero) length → tmax<0 (skipped).
    // Built BEFORE R.0 so the DOF-tag pass can couple ridge-DOF (the min-DOF
    // rule); R.4 reuses the same rg[] later. OOM here → plain cleanup (no
    // cs/polys allocated yet).
    const bool br4_trace = (getenv("BR4_TRACE") != NULL) || br_prof_on;
    // A2 DOF-coupling gate: the ridge_ids store + seed are ALWAYS built;
    // only the DOF *effect* (min-DOF, ridge axis) is gated, so the A/B
    // (coupling on vs off) is runnable from one binary. Default ON.
    const char* _dc_env = getenv("BR_DOF_COUPLE");
    const bool br_dof_couple = (_dc_env == NULL) || (_dc_env[0] != '0');
    // T4 snap-in gate (default ON). With BR_SNAP_IN=0 the snap-in EFFECT (the
    // FREE→RIDGE tag write inside the chase) is suppressed, so the loop output
    // is byte-identical to HEAD — proving the new scaffolding (ridges_incident_to
    // + the read-only guard eval) is inert. The guard eval is read-only either
    // way and never mutates state.
    const char* _si_env = getenv("BR_SNAP_IN");
    const bool br_snap_in = (_si_env == NULL) || (_si_env[0] != '0');
    // Position-snap gate (default ON). The snap-in TAGS a corner on-ridge + sets
    // DOF-1 when it drifts within tau_in perp; but DOF-1 only slides PARALLEL — it
    // never PUTS the corner on the crease (design §2: "position lies on the ridge
    // crease, set at every snap event"). So a tagged corner could sit up to tau_in
    // off the ridge (e.g. dahouet P2C11 @25cm), making the shared edge skew off the
    // crease. With br_snap_pos the snap-in also PROJECTS the position onto the
    // binding ridge. BR_SNAP_POS=0 reverts to the tag-only (pre-v16) behaviour.
    const char* _sp_env = getenv("BR_SNAP_POS");
    const bool br_snap_pos = (_sp_env == NULL) || (_sp_env[0] != '0');
    // Slide-clamp gate (default OFF — experimental backstop, BR_SLIDE_CLAMP=1
    // to enable). A DOF-1 corner slides ALONG its binding ridge; the
    // per-iteration sampler steps freely along axis_a with no bound on
    // cumulative travel. If the WSD cost gradient points along the crease
    // (a step/eave corner with nothing pinning it laterally), the corner walks
    // off the building one nudge at a time — observed on dahouet: RE2+P9C5+RE39
    // slid 621 accepted steps from t=0 to t=-85 (85 m off the roof), leaving a
    // huge spurious spike in the plane-0/9 polygons. The clamp bounds the slide
    // OFFSET so the corner's parameter t along the binding ridge stays within
    // the clipped segment [0, tmax] (the same clamp the snap-in uses). It is a
    // symptom-level backstop and is OFF by default: the real fixes (WSD
    // normalization, a shape/anti-spike term, in-loop corner insertion) are
    // tracked in journal 2026-06-29-corner-slide-runaway-and-wsd-coverage-audit.
    // A proper end-margin is still TODO (ridge extents are inexact).
    const char* _sc_env = getenv("BR_SLIDE_CLAMP");
    const bool br_slide_clamp = (_sc_env != NULL) && (_sc_env[0] != '0');
    // #25 in-loop corner insertion — PARAM `cornerInsert` (part of the cache key,
    // so on/off get distinct artifacts; a version bump can't bust content-addressed
    // artifacts, a param can). Env BR_CORNER_INSERT still overrides for CLI A/B.
    const char* _ci_env = getenv("BR_CORNER_INSERT");
    const bool br_corner_insert = _ci_env ? (_ci_env[0] != '0') : params->corner_insert;
    // Marginal compactness guard weight (env BR_LAMBDA_COMPACT overrides for CLI A/B).
    const char* _lc_env = getenv("BR_LAMBDA_COMPACT");
    const double br_lambda_compact = _lc_env ? atof(_lc_env) : params->lambda_compact;
    // Vertex-support FORCE: penalize a corner whose position sits > support_tol from
    // the nearest owner point of each of its planes (an unsupported vertex in empty
    // space, e.g. id=166). Gradient points toward the points → retracts it. This is
    // the point-aware metric compactness is blind to. Env-only for now (prototype).
    const char* _ls_env = getenv("BR_LAMBDA_SUPPORT");
    const double br_lambda_support = _ls_env ? atof(_ls_env) : 0.0;
    const char* _st_env = getenv("BR_SUPPORT_TOL");
    const double br_support_tol = _st_env ? atof(_st_env) : 1.0;
    // GATED marginal-compactness rule (the scale-free form): VETO a candidate move
    // that worsens a plane's compactness UNLESS it improves WSD by > eps. Never lets
    // compactness override a real coverage gain (decoupled from plane size, unlike
    // the weighted term above). eps = the WSD improvement that counts as "real".
    // eps = the ABSOLUTE per-plane WSD improvement below which a move counts as
    // "nothing on WSD". A candidate that worsens a plane's compactness is vetoed
    // unless it improves THAT plane's WSD by more than eps. Small (drift ΔWSD≈0 is
    // blocked; real coverage gains ≫ eps pass). Gated by env; both accept paths.
    const char*  _cg_env             = getenv("BR_COMPACT_GATE");
    const bool   br_compact_gate     = _cg_env ? (_cg_env[0] != '0') : params->compact_gate;
    const char*  _cge_env            = getenv("BR_COMPACT_GATE_EPS");
    const double br_compact_gate_eps = _cge_env ? atof(_cge_env) : params->compact_gate_eps;
    // WSD normalization (PARAM `wsdNormalize`, default false). The WSD coverage
    // term is the extensive sum Σw·d, so a big plane's cost dwarfs other planes
    // AND all (saturated) terms — a corner shared by a big and small plane is
    // governed by point COUNT, not fit quality (measured ~46× size-coupling on
    // dahouet: plane 0 raw 21952 vs plane 9 raw 13; mean-misfit 7.06 vs 0.19 m).
    // With it on, each plane's WSD is divided by its owner mass Σw (pc[].sum_w, a
    // CONSTANT) → weighted-mean misfit distance: intensive, size-invariant, fair
    // across planes and comparable to the other terms. Normalize by Σw, NOT
    // perimeter/area (those are variables → would reward spikes). It is a PARAM
    // (part of the cache key) not an env var, so on/off get distinct cache
    // entries. See journal 2026-06-29-corner-slide-runaway-and-wsd-coverage-audit.
    const bool br_wsd_norm = params->wsd_normalize;
    // Cost probe (diagnostic): BR_COST_PROBE=<corner_id> prints the per-term
    // breakdown of TOTAL_COST for that corner at the START of every iteration
    // visit, so we can see which weighted term decreases as a corner slides
    // (the "why does cost dumbly improve for 621 moves" investigation). Off
    // (UINT32_MAX) by default; zero output change.
    const char* _cp_env = getenv("BR_COST_PROBE");
    const uint32_t br_cost_probe = _cp_env ? (uint32_t)strtoul(_cp_env, NULL, 10) : UINT32_MAX;
    // BR_PIN_TRACE=<corner_id>: at the last iter, decompose the per-term cost at the
    // corner's CURRENT position vs an INWARD candidate (0.5 m toward the nearest owner
    // of its worst-supported plane). Shows which term(s) rise on the inward move —
    // i.e. what pins an unsupported corner (id=166) in empty space.
    const char* _pt_env = getenv("BR_PIN_TRACE");
    const uint32_t br_pin_trace = _pt_env ? (uint32_t)strtoul(_pt_env, NULL, 10) : UINT32_MAX;
    // BR_NO_R4 (diagnostic): suppress the legacy R.4 crude make-shared so the
    // T4 read-only share-PREVIEW reflects what T5/T7's MERGE/SPLIT would fire
    // standalone. With R.4 ON (default) it pre-empts the obvious on-crease
    // cross-plane pairs (its 14 merges), so the post-loop preview sees only the
    // no-partner residue → ~0. With R.4 OFF the preview fires (condB+condA>0).
    const bool _br_no_r4 = (getenv("BR_NO_R4") != NULL);
    // T7 SPLIT_TO_SHARE_CORNER gate (default ON; BR_NO_SPLIT=1 disables → A/B),
    // mirroring BR_NO_R4 / BR_SNAP_IN. When OFF, the in-loop SPLIT op never
    // fires and the loop reverts to the no-split behaviour (T5/T6 only).
    const bool _br_no_split = (getenv("BR_NO_SPLIT") != NULL);
    // Test-only OOM injection (fixture E): force poly_reserve/ctx_reserve to be
    // treated as failed so the SPLIT REJECTs cleanly with nothing mutated.
    const bool _br_test_reserve_fail = (getenv("BR_TEST_RESERVE_FAIL") != NULL);
    if (params->use_ridge_merge && n_ridges > 0) {
        rg = (ridge_geom_t*)safe_calloc_array(n_ridges, sizeof(ridge_geom_t));
        if (!rg) goto cleanup;
        for (size_t ri = 0; ri < n_ridges; ri++) {
            rg[ri].plane_a = ridges[ri].plane_a;
            rg[ri].plane_b = ridges[ri].plane_b;
            rg[ri].type    = ridges[ri].type;   // I1 allowlist (make-shared)
            double dx = ridges[ri].to[0] - ridges[ri].from[0];
            double dy = ridges[ri].to[1] - ridges[ri].from[1];
            double dz = ridges[ri].to[2] - ridges[ri].from[2];
            double L = sqrt(dx*dx + dy*dy + dz*dz);
            rg[ri].rp[0] = ridges[ri].from[0];
            rg[ri].rp[1] = ridges[ri].from[1];
            rg[ri].rp[2] = ridges[ri].from[2];
            if (L < 1e-6) { rg[ri].tmax = -1.0; continue; }
            rg[ri].rd[0] = dx / L; rg[ri].rd[1] = dy / L; rg[ri].rd[2] = dz / L;
            rg[ri].tmax = L;
        }

        // plane→ridges CSR index (T4 §4.5(3)). Keyed by PLANE INDEX (via
        // plane_map): a ridge with plane ids (a,b) appends ri to the slots of
        // index(a) and index(b). Counting pass → prefix-sum → fill. A ridge
        // whose plane id isn't in plane_map (shouldn't happen) is skipped for
        // that slot. Built unconditionally with rg[]; freed at cleanup.
        ridx_off = (uint32_t*)safe_calloc_array(n_planes + 1, sizeof(uint32_t));
        ridx     = (uint32_t*)safe_calloc_array(n_ridges ? 2 * n_ridges : 1, sizeof(uint32_t));
        if (!ridx_off || !ridx) goto cleanup;
        for (size_t ri = 0; ri < n_ridges; ri++) {
            uint32_t pia = id_lk(&plane_map, rg[ri].plane_a);
            uint32_t pib = id_lk(&plane_map, rg[ri].plane_b);
            if (pia != UINT32_MAX) ridx_off[pia + 1]++;
            if (pib != UINT32_MAX) ridx_off[pib + 1]++;
        }
        for (size_t pi = 0; pi < n_planes; pi++) ridx_off[pi + 1] += ridx_off[pi];
        // temp write-cursors = a copy of the offsets
        uint32_t* _cur = (uint32_t*)safe_calloc_array(n_planes ? n_planes : 1, sizeof(uint32_t));
        if (!_cur) goto cleanup;
        for (size_t pi = 0; pi < n_planes; pi++) _cur[pi] = ridx_off[pi];
        for (size_t ri = 0; ri < n_ridges; ri++) {
            uint32_t pia = id_lk(&plane_map, rg[ri].plane_a);
            uint32_t pib = id_lk(&plane_map, rg[ri].plane_b);
            if (pia != UINT32_MAX) ridx[_cur[pia]++] = (uint32_t)ri;
            if (pib != UINT32_MAX) ridx[_cur[pib]++] = (uint32_t)ri;
        }
        free(_cur);

        // T4 ridge-type histogram (validation: confirm step/coplanar excluded
        // from the make-shared allowlist). 1=ridge 2=hip 3=valley 4=coplanar
        // 5=step; "allowed" = {1,2,3}.
        if (br4_trace) {
            uint32_t _h[8] = {0}, _allowed = 0, _excluded = 0;
            for (size_t ri = 0; ri < n_ridges; ri++) {
                uint8_t t = rg[ri].type;
                _h[t < 8 ? t : 0]++;
                if (RIDGE_TYPE_ALLOWED(t)) _allowed++; else _excluded++;
            }
            fprintf(stderr,
                "[ms-rtype] n_ridges=%zu  ridge=%u hip=%u valley=%u coplanar=%u step=%u "
                "other=%u | allowed(make-shared)=%u excluded=%u\n",
                n_ridges, _h[1], _h[2], _h[3], _h[4], _h[5],
                _h[0] + _h[6] + _h[7], _allowed, _excluded);
        }
    }

    // ── R.0 DOF tagging ──────────────────────────────────────────
    uint64_t br_t_R0_start = 0; if (br_prof_on) BR_NOW(br_t_R0_start);
    corner_state_t* cs = (corner_state_t*)safe_calloc_array(n_cap, sizeof(corner_state_t));
    if (!cs) goto cleanup;
    for (size_t ci = 0; ci < n_corners; ci++) {
        const v7_br_input_corner_t* c = &corners_in[ci];
        cs[ci].point[0] = c->point[0]; cs[ci].point[1] = c->point[1]; cs[ci].point[2] = c->point[2];
        cs[ci].alive    = true;
        // DOF from PATH membership (the SSOT) = # planes whose polygon holds this
        // corner — NOT corners_in[].plane_ids. polys[] isn't built yet here, so scan
        // the INPUT polygon_paths (n>=3), the same paths polys[] is cloned from.
        // DELIBERATELY topological, not the old geometric plane_ids count: plane_ids =
        // "lies on N plane surfaces" (<=0.12m) OVER-counts junctions — a corner coplanar
        // with a FAR plane is not a vertex of it — spuriously over-locking ridge corners
        // that should slide. The A2 coupling pass then MIN-tightens this with ridge_dof
        // (it can only LOWER, never restore — verified), and RETAG_DOF refreshes on
        // merges. Switching this tag geometric->topological moves ~111 corners on dahouet
        // (optimizer-basin amplification of ~4 genuine coupled-DOF changes; A2 re-locks
        // 32/36 raw diffs via ridge tags). See design doc §2.4 DECISION UPDATE 2026-06-19
        // + working-plan L2 (the ~4 residual = correct frees + 1 near-junction tag gap).
        uint32_t r0_pids[PLANES_OF_SCRATCH];
        uint32_t r0_np = 0;
        {
            uint32_t cid = c->id;
            for (size_t pi = 0; pi < n_polygon_paths; pi++) {
                const v7_br_input_polygon_path_t* p = &polygon_paths[pi];
                if (p->n_corners < 3) continue;
                bool in = false;
                for (uint32_t k = 0; k < p->n_corners; k++)
                    if (p->corner_ids[k] == cid) { in = true; break; }
                if (!in) continue;
                bool have = false;
                for (uint32_t j = 0; j < r0_np; j++)
                    if (r0_pids[j] == p->plane_id) { have = true; break; }
                if (have) continue;
                if (r0_np < PLANES_OF_SCRATCH) r0_pids[r0_np] = p->plane_id;
                r0_np++;
            }
        }
        cs[ci].n_planes = r0_np;
        if (r0_np >= 3) {
            cs[ci].dof = 0;
            out->stats.dof0++;
        } else if (r0_np == 2) {
            uint32_t pa = id_lk(&plane_map, r0_pids[0]);
            uint32_t pb = id_lk(&plane_map, r0_pids[1]);
            cs[ci].dof = 1;
            if (pa != UINT32_MAX && pb != UINT32_MAX) {
                const double *nA = planes[pa].normal, *nB = planes[pb].normal;
                double lx = nA[1]*nB[2] - nA[2]*nB[1];
                double ly = nA[2]*nB[0] - nA[0]*nB[2];
                double lz = nA[0]*nB[1] - nA[1]*nB[0];
                double ll = sqrt(lx*lx + ly*ly + lz*lz);
                if (ll > 1e-8) {
                    cs[ci].has_axis_a = true;
                    cs[ci].axis_a[0] = lx/ll; cs[ci].axis_a[1] = ly/ll; cs[ci].axis_a[2] = lz/ll;
                }
            }
            out->stats.dof1++;
        } else if (r0_np == 1) {
            uint32_t pi = id_lk(&plane_map, r0_pids[0]);
            cs[ci].dof = 2;
            if (pi != UINT32_MAX) {
                const double* n = planes[pi].normal;
                double ux, uy, uz;
                if (fabs(n[1]) > 0.9) { ux = 1; uy = 0; uz = 0; }
                else                    { ux = 0; uy = 1; uz = 0; }
                double e1x = uy*n[2] - uz*n[1];
                double e1y = uz*n[0] - ux*n[2];
                double e1z = ux*n[1] - uy*n[0];
                double e1l = sqrt(e1x*e1x + e1y*e1y + e1z*e1z);
                if (e1l < EPS) e1l = 1.0;
                e1x /= e1l; e1y /= e1l; e1z /= e1l;
                double e2x = n[1]*e1z - n[2]*e1y;
                double e2y = n[2]*e1x - n[0]*e1z;
                double e2z = n[0]*e1y - n[1]*e1x;
                cs[ci].has_axis_a = true; cs[ci].has_axis_b = true;
                cs[ci].axis_a[0] = e1x; cs[ci].axis_a[1] = e1y; cs[ci].axis_a[2] = e1z;
                cs[ci].axis_b[0] = e2x; cs[ci].axis_b[1] = e2y; cs[ci].axis_b[2] = e2z;
            }
            out->stats.dof2++;
        } else {
            cs[ci].dof = 2;  // 0-plane orphan, fully free but shouldn't exist
        }
    }
    out->stats.n_corners_total = (uint32_t)n_corners;

    // Emit DOF records.
    out->dof = (v7_br_dof_record_t*)safe_calloc_array(n_cap, sizeof(v7_br_dof_record_t));
    if (!out->dof) { free(cs); goto cleanup; }
    for (size_t ci = 0; ci < n_corners; ci++) {
        out->dof[ci].corner_id  = corners_in[ci].id;
        out->dof[ci].dof        = cs[ci].dof;
        out->dof[ci].has_axis_a = cs[ci].has_axis_a;
        out->dof[ci].has_axis_b = cs[ci].has_axis_b;
        if (cs[ci].has_axis_a) memcpy(out->dof[ci].axis_a, cs[ci].axis_a, 24);
        if (cs[ci].has_axis_b) memcpy(out->dof[ci].axis_b, cs[ci].axis_b, 24);
    }
    out->n_dof = n_corners;
    BR_ACCUM(br_p_R0, br_t_R0_start);

    // ── R.0b — Polygon cleanup (unused corners + redundant ridge midpoints) ──
    // Mark which corners are referenced by any polygon path. Unused ones get
    // skipped during R.1 (we don't repack the corner array, but we drop their
    // emit records and skip optimisation for them — matches v2 semantics).
    uint64_t br_t_R0b_start = 0; if (br_prof_on) BR_NOW(br_t_R0b_start);
    bool* used = (bool*)safe_calloc_array(n_cap, sizeof(bool));
    if (!used) { free(cs); goto cleanup; }
    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        const v7_br_input_polygon_path_t* p = &polygon_paths[pi];
        for (uint32_t k = 0; k < p->n_corners; k++) {
            uint32_t ci = id_lk(&corner_map, p->corner_ids[k]);
            if (ci != UINT32_MAX) used[ci] = true;
        }
    }
    for (size_t ci = 0; ci < n_corners; ci++) if (!used[ci]) out->stats.unused_removed++;

    // Redundant ridge midpoint: 3 consecutive corners in a polygon path share a ridge_id.
    // We clone polygon paths into a mutable form (poly_t, file scope) and prune.
    poly_t* polys = (poly_t*)safe_calloc_array(n_polygon_paths ? n_polygon_paths : 1, sizeof(poly_t));
    if (!polys) { free(used); free(cs); goto cleanup; }
    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        const v7_br_input_polygon_path_t* p = &polygon_paths[pi];
        polys[pi].plane_id = p->plane_id;
        polys[pi].n = p->n_corners;
        polys[pi].cap = p->n_corners;
        polys[pi].ids = (uint32_t*)safe_malloc_array(p->n_corners ? p->n_corners : 1, sizeof(uint32_t));
        if (!polys[pi].ids) {
            for (size_t k = 0; k < pi; k++) free(polys[k].ids);
            free(polys); free(used); free(cs);
            goto cleanup;
        }
        if (p->n_corners > 0 && p->corner_ids) {
            memcpy(polys[pi].ids, p->corner_ids, p->n_corners * sizeof(uint32_t));
        }
    }
    // NOTE (T6, 2026-06-19): the pre-loop redundant-on-ridge cleanup is no
    // longer this per-path pass. It is now a corner-centric all-polys-agree
    // batch (try_drop_redundant_on_ridge) RE-SITED below — after cs[] and
    // cs[].ridge_ids are seeded — so the pre-loop and in-loop ridge-cleanups
    // share ONE tag source (cs[].ridge_ids) and ONE path store (polys[]). The
    // old per-path pass dropped a shared corner from one plane asymmetrically
    // (it could destroy a shared edge another plane still needs); the
    // all-polys-agree version keeps such corners. Search "T6 pre-loop".
    //
    // Mark R.0b-unused corners as dead — matches v2's `roofGraph.corners =
    // corners.filter(c => usedCornerIds.has(c.id))` filter. Without this,
    // dof.json keeps records for corners that have no polygon membership
    // (which then never get optimized but stay in the alive set). The
    // still-used recompute (catches corners orphaned by a splice/collapse) is
    // deferred to AFTER the re-sited T6 batch (search "T6 still-used").
    for (size_t ci = 0; ci < n_corners; ci++) {
        if (!used[ci]) cs[ci].alive = false;
    }
    free(used);

    // NOTE (T0, 2026-06-17): polygon_paths emission MOVED to post-loop.
    // It used to be emitted HERE (pre-loop), capturing only R.0b's edits and
    // missing every in-loop R.2/R.3/R.4 path mutation — so the emitted graph
    // carried dead-id refs (corners merged away in-loop). The emission now
    // lives next to the post-loop corner_planes/corner_positions re-derivation
    // (search "T0: emit polygon_paths from final polys[]"), emitting ALL final
    // paths (n>=3 of live ids). See §8.2 step 1 + §9 output-emission gap.

    // ── Build per-plane UV cache (owner points + nearby foreign points) ─
    typedef struct {
        bool       have;
        plane_uv_basis_t basis;
        double*    own_u; double* own_v;
        double*    own_w;            // trust weights (or NULL)
        size_t     n_owner;
        double     sum_w;            // Σ own_w (or n_owner if own_w==NULL) — the
                                     // per-plane owner mass; constant normalizer
                                     // for the WSD coverage term (BR_WSD_NORM).
        double*    near_u; double* near_v;
        double*    near_w;
        size_t     n_nearby;
        bool       is_wall;
        uint8_t    plane_type;       // rg_plane_type_t — for the cross-type merge gate
    } plane_cache_t;

    plane_cache_t* pc = (plane_cache_t*)safe_calloc_array(n_planes, sizeof(plane_cache_t));
    if (!pc) { for (size_t pi = 0; pi < n_polygon_paths; pi++) free(polys[pi].ids); free(polys); free(cs); goto cleanup; }

    for (size_t pi = 0; pi < n_planes; pi++) {
        const v7_br_input_plane_t* p = &planes[pi];
        if (p->rejected) continue;
        plane_basis(&pc[pi].basis, p->normal, p->d);
        // Read the authoritative plane_type from topology (the sole classifier) —
        // no local tilt recompute. Behaviour-identical to the old `tilt >= 72` check
        // today, but now there is ONE source of truth for "is this a wall".
        pc[pi].is_wall = (p->plane_type == RG_PLANE_TYPE_WALL);
        pc[pi].plane_type = p->plane_type;
        // Owner points: scan all positions; collect those whose owner == p->id.
        size_t n_own = 0;
        for (size_t k = 0; k < n_points; k++) if ((int32_t)owner[k] == (int32_t)p->id) n_own++;
        if (n_own > 0) {
            pc[pi].own_u = (double*)safe_malloc_array(n_own, sizeof(double));
            pc[pi].own_v = (double*)safe_malloc_array(n_own, sizeof(double));
            if (!pc[pi].own_u || !pc[pi].own_v) goto inner_oom;
            if (params->use_trust_weighting && point_plane_residual && point_plane_dist_threshold > 0) {
                pc[pi].own_w = (double*)safe_malloc_array(n_own, sizeof(double));
                if (!pc[pi].own_w) goto inner_oom;
            }
            size_t k_out = 0;
            for (size_t k = 0; k < n_points; k++) {
                if ((int32_t)owner[k] != (int32_t)p->id) continue;
                double u, v;
                project_uv(&pc[pi].basis,
                            (double)positions[3*k+0],
                            (double)positions[3*k+1],
                            (double)positions[3*k+2],
                            &u, &v);
                pc[pi].own_u[k_out] = u;
                pc[pi].own_v[k_out] = v;
                if (pc[pi].own_w) {
                    double r = (double)point_plane_residual[k];
                    if (!isfinite(r)) r = 0.0;
                    double w = 1.0 - r / point_plane_dist_threshold;
                    if (w < 0) w = 0; else if (w > 1) w = 1;
                    pc[pi].own_w[k_out] = w;
                }
                k_out++;
            }
            pc[pi].n_owner = n_own;
            // Owner mass Σw (constant): the intensive normalizer for WSD. With
            // trust weighting off (own_w==NULL) every point weighs 1 → Σw=n_owner.
            double _sw = 0;
            if (pc[pi].own_w) { for (size_t _k = 0; _k < n_own; _k++) _sw += pc[pi].own_w[_k]; }
            else _sw = (double)n_own;
            pc[pi].sum_w = _sw;
        }
        // Nearby foreign points for THIS plane id. Filter out items basically on this plane.
        for (size_t ni = 0; ni < n_nearby; ni++) {
            if (nearby[ni].plane_id != p->id) continue;
            size_t kept = 0;
            for (uint32_t k = 0; k < nearby[ni].n_items; k++) {
                if (fabs((double)nearby[ni].items[k].signed_dist) >= NEARBY_OUTSIDE_MIN_M) kept++;
            }
            if (kept == 0) break;
            pc[pi].near_u = (double*)safe_malloc_array(kept, sizeof(double));
            pc[pi].near_v = (double*)safe_malloc_array(kept, sizeof(double));
            if (!pc[pi].near_u || !pc[pi].near_v) goto inner_oom;
            if (params->use_trust_weighting && point_plane_residual && point_plane_dist_threshold > 0) {
                pc[pi].near_w = (double*)safe_malloc_array(kept, sizeof(double));
                if (!pc[pi].near_w) goto inner_oom;
            }
            size_t k_out = 0;
            for (uint32_t k = 0; k < nearby[ni].n_items; k++) {
                if (fabs((double)nearby[ni].items[k].signed_dist) < NEARBY_OUTSIDE_MIN_M) continue;
                double u, v;
                project_uv(&pc[pi].basis,
                            nearby[ni].items[k].position[0],
                            nearby[ni].items[k].position[1],
                            nearby[ni].items[k].position[2],
                            &u, &v);
                pc[pi].near_u[k_out] = u;
                pc[pi].near_v[k_out] = v;
                if (pc[pi].near_w) {
                    uint32_t gi = nearby[ni].items[k].src_idx;
                    double w = 1.0;
                    if (gi < n_points) {
                        double r = (double)point_plane_residual[gi];
                        if (isfinite(r)) {
                            w = 1.0 - r / point_plane_dist_threshold;
                            if (w < 0) w = 0; else if (w > 1) w = 1;
                        }
                    }
                    pc[pi].near_w[k_out] = w;
                }
                k_out++;
            }
            pc[pi].n_nearby = kept;
            break;
        }
        pc[pi].have = true;
        continue;
inner_oom:
        for (size_t pj = 0; pj < n_polygon_paths; pj++) free(polys[pj].ids);
        free(polys); free(cs);
        for (size_t pj = 0; pj <= pi; pj++) {
            free(pc[pj].own_u); free(pc[pj].own_v); free(pc[pj].own_w);
            free(pc[pj].near_u); free(pc[pj].near_v); free(pc[pj].near_w);
        }
        free(pc);
        goto cleanup;
    }

    // ── Build corner contexts: for each (corner, polygon-it-appears-in)
    //    record (plane_id, prev_corner_idx, next_corner_idx, frozen_target).
    //    Use the post-R.0b polygon paths.
    // total_ctx = Σ path-lengths = the number of (corner,polygon) memberships,
    // which is exactly how many records REBUILD_CTXS writes. cap_ctx is the
    // ALLOCATED capacity of ctxs[]; until T7 the two were implicitly equal and
    // REBUILD_CTXS wrote ctxs[n_ctx++] with NO bound. A SPLIT grows a path by 1
    // (+1 membership), so total_ctx grows and the pool must grow first — see
    // ctx_reserve below (§4.3a, MG1). cap_ctx is bumped only by ctx_reserve;
    // total_ctx is bumped on a committed SPLIT so the pool stays correctly sized.
    size_t total_ctx = 0;
    for (size_t pi = 0; pi < n_polygon_paths; pi++) total_ctx += polys[pi].n;
    size_t cap_ctx = total_ctx ? total_ctx : 1;
    corner_context_t* ctxs = (corner_context_t*)safe_calloc_array(cap_ctx, sizeof(corner_context_t));
    if (!ctxs) {
        for (size_t pj = 0; pj < n_polygon_paths; pj++) free(polys[pj].ids);
        for (size_t pj = 0; pj < n_planes; pj++) {
            free(pc[pj].own_u); free(pc[pj].own_v); free(pc[pj].own_w);
            free(pc[pj].near_u); free(pc[pj].near_v); free(pc[pj].near_w);
        }
        free(pc); free(polys); free(cs);
        goto cleanup;
    }
    size_t n_ctx = 0;
    // Per-corner: list of context indices.
    uint32_t** corner_ctx_indices = (uint32_t**)safe_calloc_array(n_cap, sizeof(uint32_t*));
    uint32_t*  corner_ctx_n        = (uint32_t*) safe_calloc_array(n_cap, sizeof(uint32_t));
    uint32_t*  corner_ctx_cap      = (uint32_t*) safe_calloc_array(n_cap, sizeof(uint32_t));
    if (!corner_ctx_indices || !corner_ctx_n || !corner_ctx_cap) {
        free(corner_ctx_indices); free(corner_ctx_n); free(corner_ctx_cap); free(ctxs);
        for (size_t pj = 0; pj < n_polygon_paths; pj++) free(polys[pj].ids);
        for (size_t pj = 0; pj < n_planes; pj++) {
            free(pc[pj].own_u); free(pc[pj].own_v); free(pc[pj].own_w);
            free(pc[pj].near_u); free(pc[pj].near_v); free(pc[pj].near_w);
        }
        free(pc); free(polys); free(cs);
        goto cleanup;
    }
    // Per-corner plane membership is now DERIVED on demand from the canonical
    // polygon paths (polys[]) via planes_of() — there is no stored/maintained
    // planes_dyn. The old seed-then-reconcile was a path-membership snapshot;
    // planes_of computes exactly that set live, so it can never stale-claim
    // (the corner-197 class of over-claims is fixed by construction). See
    // design doc §2.4 DECISION UPDATE 2026-06-19.

    // ── A2 (§4.6): seed cs[].ridge_ids from corners_in[].ridge_ids, REALITY-
    //    INTERSECTED. A copy, then keep a tag R
    //    iff R is a live ridge index, rg[R].tmax>0, AND the corner actually
    //    projects onto R's CLIPPED segment within τ_in perp (t∈[0,tmax]).
    //    τ_in = ridge_merge_coplanar_m = 0.30 ("lies on the crease line").
    //    The store is built unconditionally (the DOF *effect* is gated, not
    //    the store) so the A/B can flip BR_DOF_COUPLE without rebuilding.
    {
        const double tau_in = params->ridge_merge_coplanar_m;
        for (size_t ci = 0; ci < n_corners; ci++) {
            uint32_t nin = corners_in[ci].n_ridge_ids;
            if (nin == 0 || rg == NULL) continue;
            const uint32_t* src = corners_in[ci].ridge_ids;
            for (uint32_t j = 0; j < nin; j++) {
                uint32_t R = src[j];
                if (R >= n_ridges) continue;
                if (rg[R].tmax <= 0.0) continue;
                double t, perp;
                ridge_proj(cs[ci].point, rg[R].rp, rg[R].rd, &t, &perp);
                if (perp > tau_in) continue;
                if (t < 0.0 || t > rg[R].tmax) continue;
                if (!ridge_ids_push_unique(&cs[ci].ridge_ids, &cs[ci].n_ridge_ids, R)) {
                    rc = -1; goto r1_ctx_oom;
                }
            }
        }
    }

    // ── T6 pre-loop: redundant-on-ridge cleanup, all-polys-agree (§4.2) ─────
    // RE-SITED here (was a per-path pass right after the polys[] clone) so it
    // reads the SAME maintained tag source (cs[].ridge_ids, just seeded above)
    // and SAME path store (polys[]) as the in-loop T6 call — one implementation,
    // try_drop_redundant_on_ridge. This runs BEFORE any corner-moving mutation
    // (the R.1 loop / chase), so it remains a genuine PRE-pass; cs[].ridge_ids
    // here is the reality-intersected input seed (no in-loop union yet), so the
    // only behaviour delta vs the old per-path pass is the all-polys-agree rule
    // (it KEEPS a corner that is straight in one plane but a shared-edge endpoint
    // in another, instead of dropping it asymmetrically). Corner-centric +
    // re-queueing while(changed): dropping ci can make its old neighbours newly
    // redundant, so we sweep to a fixpoint.
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t ci = 0; ci < n_corners; ci++) {
                if (!cs[ci].alive) continue;
                if (try_drop_redundant_on_ridge(ci, corners_in, polys, n_polygon_paths,
                                                &corner_map, cs)) {
                    out->stats.redundant_ridge_removed++;
                    out->stats.n_drop_ridge++;   // T6: DROP_REDUNDANT_ON_RIDGE (pre-loop)
                    changed = true;
                }
            }
        }
    }
    // L1: catch corners orphaned by the pre-loop ridge-cleanup batch (a splice that
    // collapsed a ring to n<3 leaves bystanders in no path). Swept again post-loop.
    kill_orphaned_corners(cs, n_corners, corners_in, polys, n_polygon_paths);

    // ── A2 (§4.6) DOF coupling — MIN-DOF rule, gated by BR_DOF_COUPLE ──────
    //   dof = min(plane_dof, ridge_dof), ridge_dof over the reality-intersected
    //   ridge set = {>=2 surviving → 0, ==1 → 1, ==0 → 2}. min (not ridge-
    //   overrides-plane) so a real junction (plane_dof=0) whose extra ridge tags
    //   fall outside their clipped segments — keeping 1 surviving tag — stays
    //   PINNED (min(0,1)=0), never wrongly un-pinned. When the result is DOF-1
    //   and the ridge is the binding constraint, the slide axis is the ridge
    //   direction rg.rd (A3 — NOT nA×nB) and has_axis_a MUST be true (a DOF-1
    //   corner with no axis generates zero SLIDE candidates and FREEZES).
    //   Applied here once (post-seed; the R.0 plane-count tag above is the
    //   provisional) and appended to RETAG_DOF so every in-loop merge re-couples.
    //   Re-emits out->dof + recomputes the dof0/1/2 stats from the coupled set.
    #define COUPLE_RIDGE_DOF(_ci)                                                                  \
        do {                                                                                       \
            if (br_dof_couple && cs[_ci].n_ridge_ids > 0) {                                         \
                size_t _cd_bind;                                                                    \
                int _cd_rd = ridge_dof_for_corner(rg, n_ridges, cs[_ci].ridge_ids,                  \
                                                  cs[_ci].n_ridge_ids, cs[_ci].point,               \
                                                  params->ridge_merge_coplanar_m, &_cd_bind);       \
                int _cd_pd = (int)cs[_ci].dof;                                                      \
                int _cd_final = (_cd_rd < _cd_pd) ? _cd_rd : _cd_pd;                                \
                if (_cd_final != _cd_pd) {                                                          \
                    cs[_ci].dof = (uint8_t)_cd_final;                                               \
                    if (_cd_final == 0) { cs[_ci].has_axis_a = false; cs[_ci].has_axis_b = false; } \
                }                                                                                   \
                /* DOF-1 with the ridge binding → slide along rg.rd, axis present */                \
                if (_cd_final == 1 && _cd_rd == 1 && _cd_bind != (size_t)-1                          \
                        && rg[_cd_bind].tmax > 0.0) {                                                \
                    cs[_ci].has_axis_a = true; cs[_ci].has_axis_b = false;                          \
                    cs[_ci].axis_a[0] = rg[_cd_bind].rd[0];                                          \
                    cs[_ci].axis_a[1] = rg[_cd_bind].rd[1];                                          \
                    cs[_ci].axis_a[2] = rg[_cd_bind].rd[2];                                          \
                }                                                                                   \
            }                                                                                       \
        } while (0)

    // Initial coupling pass + out->dof re-emit + stats recompute (the R.0
    // emission above was plane-count-only; correct it now that tags are seeded).
    if (br_dof_couple) {
        // A2 pre-measure counters (printed under BR_PROFILE/BR4_TRACE): the
        // scout's Python measure expected ~17 single-plane-on-ridge corners on
        // dahouet flipping DOF-2→1, plus the 2→1 / 1→0 flip tallies.
        uint32_t a2_seed_on_ridge_singleplane = 0;  // tagged + single-plane (pre-couple DOF-2)
        uint32_t a2_flip_2_to_1 = 0, a2_flip_1_to_0 = 0, a2_flip_2_to_0 = 0, a2_demote_check = 0;
        out->stats.dof0 = 0; out->stats.dof1 = 0; out->stats.dof2 = 0;
        for (size_t ci = 0; ci < n_corners; ci++) {
            uint8_t _pre = cs[ci].dof;  // plane-count DOF before coupling
            uint32_t _a2_np = planes_of_n(ci, corners_in, polys, n_polygon_paths);
            if (cs[ci].n_ridge_ids > 0 && _a2_np == 1)
                a2_seed_on_ridge_singleplane++;
            COUPLE_RIDGE_DOF(ci);
            uint8_t _post = cs[ci].dof;
            if (_post < _pre) {
                if (_pre == 2 && _post == 1) a2_flip_2_to_1++;
                else if (_pre == 1 && _post == 0) a2_flip_1_to_0++;
                else if (_pre == 2 && _post == 0) a2_flip_2_to_0++;
            } else if (_post > _pre) {
                a2_demote_check++;   // must stay 0 — coupling only tightens (min)
            }
            // Count only real corners (>=1 plane membership, path-derived),
            // matching the R.0 stat domain (0-plane orphans aren't counted there).
            if (_a2_np >= 1) {
                if      (cs[ci].dof == 0) out->stats.dof0++;
                else if (cs[ci].dof == 1) out->stats.dof1++;
                else                      out->stats.dof2++;
            }
            out->dof[ci].dof        = cs[ci].dof;
            out->dof[ci].has_axis_a = cs[ci].has_axis_a;
            out->dof[ci].has_axis_b = cs[ci].has_axis_b;
            if (cs[ci].has_axis_a) memcpy(out->dof[ci].axis_a, cs[ci].axis_a, 24);
            if (cs[ci].has_axis_b) memcpy(out->dof[ci].axis_b, cs[ci].axis_b, 24);
        }
        if (br4_trace) {
            fprintf(stderr,
                "[a2-dof] seed_on_ridge_singleplane=%u  flip 2->1=%u  1->0=%u  2->0=%u  "
                "(wrong-loosen=%u, must be 0)\n",
                a2_seed_on_ridge_singleplane, a2_flip_2_to_1, a2_flip_1_to_0,
                a2_flip_2_to_0, a2_demote_check);
        }
    }

    // ── CTX_RESERVE (T7, §4.3a): ensure ctxs[] has capacity for `_need`
    //    records, growing cap_ctx via realloc. Used by ctx_reserve() in the
    //    SPLIT PREPARE phase (so the COMMIT-phase REBUILD_CTXS cannot overflow)
    //    AND defensively inside REBUILD_CTXS itself (a belt-and-braces guard:
    //    the pool is pre-reserved before any SPLIT, but if a future caller ever
    //    grows a path without pre-reserving, this still keeps the write in
    //    bounds instead of corrupting the heap). On OOM sets rc=-1 + jumps.
    #define CTX_RESERVE(_need)                                                                       \
        do {                                                                                       \
            if ((size_t)(_need) > cap_ctx) {                                                        \
                size_t _ncap = cap_ctx ? cap_ctx * 2 : 4;                                           \
                if (_ncap < (size_t)(_need)) _ncap = (size_t)(_need);                               \
                corner_context_t* _nb = (corner_context_t*)safe_realloc_array(                      \
                        ctxs, _ncap, sizeof(corner_context_t));                                     \
                if (!_nb) { rc = -1; goto r1_ctx_oom; }                                             \
                ctxs = _nb; cap_ctx = _ncap;                                                        \
            }                                                                                       \
        } while (0)

    // ── REBUILD_CTXS: clear n_ctx + corner_ctx_n[], re-walk polys[].
    //    Skips contexts where any of (curr, prev, next) is !alive.
    //    Called once at startup AND after each R.2/R.3 topology change.
    #define REBUILD_CTXS()                                                                          \
        do {                                                                                       \
            n_ctx = 0;                                                                              \
            for (size_t _ci = 0; _ci < n_corners; _ci++) corner_ctx_n[_ci] = 0;                     \
            for (size_t _pi = 0; _pi < n_polygon_paths; _pi++) {                                   \
                poly_t* _p = &polys[_pi];                                                          \
                if (_p->n < 3) continue;                                                            \
                for (uint32_t _i = 0; _i < _p->n; _i++) {                                          \
                    uint32_t _ci    = id_lk(&corner_map, _p->ids[_i]);                              \
                    uint32_t _prev  = id_lk(&corner_map, _p->ids[(_i + _p->n - 1) % _p->n]);        \
                    uint32_t _next  = id_lk(&corner_map, _p->ids[(_i + 1)        % _p->n]);        \
                    if (_ci == UINT32_MAX || _prev == UINT32_MAX || _next == UINT32_MAX) continue; \
                    if (!cs[_ci].alive || !cs[_prev].alive || !cs[_next].alive) continue;          \
                    CTX_RESERVE(n_ctx + 1);  /* defensive: keep the write in bounds */              \
                    ctxs[n_ctx].plane_id   = _p->plane_id;                                          \
                    ctxs[n_ctx].corner_idx = _ci;                                                   \
                    ctxs[n_ctx].prev_idx   = _prev;                                                 \
                    ctxs[n_ctx].next_idx   = _next;                                                 \
                    ctxs[n_ctx].has_target = false;                                                 \
                    double _a = corner_angle_rad(cs[_ci].point, cs[_prev].point, cs[_next].point); \
                    double _dev;                                                                    \
                    double _tgt = nearest_canonical_target_deg(_a * 180.0 / M_PI, targets, n_targets, &_dev); \
                    if (_dev * M_PI / 180.0 < ANGLE_SNAP_THRESH_RAD) {                              \
                        ctxs[n_ctx].has_target = true;                                              \
                        ctxs[n_ctx].frozen_target_rad = _tgt * M_PI / 180.0;                        \
                    }                                                                               \
                    if (corner_ctx_n[_ci] == corner_ctx_cap[_ci]) {                                 \
                        uint32_t _nc = corner_ctx_cap[_ci] == 0 ? 4 : corner_ctx_cap[_ci] * 2;      \
                        uint32_t* _nb = (uint32_t*)safe_realloc_array(corner_ctx_indices[_ci], _nc, sizeof(uint32_t)); \
                        if (!_nb) { rc = -1; goto r1_ctx_oom; }                                    \
                        corner_ctx_indices[_ci] = _nb;                                              \
                        corner_ctx_cap[_ci]     = _nc;                                              \
                    }                                                                               \
                    corner_ctx_indices[_ci][corner_ctx_n[_ci]++] = (uint32_t)n_ctx;                 \
                    n_ctx++;                                                                        \
                }                                                                                   \
            }                                                                                       \
        } while (0)

    REBUILD_CTXS();

    // ── Cost helpers (closures-as-functions via params struct) ────
    //
    // WSD for one plane uses the post-R.0b polygon path of THAT plane,
    // projecting it to UV per evaluation. The polygon mutates only at
    // corner positions we're optimising; corner ids are fixed.

    // Re-project polygon for plane pi → out arrays polyU/polyV (caller-owned).
    #define MAX_POLY_VERTS 256
    double polyU[MAX_POLY_VERTS], polyV[MAX_POLY_VERTS];

    // Compute per-plane WSD given current corner positions.
    double (*wsd_for_plane)(size_t, const corner_state_t*) = NULL;
    // We use a static-ish function via macro since C lacks closures cleanly.
    // For clarity, inline the WSD eval here as a helper above the loop.
    (void)wsd_for_plane;
    BR_ACCUM(br_p_R0b, br_t_R0b_start);

    // ── R.1 — greedy coord descent, single-corner moves ──────────
    // Iterate until no corner moves. Per iteration: for each DOF>0
    // corner, sample candidates along its DOF axes, compute total cost,
    // accept the best if better than current.
    uint64_t br_t_R1_start = 0; if (br_prof_on) BR_NOW(br_t_R1_start);

    // Optimizable set matches v2: dof>0 AND has at least one polygon
    // context. A corner with no polygon membership can't be cost-evaluated.
    // v2 reports this AFTER R.1 completes (re-filtered each iter as
    // R.2/R.3 remove corners) — we recompute at the end for parity.

    // Scratch trajectory buffers (one per corner that moves).
    typedef struct {
        double* steps;     // flattened 3D positions
        uint32_t n_steps;
        uint32_t cap;
    } traj_t;
    traj_t* traj = (traj_t*)safe_calloc_array(n_cap, sizeof(traj_t));
    if (!traj) goto r1_oom;

    // Inline WSD evaluator.
    #define WSD_EVAL(plane_idx, out_dist)                                                       \
        do {                                                                                    \
            const poly_t* pl = NULL;                                                            \
            for (size_t ppi = 0; ppi < n_polygon_paths; ppi++) {                                \
                if (polys[ppi].plane_id == planes[plane_idx].id) { pl = &polys[ppi]; break; }   \
            }                                                                                   \
            (out_dist) = 0.0;                                                                   \
            if (pl && pl->n >= 3 && pl->n <= MAX_POLY_VERTS && pc[plane_idx].have) {            \
                for (uint32_t k = 0; k < pl->n; k++) {                                          \
                    uint32_t ci_k = id_lk(&corner_map, pl->ids[k]);                             \
                    if (ci_k == UINT32_MAX) { (out_dist) = 0; break; }                          \
                    project_uv(&pc[plane_idx].basis,                                            \
                                cs[ci_k].point[0], cs[ci_k].point[1], cs[ci_k].point[2],         \
                                &polyU[k], &polyV[k]);                                          \
                }                                                                               \
                /* owner-out */                                                                  \
                for (size_t kk = 0; kk < pc[plane_idx].n_owner; kk++) {                          \
                    if (point_in_poly_uv(pc[plane_idx].own_u[kk], pc[plane_idx].own_v[kk],      \
                                          polyU, polyV, pl->n)) continue;                       \
                    double w = pc[plane_idx].own_w ? pc[plane_idx].own_w[kk] : 1.0;             \
                    if (w <= 0) continue;                                                       \
                    (out_dist) += w * pt_edge_dist_uv(pc[plane_idx].own_u[kk],                  \
                                                       pc[plane_idx].own_v[kk],                 \
                                                       polyU, polyV, pl->n);                    \
                }                                                                               \
                /* foreign-in (skip walls) */                                                    \
                if (!pc[plane_idx].is_wall) {                                                    \
                    for (size_t kk = 0; kk < pc[plane_idx].n_nearby; kk++) {                     \
                        if (!point_in_poly_uv(pc[plane_idx].near_u[kk], pc[plane_idx].near_v[kk], \
                                                polyU, polyV, pl->n)) continue;                  \
                        double w = pc[plane_idx].near_w ? pc[plane_idx].near_w[kk] : 1.0;        \
                        if (w <= 0) continue;                                                   \
                        (out_dist) += w * pt_edge_dist_uv(pc[plane_idx].near_u[kk],             \
                                                          pc[plane_idx].near_v[kk],             \
                                                          polyU, polyV, pl->n);                 \
                    }                                                                            \
                }                                                                                \
            }                                                                                   \
        } while (0)

    // Compactness of plane_idx's polygon at the current cs[] positions:
    // perim²/(4π·area), scale-invariant (1.0 = circle, grows unbounded for
    // slivers). Same polygon projection as WSD_EVAL. Degenerate (area≈0) → a
    // large finite value (avoids INF in cost arithmetic; never bites legit shapes).
    #define COMPACT_EVAL(plane_idx, out_compact)                                                 \
        do {                                                                                    \
            const poly_t* pl = NULL;                                                            \
            for (size_t ppi = 0; ppi < n_polygon_paths; ppi++) {                                \
                if (polys[ppi].plane_id == planes[plane_idx].id) { pl = &polys[ppi]; break; }   \
            }                                                                                   \
            (out_compact) = 0.0;                                                               \
            if (pl && pl->n >= 3 && pl->n <= MAX_POLY_VERTS) {                                   \
                bool _cok = true;                                                              \
                for (uint32_t k = 0; k < pl->n; k++) {                                          \
                    uint32_t ci_k = id_lk(&corner_map, pl->ids[k]);                             \
                    if (ci_k == UINT32_MAX) { _cok = false; break; }                            \
                    project_uv(&pc[plane_idx].basis,                                            \
                                cs[ci_k].point[0], cs[ci_k].point[1], cs[ci_k].point[2],         \
                                &polyU[k], &polyV[k]);                                          \
                }                                                                               \
                if (_cok) {                                                                    \
                    double _a2 = 0, _pm = 0;                                                    \
                    for (uint32_t k = 0; k < pl->n; k++) { uint32_t k1 = (k+1)%pl->n;           \
                        _a2 += polyU[k]*polyV[k1] - polyU[k1]*polyV[k];                          \
                        double _du=polyU[k1]-polyU[k],_dv=polyV[k1]-polyV[k];                    \
                        _pm += sqrt(_du*_du+_dv*_dv); }                                          \
                    double _ar = fabs(_a2)*0.5;                                                 \
                    (out_compact) = _ar > 1e-6 ? (_pm*_pm)/(4.0*M_PI*_ar) : 1.0e4;               \
                }                                                                               \
            }                                                                                   \
        } while (0)

    // ── Marginal-compactness GATE helpers (shared by the single-corner and joint
    // two-corner accept paths). The gate reference is a set of (plane, WSD, compact)
    // triples captured at the corner(s)' CURRENT position; a candidate is vetoed if
    // ANY referenced plane has its compactness worsened while ITS OWN WSD does not
    // improve by more than a RELATIVE eps (a fraction of that plane's current WSD,
    // floored) — so the threshold scales with each plane's coverage stakes instead
    // of a single absolute number (which coupled to plane size). Both call sites
    // declare: uint32_t _g_pl[32]; double _g_w[32], _g_c[32]; uint32_t _g_n=0;
    //
    // GATE_ADD_PLANES(ci_x): append ci_x's unique planes + their current WSD/compact
    // to the reference. Must be called with cs[] at the REFERENCE (pre-move) state.
    #define GATE_ADD_PLANES(ci_x)                                                                \
        for (uint32_t _cci = 0; _cci < corner_ctx_n[ci_x]; _cci++) {                             \
            uint32_t _pl = ctxs[corner_ctx_indices[ci_x][_cci]].plane_id;                        \
            uint32_t _pi = id_lk(&plane_map, _pl); if (_pi == UINT32_MAX) continue;              \
            bool _s = false; for (uint32_t _g = 0; _g < _g_n; _g++) if (_g_pl[_g] == _pi) { _s = true; break; } \
            if (_s || _g_n >= 32) continue;                                                      \
            _g_pl[_g_n] = _pi; WSD_EVAL(_pi, _g_w[_g_n]); COMPACT_EVAL(_pi, _g_c[_g_n]); _g_n++;  \
        }
    // GATE_VETO_NOW(out): with cs[] at the CANDIDATE state, set out=true iff any
    // referenced plane worsens compactness without a > relative-eps WSD improvement.
    #define GATE_VETO_NOW(out)                                                                   \
        do {                                                                                    \
            (out) = false;                                                                      \
            for (uint32_t _g = 0; _g < _g_n; _g++) {                                             \
                double _w; WSD_EVAL(_g_pl[_g], _w);                                              \
                double _c; COMPACT_EVAL(_g_pl[_g], _c);                                          \
                /* ABSOLUTE eps: "for nothing on WSD" = ΔWSD below a small fixed floor. */       \
                /* (relative-to-current-WSD death-spirals: it shrinks with WSD and blocks */     \
                /* the incremental covering moves it should allow.) */                           \
                if (_c > _g_c[_g] + 1e-4 && _w >= _g_w[_g] - br_compact_gate_eps) { (out) = true; break; } \
            }                                                                                   \
        } while (0)

    // WSD split (diagnostic): same as WSD_EVAL but separates the owner-out
    // (own points left OUTSIDE the polygon) and foreign-in (foreign points
    // wrongly INSIDE) halves, so the cost probe can attribute the slide.
    #define WSD_EVAL_SPLIT(plane_idx, out_owner, out_foreign)                                    \
        do {                                                                                    \
            const poly_t* pl = NULL;                                                            \
            for (size_t ppi = 0; ppi < n_polygon_paths; ppi++) {                                \
                if (polys[ppi].plane_id == planes[plane_idx].id) { pl = &polys[ppi]; break; }   \
            }                                                                                   \
            (out_owner) = 0.0; (out_foreign) = 0.0;                                             \
            if (pl && pl->n >= 3 && pl->n <= MAX_POLY_VERTS && pc[plane_idx].have) {            \
                for (uint32_t k = 0; k < pl->n; k++) {                                          \
                    uint32_t ci_k = id_lk(&corner_map, pl->ids[k]);                             \
                    if (ci_k == UINT32_MAX) { (out_owner) = 0; (out_foreign) = 0; break; }      \
                    project_uv(&pc[plane_idx].basis,                                            \
                                cs[ci_k].point[0], cs[ci_k].point[1], cs[ci_k].point[2],         \
                                &polyU[k], &polyV[k]);                                          \
                }                                                                               \
                for (size_t kk = 0; kk < pc[plane_idx].n_owner; kk++) {                          \
                    if (point_in_poly_uv(pc[plane_idx].own_u[kk], pc[plane_idx].own_v[kk],      \
                                          polyU, polyV, pl->n)) continue;                       \
                    double w = pc[plane_idx].own_w ? pc[plane_idx].own_w[kk] : 1.0;             \
                    if (w <= 0) continue;                                                       \
                    (out_owner) += w * pt_edge_dist_uv(pc[plane_idx].own_u[kk],                 \
                                                        pc[plane_idx].own_v[kk],                \
                                                        polyU, polyV, pl->n);                   \
                }                                                                               \
                if (!pc[plane_idx].is_wall) {                                                    \
                    for (size_t kk = 0; kk < pc[plane_idx].n_nearby; kk++) {                     \
                        if (!point_in_poly_uv(pc[plane_idx].near_u[kk], pc[plane_idx].near_v[kk], \
                                                polyU, polyV, pl->n)) continue;                  \
                        double w = pc[plane_idx].near_w ? pc[plane_idx].near_w[kk] : 1.0;        \
                        if (w <= 0) continue;                                                   \
                        (out_foreign) += w * pt_edge_dist_uv(pc[plane_idx].near_u[kk],          \
                                                             pc[plane_idx].near_v[kk],          \
                                                             polyU, polyV, pl->n);              \
                    }                                                                            \
                }                                                                                \
            }                                                                                   \
        } while (0)

    // Angle cost (sum over corner's contexts).
    #define ANGLE_COST(ci_eval, c_pt, out_cost)                                                  \
        do {                                                                                    \
            (out_cost) = 0.0;                                                                   \
            for (uint32_t cci = 0; cci < corner_ctx_n[ci_eval]; cci++) {                         \
                const corner_context_t* cx = &ctxs[corner_ctx_indices[ci_eval][cci]];           \
                if (!cx->has_target) continue;                                                   \
                double a = corner_angle_rad(c_pt, cs[cx->prev_idx].point, cs[cx->next_idx].point);\
                double dev = a - cx->frozen_target_rad;                                          \
                (out_cost) += saturate(dev * dev, ANGLE_TWO_SIGMA_SQ, params->cost_shape);       \
            }                                                                                   \
        } while (0)

    // Neighbor-angle cost: SUM of angleDeviationCost over UNIQUE neighbour ids
    // (excluding ci_eval). Matches v2 boundary-regularize.js L926-937: builds a
    // Set of {prev.id, next.id} across ci_eval's ctxs, deletes self, calls
    // angleDeviationCost(nid) per neighbour — which itself sums saturated dev²
    // over the neighbour's contexts. NO averaging.
    //
    // TOTAL_COST has overwritten cs[ci_eval].point with c_pt, so reading
    // cs[neighbour.prev_idx].point automatically uses c_pt when prev_idx ==
    // ci_eval (likewise for next). The c_pt macro arg is unused here.
    #define NEIGHBOR_ANGLE_COST(ci_eval, c_pt, out_cost)                                         \
        do {                                                                                    \
            (void)(c_pt);                                                                       \
            (out_cost) = 0.0;                                                                   \
            uint32_t _na_neighs[16]; uint32_t _na_n_neighs = 0;                                  \
            for (uint32_t _na_cci = 0; _na_cci < corner_ctx_n[ci_eval]; _na_cci++) {            \
                const corner_context_t* _na_cx = &ctxs[corner_ctx_indices[ci_eval][_na_cci]];   \
                uint32_t _na_cands[2] = { _na_cx->prev_idx, _na_cx->next_idx };                  \
                for (int _na_nk = 0; _na_nk < 2; _na_nk++) {                                     \
                    uint32_t _na_nci = _na_cands[_na_nk];                                        \
                    if (_na_nci == ci_eval) continue;                                            \
                    bool _na_seen = false;                                                       \
                    for (uint32_t _na_i = 0; _na_i < _na_n_neighs; _na_i++) {                    \
                        if (_na_neighs[_na_i] == _na_nci) { _na_seen = true; break; }            \
                    }                                                                            \
                    if (!_na_seen && _na_n_neighs < 16) _na_neighs[_na_n_neighs++] = _na_nci;    \
                }                                                                                \
            }                                                                                    \
            for (uint32_t _na_ni = 0; _na_ni < _na_n_neighs; _na_ni++) {                         \
                uint32_t _na_nci = _na_neighs[_na_ni];                                           \
                for (uint32_t _na_k2 = 0; _na_k2 < corner_ctx_n[_na_nci]; _na_k2++) {            \
                    const corner_context_t* _na_ncx = &ctxs[corner_ctx_indices[_na_nci][_na_k2]];\
                    if (!_na_ncx->has_target) continue;                                          \
                    double _na_a = corner_angle_rad(cs[_na_nci].point,                           \
                                                      cs[_na_ncx->prev_idx].point,               \
                                                      cs[_na_ncx->next_idx].point);              \
                    double _na_dev = _na_a - _na_ncx->frozen_target_rad;                         \
                    (out_cost) += saturate(_na_dev * _na_dev, ANGLE_TWO_SIGMA_SQ, params->cost_shape);\
                }                                                                                \
            }                                                                                    \
        } while (0)

    // Horizontal edge cost: sum over UNIQUE incident polygon edges of
    // saturate(sin²(tilt)). v2 uses sin²(tilt) = dy² / (dx²+dy²+dz²) — EXACT
    // for any tilt, no atan needed (boundary-regularize.js L646-651). Earlier
    // v3 versions iterated per-ctx prev/next (double-counting edges shared
    // between polygons) AND used atan2(|dy|, horiz)² which diverges from
    // sin² for non-tiny tilts. Now uses pw_edges (deduped polygon edge list,
    // shared with R.7).
    //
    // TOTAL_COST has overwritten cs[ci_eval].point with c_pt, so reading
    // pw_edges endpoint positions via cs[].point picks up c_pt for ci_eval.
    #define HORIZ_EDGE_COST(ci_eval, c_pt, out_cost)                                             \
        do {                                                                                    \
            (void)(c_pt);                                                                       \
            (out_cost) = 0.0;                                                                   \
            for (uint32_t _he_k = 0; _he_k < corner_to_pw_n[ci_eval]; _he_k++) {                 \
                uint32_t _he_pe = corner_to_pw_idxs[ci_eval][_he_k];                             \
                const double* _he_pa = cs[pw_edges[_he_pe].a_ci].point;                          \
                const double* _he_pb = cs[pw_edges[_he_pe].b_ci].point;                          \
                double _he_dx = _he_pb[0] - _he_pa[0];                                           \
                double _he_dy = _he_pb[1] - _he_pa[1];                                           \
                double _he_dz = _he_pb[2] - _he_pa[2];                                           \
                double _he_len2 = _he_dx*_he_dx + _he_dy*_he_dy + _he_dz*_he_dz;                 \
                if (_he_len2 < 1e-8) continue;                                                   \
                double _he_sin_tilt_sq = (_he_dy * _he_dy) / _he_len2;                           \
                (out_cost) += saturate(_he_sin_tilt_sq, HORIZ_TWO_SIGMA_SQ, params->cost_shape); \
            }                                                                                    \
        } while (0)

    // Helper: total cost at hypothetical point `c_pt` for corner ci.
    // Profiling: per-metric timers + outer "scoring" timer (br_p_R1_score)
    // accumulate inside this macro. When br_prof_on==false the conditional
    // checks compile-away to negligible overhead.
    #define TOTAL_COST(ci_eval, c_pt, out_cost)                                                  \
        do {                                                                                    \
            uint64_t _br_t_score_start = 0; if (br_prof_on) BR_NOW(_br_t_score_start);           \
            (out_cost) = 0.0;                                                                   \
            /* Save current position, swap in trial. */                                          \
            double saved[3] = { cs[ci_eval].point[0], cs[ci_eval].point[1], cs[ci_eval].point[2] };\
            cs[ci_eval].point[0] = c_pt[0]; cs[ci_eval].point[1] = c_pt[1]; cs[ci_eval].point[2] = c_pt[2];\
            if (params->use_wsd_cost) {                                                          \
                uint64_t _t0 = 0; if (br_prof_on) BR_NOW(_t0);                                   \
                /* Affected planes: those carrying ci_eval. */                                   \
                double wsd_sum = 0;                                                              \
                for (uint32_t cci = 0; cci < corner_ctx_n[ci_eval]; cci++) {                     \
                    uint32_t plane_id = ctxs[corner_ctx_indices[ci_eval][cci]].plane_id;         \
                    uint32_t pi_idx = id_lk(&plane_map, plane_id);                               \
                    if (pi_idx == UINT32_MAX) continue;                                           \
                    bool seen = false;                                                           \
                    for (uint32_t cck = 0; cck < cci; cck++) {                                   \
                        if (ctxs[corner_ctx_indices[ci_eval][cck]].plane_id == plane_id) { seen = true; break; } \
                    }                                                                            \
                    if (seen) continue;                                                          \
                    double wsd_p; WSD_EVAL(pi_idx, wsd_p);                                       \
                    wsd_sum += (br_wsd_norm && pc[pi_idx].sum_w > 0.0)                            \
                                 ? wsd_p / pc[pi_idx].sum_w : wsd_p;                              \
                }                                                                                \
                (out_cost) += params->lambda_wsd * (wsd_sum / params->wsd_sigma_m);              \
                BR_ACCUM(br_m_wsd, _t0);                                                         \
            }                                                                                   \
            /* MARGINAL compactness guard: += lambda_compact · Σ compactness over the */        \
            /* corner's (deduped) planes. Combined with accept-if-cost-drops, this rejects */   \
            /* a move that worsens a plane's shape without a WSD gain. Same plane-dedup */       \
            /* as the WSD loop above. */                                                         \
            if (br_lambda_compact > 0.0) {                                                       \
                double _compact_sum = 0;                                                         \
                for (uint32_t cci = 0; cci < corner_ctx_n[ci_eval]; cci++) {                     \
                    uint32_t plane_id = ctxs[corner_ctx_indices[ci_eval][cci]].plane_id;         \
                    uint32_t pi_idx = id_lk(&plane_map, plane_id);                               \
                    if (pi_idx == UINT32_MAX) continue;                                          \
                    bool seen = false;                                                           \
                    for (uint32_t cck = 0; cck < cci; cck++)                                     \
                        if (ctxs[corner_ctx_indices[ci_eval][cck]].plane_id == plane_id) { seen = true; break; } \
                    if (seen) continue;                                                          \
                    double _cmp; COMPACT_EVAL(pi_idx, _cmp);                                     \
                    _compact_sum += _cmp;                                                        \
                }                                                                               \
                (out_cost) += br_lambda_compact * _compact_sum;                                  \
            }                                                                                   \
            /* Vertex-support force: += lambda_support · Σ max(0, dist(corner, nearest */        \
            /* owner) − tol) over the corner's (deduped) planes. Gradient pulls an */            \
            /* unsupported vertex toward its points. Same plane-dedup as WSD. */                 \
            if (br_lambda_support > 0.0) {                                                       \
                double _sup_sum = 0;                                                             \
                for (uint32_t cci = 0; cci < corner_ctx_n[ci_eval]; cci++) {                     \
                    uint32_t plane_id = ctxs[corner_ctx_indices[ci_eval][cci]].plane_id;         \
                    uint32_t pi_idx = id_lk(&plane_map, plane_id);                               \
                    if (pi_idx == UINT32_MAX || !pc[pi_idx].have) continue;                      \
                    bool seen = false;                                                           \
                    for (uint32_t cck = 0; cck < cci; cck++)                                     \
                        if (ctxs[corner_ctx_indices[ci_eval][cck]].plane_id == plane_id) { seen = true; break; } \
                    if (seen) continue;                                                          \
                    double _su, _sv; project_uv(&pc[pi_idx].basis,                               \
                        (c_pt)[0], (c_pt)[1], (c_pt)[2], &_su, &_sv);                             \
                    double _mind = 1e30;                                                         \
                    for (size_t _q = 0; _q < pc[pi_idx].n_owner; _q++) {                         \
                        double _du = _su - pc[pi_idx].own_u[_q], _dv = _sv - pc[pi_idx].own_v[_q]; \
                        double _d2 = _du*_du + _dv*_dv; if (_d2 < _mind) _mind = _d2;             \
                    }                                                                            \
                    double _sd = sqrt(_mind);                                                    \
                    if (_sd > br_support_tol) _sup_sum += (_sd - br_support_tol);                \
                }                                                                               \
                (out_cost) += br_lambda_support * _sup_sum;                                      \
            }                                                                                   \
            double ac, nac, hc;                                                                  \
            if (params->use_angle_cost)            { uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0); ANGLE_COST(ci_eval, c_pt, ac);   (out_cost) += params->lambda_angle * ac; BR_ACCUM(br_m_angle, _t0); } \
            if (params->use_neighbor_angle_cost)   { uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0); NEIGHBOR_ANGLE_COST(ci_eval, c_pt, nac); (out_cost) += params->lambda_neighbor_angle * nac; BR_ACCUM(br_m_nbr_angle, _t0); } \
            if (params->use_horizontal_edge_cost)  { uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0); HORIZ_EDGE_COST(ci_eval, c_pt, hc); (out_cost) += params->lambda_horizontal_edge * hc; BR_ACCUM(br_m_horiz_edge, _t0); } \
            /* R.7 pairwise terms (hcop/vcop/collinear). Pair-cost is averaged */ \
            /* over polygon-edge pairs where ≥1 edge is incident to ci_eval, */    \
            /* filtered to those with cost < ACTIVE_THRESH. Returns 1.0 when */    \
            /* no active partners (v2 fallback). */                                 \
            if (params->use_horiz_coplanar_cost) {                                  \
                uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0);                        \
                double _sigma_sq = params->horiz_coplanar_sigma_m * params->horiz_coplanar_sigma_m; \
                double _v = pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, \
                    corner_to_pw_idxs[ci_eval], corner_to_pw_n[ci_eval],            \
                    incident_mark, pair_hcop_cost,                                  \
                    2.0 * _sigma_sq, params->cost_shape);                           \
                (out_cost) += params->lambda_horiz_coplanar * _v;                   \
                BR_ACCUM(br_m_hcop, _t0);                                            \
            }                                                                       \
            if (params->use_vert_coplanar_cost) {                                   \
                uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0);                        \
                double _sigma_sq = params->vert_coplanar_sigma_m * params->vert_coplanar_sigma_m; \
                double _v = pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, \
                    corner_to_pw_idxs[ci_eval], corner_to_pw_n[ci_eval],            \
                    incident_mark, pair_vcop_cost,                                  \
                    2.0 * _sigma_sq, params->cost_shape);                           \
                (out_cost) += params->lambda_vert_coplanar * _v;                    \
                BR_ACCUM(br_m_vcop, _t0);                                            \
            }                                                                       \
            if (params->use_collinear_cost) {                                       \
                uint64_t _t0=0; if (br_prof_on) BR_NOW(_t0);                        \
                double _sigma_sq = params->collinear_sigma_m * params->collinear_sigma_m; \
                double _v = pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, \
                    corner_to_pw_idxs[ci_eval], corner_to_pw_n[ci_eval],            \
                    incident_mark, pair_collinear_cost,                             \
                    2.0 * _sigma_sq, params->cost_shape);                           \
                (out_cost) += params->lambda_collinear * _v;                        \
                BR_ACCUM(br_m_collin, _t0);                                          \
            }                                                                       \
            cs[ci_eval].point[0] = saved[0]; cs[ci_eval].point[1] = saved[1]; cs[ci_eval].point[2] = saved[2];\
            BR_ACCUM(br_p_R1_score, _br_t_score_start);                              \
        } while (0)

    // EVAL_CAND: score a candidate position and update (best_cost, best_pt), with
    // the GATED marginal-compactness veto, applied PER-PLANE. Expects locals in
    // scope at the call site: ci, best_cost, best_pt[3], and (when br_compact_gate)
    // the gate reference arrays _gate_n / _gate_pl[] / _gate_cur_w[] / _gate_cur_c[]
    // (per-plane current WSD & compactness). Veto if ANY of the corner's planes has
    // its compactness worsened while ITS OWN WSD does not improve by > eps — "worse
    // shape for nothing on WSD, for the CONSIDERED plane". Per-plane is essential:
    // a shared corner's big-plane WSD noise must not excuse wrecking a small plane's
    // shape.
    #define EVAL_CAND(cand)                                                                     \
        do {                                                                                    \
            double _ec_cost; TOTAL_COST(ci, (cand), _ec_cost);                                   \
            bool _ec_veto = false;                                                              \
            if (br_compact_gate) {                                                              \
                double _ec_sv[3] = { cs[ci].point[0], cs[ci].point[1], cs[ci].point[2] };        \
                cs[ci].point[0]=(cand)[0]; cs[ci].point[1]=(cand)[1]; cs[ci].point[2]=(cand)[2]; \
                GATE_VETO_NOW(_ec_veto);                                                         \
                cs[ci].point[0]=_ec_sv[0]; cs[ci].point[1]=_ec_sv[1]; cs[ci].point[2]=_ec_sv[2]; \
            }                                                                                   \
            if (!_ec_veto && _ec_cost < best_cost - 1e-9) {                                      \
                best_cost = _ec_cost;                                                           \
                best_pt[0]=(cand)[0]; best_pt[1]=(cand)[1]; best_pt[2]=(cand)[2];                \
            }                                                                                   \
        } while (0)

    // ── Joint-move infrastructure (5.7g.3) ───────────────────────
    //
    // mulberry32: deterministic 32-bit PRNG, seeded by joint_move_seed.
    // Returns a double in [0, 1). When seed == 0, the impl uses
    // rand()/RAND_MAX for non-reproducible runs (matches v2 behaviour).
    uint32_t mb_state = params->joint_move_seed;

    // Joint edge list: (a_ci, b_ci) pairs where both alive + DOF>0,
    // adjacent in some polygon path. Rebuilt after each topology change.
    typedef struct { uint32_t a, b; } joint_edge_t;
    joint_edge_t* joint_edges = NULL;
    size_t n_joint_edges = 0, cap_joint_edges = 0;
    bool joint_dirty = true;  // initial build, then dirty on each R.2/R.3 fire

    // Pairwise edge list (R.7): every (a_ci, b_ci) adjacent in some
    // polygon path, both alive, DEDUPED across polygons. Includes
    // DOF=0 corners (a stationary endpoint still contributes to the
    // pair geometry). Used by pairwise_cost_for_corner.
    pairwise_edge_t* pw_edges = NULL;
    size_t n_pw = 0, cap_pw = 0;
    // Per-corner incidence: corner_to_pw_idxs[ci] holds indices into pw_edges.
    uint32_t** corner_to_pw_idxs = (uint32_t**)safe_calloc_array(n_cap, sizeof(uint32_t*));
    uint32_t*  corner_to_pw_n    = (uint32_t*) safe_calloc_array(n_cap, sizeof(uint32_t));
    uint32_t*  corner_to_pw_cap  = (uint32_t*) safe_calloc_array(n_cap, sizeof(uint32_t));
    bool*      incident_mark     = NULL;
    size_t     incident_mark_cap = 0;
    if (!corner_to_pw_idxs || !corner_to_pw_n || !corner_to_pw_cap) {
        rc = -1; goto r1_pw_oom;
    }

    #define REBUILD_JOINT_EDGES()                                                                   \
        do {                                                                                       \
            n_joint_edges = 0;                                                                      \
            for (size_t _pi = 0; _pi < n_polygon_paths; _pi++) {                                   \
                poly_t* _p = &polys[_pi];                                                          \
                if (_p->n < 3) continue;                                                            \
                for (uint32_t _i = 0; _i < _p->n; _i++) {                                          \
                    uint32_t _aid = _p->ids[_i];                                                    \
                    uint32_t _bid = _p->ids[(_i + 1) % _p->n];                                      \
                    if (_aid == _bid) continue;                                                    \
                    uint32_t _aci = id_lk(&corner_map, _aid);                                       \
                    uint32_t _bci = id_lk(&corner_map, _bid);                                       \
                    if (_aci == UINT32_MAX || _bci == UINT32_MAX) continue;                        \
                    if (!cs[_aci].alive || !cs[_bci].alive) continue;                              \
                    if (cs[_aci].dof == 0 || cs[_bci].dof == 0) continue;                          \
                    /* Order canonical so dedup catches reverse-traversal across plane pairs. */    \
                    uint32_t _lo = _aci < _bci ? _aci : _bci;                                       \
                    uint32_t _hi = _aci < _bci ? _bci : _aci;                                       \
                    bool _dup = false;                                                              \
                    for (size_t _j = 0; _j < n_joint_edges; _j++) {                                \
                        if (joint_edges[_j].a == _lo && joint_edges[_j].b == _hi) { _dup = true; break; } \
                    }                                                                              \
                    if (_dup) continue;                                                            \
                    if (n_joint_edges == cap_joint_edges) {                                        \
                        size_t _nc = cap_joint_edges == 0 ? 32 : cap_joint_edges * 2;              \
                        joint_edge_t* _nb = (joint_edge_t*)safe_realloc_array(joint_edges, _nc, sizeof(*joint_edges)); \
                        if (!_nb) { rc = -1; goto r1_joint_oom; }                                  \
                        joint_edges = _nb; cap_joint_edges = _nc;                                  \
                    }                                                                              \
                    joint_edges[n_joint_edges].a = _lo;                                            \
                    joint_edges[n_joint_edges].b = _hi;                                            \
                    n_joint_edges++;                                                                \
                }                                                                                   \
            }                                                                                       \
            joint_dirty = false;                                                                    \
        } while (0)

    // Rebuild pairwise edge list + per-corner incidence. Called once
    // before R.1 and again after every R.2/R.3 topology change. Also
    // grows incident_mark if cap_pw grew, and zeroes it.
    #define REBUILD_PW_EDGES()                                                                  \
        do {                                                                                    \
            n_pw = 0;                                                                            \
            for (size_t _ci = 0; _ci < n_corners; _ci++) corner_to_pw_n[_ci] = 0;               \
            for (size_t _pi = 0; _pi < n_polygon_paths; _pi++) {                                \
                poly_t* _p = &polys[_pi];                                                       \
                if (_p->n < 2) continue;                                                         \
                for (uint32_t _i = 0; _i < _p->n; _i++) {                                       \
                    uint32_t _aid = _p->ids[_i];                                                 \
                    uint32_t _bid = _p->ids[(_i + 1) % _p->n];                                   \
                    if (_aid == _bid) continue;                                                 \
                    uint32_t _aci = id_lk(&corner_map, _aid);                                    \
                    uint32_t _bci = id_lk(&corner_map, _bid);                                    \
                    if (_aci == UINT32_MAX || _bci == UINT32_MAX) continue;                     \
                    if (!cs[_aci].alive || !cs[_bci].alive) continue;                           \
                    uint32_t _lo = _aci < _bci ? _aci : _bci;                                    \
                    uint32_t _hi = _aci < _bci ? _bci : _aci;                                    \
                    bool _dup = false;                                                           \
                    for (size_t _j = 0; _j < n_pw; _j++) {                                       \
                        if (pw_edges[_j].a_ci == _lo && pw_edges[_j].b_ci == _hi) { _dup = true; break; } \
                    }                                                                            \
                    if (_dup) continue;                                                          \
                    if (n_pw == cap_pw) {                                                        \
                        size_t _nc = cap_pw == 0 ? 32 : cap_pw * 2;                              \
                        pairwise_edge_t* _nb = (pairwise_edge_t*)safe_realloc_array(             \
                            pw_edges, _nc, sizeof(*pw_edges));                                   \
                        if (!_nb) { rc = -1; goto r1_pw_oom; }                                  \
                        pw_edges = _nb; cap_pw = _nc;                                            \
                    }                                                                            \
                    pw_edges[n_pw].a_ci = _lo;                                                   \
                    pw_edges[n_pw].b_ci = _hi;                                                   \
                    for (int _e = 0; _e < 2; _e++) {                                             \
                        uint32_t _c = (_e == 0) ? _lo : _hi;                                     \
                        if (corner_to_pw_n[_c] == corner_to_pw_cap[_c]) {                        \
                            uint32_t _nc2 = corner_to_pw_cap[_c] == 0 ? 4 : corner_to_pw_cap[_c] * 2; \
                            uint32_t* _nb2 = (uint32_t*)safe_realloc_array(                      \
                                corner_to_pw_idxs[_c], _nc2, sizeof(uint32_t));                  \
                            if (!_nb2) { rc = -1; goto r1_pw_oom; }                              \
                            corner_to_pw_idxs[_c] = _nb2; corner_to_pw_cap[_c] = _nc2;           \
                        }                                                                        \
                        corner_to_pw_idxs[_c][corner_to_pw_n[_c]++] = (uint32_t)n_pw;            \
                    }                                                                            \
                    n_pw++;                                                                      \
                }                                                                                \
            }                                                                                    \
            if (incident_mark_cap < cap_pw) {                                                    \
                bool* _nb3 = (bool*)safe_realloc_array(incident_mark, cap_pw, sizeof(bool));    \
                if (!_nb3) { rc = -1; goto r1_pw_oom; }                                          \
                incident_mark = _nb3; incident_mark_cap = cap_pw;                                \
            }                                                                                    \
            if (incident_mark_cap > 0) memset(incident_mark, 0, incident_mark_cap * sizeof(bool)); \
        } while (0)

    REBUILD_PW_EDGES();

    // (RECONCILE_PLANES_DOWN deleted 2026-06-19: membership is now derived
    //  on demand via planes_of() from the canonical polys[] — there is no
    //  stored planes_dyn to reconcile. The path mutations in MERGE_CORNERS /
    //  R.2 / R.3 ARE the membership update.)

    // ── RETAG_DOF: recompute a corner's DOF + freedom axes from its
    //    CURRENT path membership (planes_of, the SSOT) (>=3 → 0 locked;
    //    ==2 → 1 ridge-slide with axis_a = normalize(nA×nB); ==1 → 2 surface
    //    with e1/e2). Called on the survivor after EVERY merge (R.2/R.3/R.4).
    //    Reads membership via planes_of (path-derived) — no stored planes_dyn.
    //    Guards nA×nB ≈ 0 (near-parallel planes): leaves DOF=1 but clears
    //    has_axis_a so the corner won't slide along a NaN axis.
    #define RETAG_DOF(_ci)                                                                          \
        do {                                                                                       \
            uint32_t _rt_pids[PLANES_OF_SCRATCH];                                                   \
            uint32_t _rt_np = planes_of((_ci), corners_in, polys, n_polygon_paths,                  \
                                        _rt_pids, PLANES_OF_SCRATCH);                                \
            if (_rt_np >= 3) {                                                                       \
                cs[_ci].dof = 0; cs[_ci].has_axis_a = false; cs[_ci].has_axis_b = false;            \
            } else if (_rt_np == 2) {                                                                \
                cs[_ci].dof = 1; cs[_ci].has_axis_b = false; cs[_ci].has_axis_a = false;            \
                uint32_t _rt_pa = id_lk(&plane_map, _rt_pids[0]);                                   \
                uint32_t _rt_pb = id_lk(&plane_map, _rt_pids[1]);                                   \
                if (_rt_pa != UINT32_MAX && _rt_pb != UINT32_MAX) {                                  \
                    const double *_rt_nA = planes[_rt_pa].normal, *_rt_nB = planes[_rt_pb].normal;   \
                    double _rt_lx = _rt_nA[1]*_rt_nB[2] - _rt_nA[2]*_rt_nB[1];                        \
                    double _rt_ly = _rt_nA[2]*_rt_nB[0] - _rt_nA[0]*_rt_nB[2];                        \
                    double _rt_lz = _rt_nA[0]*_rt_nB[1] - _rt_nA[1]*_rt_nB[0];                        \
                    double _rt_ll = sqrt(_rt_lx*_rt_lx + _rt_ly*_rt_ly + _rt_lz*_rt_lz);             \
                    if (_rt_ll > 1e-8) {                                                             \
                        cs[_ci].has_axis_a = true;                                                    \
                        cs[_ci].axis_a[0] = _rt_lx/_rt_ll; cs[_ci].axis_a[1] = _rt_ly/_rt_ll;        \
                        cs[_ci].axis_a[2] = _rt_lz/_rt_ll;                                           \
                    }                                                                                 \
                }                                                                                     \
            } else if (_rt_np == 1) {                                                                \
                cs[_ci].dof = 2; cs[_ci].has_axis_a = false; cs[_ci].has_axis_b = false;             \
                uint32_t _rt_pi = id_lk(&plane_map, _rt_pids[0]);                                   \
                if (_rt_pi != UINT32_MAX) {                                                          \
                    const double* _rt_n = planes[_rt_pi].normal;                                      \
                    double _rt_ux, _rt_uy, _rt_uz;                                                   \
                    if (fabs(_rt_n[1]) > 0.9) { _rt_ux = 1; _rt_uy = 0; _rt_uz = 0; }                \
                    else                       { _rt_ux = 0; _rt_uy = 1; _rt_uz = 0; }               \
                    double _rt_e1x = _rt_uy*_rt_n[2] - _rt_uz*_rt_n[1];                               \
                    double _rt_e1y = _rt_uz*_rt_n[0] - _rt_ux*_rt_n[2];                               \
                    double _rt_e1z = _rt_ux*_rt_n[1] - _rt_uy*_rt_n[0];                               \
                    double _rt_e1l = sqrt(_rt_e1x*_rt_e1x + _rt_e1y*_rt_e1y + _rt_e1z*_rt_e1z);      \
                    if (_rt_e1l < EPS) _rt_e1l = 1.0;                                                \
                    _rt_e1x /= _rt_e1l; _rt_e1y /= _rt_e1l; _rt_e1z /= _rt_e1l;                       \
                    double _rt_e2x = _rt_n[1]*_rt_e1z - _rt_n[2]*_rt_e1y;                             \
                    double _rt_e2y = _rt_n[2]*_rt_e1x - _rt_n[0]*_rt_e1z;                             \
                    double _rt_e2z = _rt_n[0]*_rt_e1y - _rt_n[1]*_rt_e1x;                             \
                    cs[_ci].has_axis_a = true; cs[_ci].has_axis_b = true;                            \
                    cs[_ci].axis_a[0] = _rt_e1x; cs[_ci].axis_a[1] = _rt_e1y; cs[_ci].axis_a[2] = _rt_e1z; \
                    cs[_ci].axis_b[0] = _rt_e2x; cs[_ci].axis_b[1] = _rt_e2y; cs[_ci].axis_b[2] = _rt_e2z; \
                }                                                                                     \
            }                                                                                         \
            cs[_ci].n_planes = _rt_np;                                                                \
            COUPLE_RIDGE_DOF(_ci);  /* A2: re-apply min-DOF over the (unioned) ridge set */            \
        } while (0)

    // ── #25 INSERT_CORNER_AT: mint a new corner at UV (_mu,_mv) on plane _p_pi's
    //    polygon _p_pp, spliced into the ring after edge _ek. FATAL-aborts past the
    //    buffer cap (fail-loud, Release-safe). Caller MUST self-int-check the
    //    would-be ring first. RETAG_DOF sets DOF-2 + in-plane axes for the fresh
    //    single-plane corner; the optimizer refines it on subsequent iterations.
    #define INSERT_CORNER_AT(_p_pp, _p_pi, _ek, _mu, _mv)                                              \
        do {                                                                                          \
            if (n_corners >= n_cap) {                                                                  \
                fprintf(stderr, "[v7-boundary-regularize] FATAL: corner-insertion exceeded "           \
                    "N_INSERT_MAX=%d (n_corners=%zu). Raise cap or investigate.\n", (int)N_INSERT_MAX, n_corners); \
                abort();                                                                              \
            }                                                                                         \
            poly_t* _ins_p = &polys[(_p_pp)];                                                          \
            if (!poly_reserve(_ins_p, _ins_p->n + 1)) { rc = -1; goto r1_ctx_oom; }                     \
            size_t   _ins_ci = n_corners++;                                                            \
            uint32_t _ins_id = (uint32_t)(max_corner + 1 + n_inserted); n_inserted++;                  \
            double _ins_p3[3]; uv_to_3d(&pc[(_p_pi)].basis, (_mu), (_mv), _ins_p3);                     \
            memset(&corners_owned[_ins_ci], 0, sizeof(corners_owned[_ins_ci]));                        \
            corners_owned[_ins_ci].id = _ins_id; corners_owned[_ins_ci].source = 4;                    \
            corners_owned[_ins_ci].point[0]=_ins_p3[0]; corners_owned[_ins_ci].point[1]=_ins_p3[1]; corners_owned[_ins_ci].point[2]=_ins_p3[2]; \
            cs[_ins_ci].alive=true; cs[_ins_ci].n_planes=1; cs[_ins_ci].moved=false;                   \
            cs[_ins_ci].ridge_ids=NULL; cs[_ins_ci].n_ridge_ids=0;                                     \
            cs[_ins_ci].point[0]=_ins_p3[0]; cs[_ins_ci].point[1]=_ins_p3[1]; cs[_ins_ci].point[2]=_ins_p3[2]; \
            id_set(&corner_map, _ins_id, (uint32_t)_ins_ci);                                           \
            for (uint32_t _ins_s=_ins_p->n; _ins_s>(uint32_t)(_ek)+1; _ins_s--) _ins_p->ids[_ins_s]=_ins_p->ids[_ins_s-1]; \
            _ins_p->ids[(_ek)+1]=_ins_id; _ins_p->n++; _ins_p->changed=true;                           \
            total_ctx++;                                                                               \
            REBUILD_CTXS(); RETAG_DOF(_ins_ci); REBUILD_PW_EDGES();                                    \
        } while (0)

    // ── MERGE_CORNERS: the "redirectAndRebuild" idiom factored out of the
    //    R.2/R.3 copy-paste so R.4 can reuse it. Rewrites every polys[].ids
    //    loser_id→surv_id with consecutive+wrap dedup (paths <3 zeroed) — the
    //    path rewrite IS the membership update (SSOT; survivor's planes are now
    //    derived via planes_of from those paths). Marks loser dead, re-tags the
    //    survivor's DOF, and rebuilds the derived caches.
    //    Sets joint_dirty. On OOM jumps to r1_traj_oom (in scope inside R.1).
    //    NOTE: assumes survivor != loser; whether they already share a plane is
    //    irrelevant (the rewrite/dedup handles it). After this, `_surv` is the
    //    surviving corner index the caller should chase from.
    #define MERGE_CORNERS(_surv, _lose)                                                              \
        do {                                                                                         \
            /* A2 (§4.6): additive ridge_ids union — survivor gets the loser's   */                   \
            /* on-ridge tags too. RETAG_DOF                                      */                   \
            /* below then re-couples DOF over the merged ridge set. One-way.     */                   \
            for (uint32_t _rk = 0; _rk < cs[_lose].n_ridge_ids; _rk++) {                               \
                if (!ridge_ids_push_unique(&cs[_surv].ridge_ids, &cs[_surv].n_ridge_ids,               \
                                           cs[_lose].ridge_ids[_rk])) { rc = -1; goto r1_traj_oom; }    \
            }                                                                                         \
            uint32_t _loser_id = corners_in[_lose].id;                                                \
            uint32_t _surv_id  = corners_in[_surv].id;                                                \
            for (size_t _pi = 0; _pi < n_polygon_paths; _pi++) {                                      \
                poly_t* _p = &polys[_pi];                                                             \
                bool _touched = false;                                                               \
                for (uint32_t _i = 0; _i < _p->n; _i++) {                                             \
                    if (_p->ids[_i] == _loser_id) { _p->ids[_i] = _surv_id; _touched = true; }        \
                }                                                                                     \
                if (!_touched) continue;                                                             \
                uint32_t _w = 0;                                                                      \
                for (uint32_t _r2 = 0; _r2 < _p->n; _r2++) {                                          \
                    if (_w == 0 || _p->ids[_w-1] != _p->ids[_r2]) _p->ids[_w++] = _p->ids[_r2];       \
                }                                                                                     \
                if (_w >= 2 && _p->ids[0] == _p->ids[_w-1]) _w--;                                      \
                _p->n = (_w >= 3) ? _w : 0;                                                            \
                _p->changed = true;                                                                   \
            }                                                                                         \
            cs[_lose].alive = false;                                                                  \
            /* Membership is path-derived (planes_of) — the rewrite above already */                  \
            /* updated _surv's planes. RETAG_DOF reads the fresh paths.           */                  \
            RETAG_DOF(_surv);                                                                          \
            REBUILD_CTXS();                                                                           \
            REBUILD_PW_EDGES();                                                                       \
            joint_dirty = true;                                                                       \
        } while (0)

    // ── R.4 ridge geometry precompute ────────────────────────────────
    // (rg[] is now built ABOVE the R.0 DOF-tag pass — A2 §4.6 needs it for
    //  ridge-DOF coupling at tag time. See the hoisted block near R.0.)
    // Per-ridge "did it fire >=1 merge" flag for the R.4 summary.
    bool* ridge_xplane_hit = NULL;
    if (params->use_ridge_merge && n_ridges > 0) {
        ridge_xplane_hit = (bool*)safe_calloc_array(n_ridges, sizeof(bool));
        if (!ridge_xplane_hit) { rc = -1; goto r1_xplane_oom; }
    }

    // T4: scratch for ridges_incident_to (snap-in + guard preview). Sized to
    // the full ridge count (a safe upper bound on the dedup'd incident set).
    // Stays NULL when no ridges. Freed at r1_xplane_oom (declared at top of fn).
    if (n_ridges > 0) {
        ridge_inc_buf = (uint32_t*)safe_calloc_array(n_ridges, sizeof(uint32_t));
        if (!ridge_inc_buf) { rc = -1; goto r1_xplane_oom; }
    }
    // T5: would-be-ring scratch for the MERGE_CORNER pre-commit self-int guard.
    if (n_corners > 0) {
        mc_id_scratch = (uint32_t*)safe_calloc_array(n_cap, sizeof(uint32_t));
        mc_u_scratch  = (double*)  safe_calloc_array(n_cap, sizeof(double));
        mc_v_scratch  = (double*)  safe_calloc_array(n_cap, sizeof(double));
        if (!mc_id_scratch || !mc_u_scratch || !mc_v_scratch) { rc = -1; goto r1_xplane_oom; }
    }
    // T4 make-shared PREVIEW + snap-in tallies (printed under BR4_TRACE).
    uint32_t ms_snap_in_count = 0;   // corners that snapped onto a NEW ridge mid-loop
    uint32_t ms_would_share_A = 0;   // cond-A (both-R B path-edge) would-share previews
    uint32_t ms_would_share_B = 0;   // cond-B (B corner within τ_partner) would-share previews
    // T5 MERGE_CORNER decomposition tally (printed under BR4_TRACE): how many
    // candidate cross-plane welds the pre-commit self-int reject dropped — for
    // the merge-count watch-item. (Type-gate drops are attributed by comparing
    // welded ridge types in the trace against the {ridge,hip,valley} allowlist.)
    uint32_t mc_selfint_reject = 0;
    // T7 SPLIT decomposition tallies (printed under BR4_TRACE):
    uint32_t sp_selfint_reject = 0;   // M5: would-be ring self-int in B's UV → REJECT
    uint32_t sp_tie_refuse     = 0;    // folded-ridge: two both-R B edges within ε → REFUSE (don't guess the arm)
    uint32_t sp_junction_refuse= 0;    // M4(b): SPLIT target is a real ≥3-plane junction needing a meet → REFUSE (T8)
    uint32_t sp_oom_reject     = 0;    // poly_reserve/ctx_reserve OOM → clean REJECT (nothing mutated)

    // ── MERGE_CORNER_XPLANE (T5): the cross-plane weld that REPLACES the legacy
    //    R.4 (§4.0/§4.1; the MERGE_CORNER half of "SHARE"). One weld per
    //    invocation. For each ALLOW-LISTED ridge ({ridge,hip,valley}, I1 §9)
    //    with valid geometry, find a plane-A corner ON the crease and its
    //    nearest valid plane-B partner within ridge_merge_dist_m that does NOT
    //    already share a plane (strictly cross-plane), pick the survivor by the
    //    R.2 tie-break (dof asc, plane-count desc, id asc → M4(a): a DOF-0
    //    junction wins and the corner merges INTO it), place the survivor at
    //    the constraint-aware POOL-SOLVER meet (not a midpoint — §4.1/§4.5(1),
    //    via v7p_solve_member_position), REJECT pre-commit if the weld would
    //    self-intersect any plane the survivor will belong to (M5 tier-1,
    //    v7_local_edges_cross_uv — nothing committed = clean revert), else
    //    MERGE_CORNERS. Sets _mc_fired=true and updates `_chase` to the survivor.
    //    Type-gate fixes an R.4 bug (R.4 ignored rg[].type, welding step eaves).
    //    Still gated by BR_NO_R4 (the A/B lever). `_chase` is an lvalue.
    #define MERGE_CORNER_XPLANE(_chase, _mc_fired)                                                   \
        do {                                                                                         \
            (_mc_fired) = false;                                                                     \
            if (params->use_ridge_merge && rg) {                                                     \
                const double _cop   = params->ridge_merge_coplanar_m;                                \
                const double _cop2  = _cop * _cop;                                                   \
                const double _md2   = params->ridge_merge_dist_m * params->ridge_merge_dist_m;       \
                for (size_t _ri = 0; _ri < n_ridges && !(_mc_fired); _ri++) {                         \
                    if (rg[_ri].tmax < 0) continue;                                                  \
                    /* I1 (§9): only weld roof–roof creases; step (roof–wall eave) */                \
                    /* + coplanar (degenerate) are EXCLUDED. Fixes an R.4 bug.     */                \
                    if (!RIDGE_TYPE_ALLOWED(rg[_ri].type)) continue;                                 \
                    uint32_t _A = rg[_ri].plane_a, _B = rg[_ri].plane_b;                              \
                    /* Decouple: never weld across plane types, even on an allow-listed */            \
                    /* ridge — a ridge/hip/valley can still span roof↔wall/ground.      */            \
                    { uint32_t _ia = id_lk(&plane_map,_A), _ib = id_lk(&plane_map,_B);                \
                      if (_ia >= (uint32_t)n_planes || _ib >= (uint32_t)n_planes ||                   \
                          !rg_plane_types_mergeable((rg_plane_type_t)pc[_ia].plane_type,              \
                                                    (rg_plane_type_t)pc[_ib].plane_type)) continue; } \
                    /* scan A-side corner, then nearest valid B-side corner */                        \
                    for (size_t _cA = 0; _cA < n_corners && !(_mc_fired); _cA++) {                     \
                        if (!cs[_cA].alive) continue;                                                 \
                        if (!corner_has_plane(_cA, _A, corners_in, polys, n_polygon_paths)) continue; \
                        double _tA, _perpA;                                                           \
                        ridge_proj(cs[_cA].point, rg[_ri].rp, rg[_ri].rd, &_tA, &_perpA);             \
                        if (_perpA*_perpA > _cop2) continue;                                          \
                        if (_tA < 0 || _tA > rg[_ri].tmax) continue;                                  \
                        uint32_t _bestB = UINT32_MAX; double _bestd2 = _md2;                          \
                        double _bestPerpB = 0, _besttB = 0;                                           \
                        for (size_t _cB = 0; _cB < n_corners; _cB++) {                                 \
                            if (_cB == _cA || !cs[_cB].alive) continue;                                \
                            if (!corner_has_plane(_cB, _B, corners_in, polys, n_polygon_paths)) continue; \
                            /* strictly cross-plane: must NOT already share any plane with cA */       \
                            if (corners_share_plane(_cA, _cB, corners_in, polys, n_polygon_paths)) continue; \
                            double _tB, _perpB;                                                        \
                            ridge_proj(cs[_cB].point, rg[_ri].rp, rg[_ri].rd, &_tB, &_perpB);          \
                            if (_perpB*_perpB > _cop2) continue;                                       \
                            if (_tB < 0 || _tB > rg[_ri].tmax) continue;                               \
                            double _dx = cs[_cA].point[0] - cs[_cB].point[0];                          \
                            double _dy = cs[_cA].point[1] - cs[_cB].point[1];                          \
                            double _dz = cs[_cA].point[2] - cs[_cB].point[2];                          \
                            double _d2 = _dx*_dx + _dy*_dy + _dz*_dz;                                  \
                            if (_d2 < _bestd2) { _bestd2 = _d2; _bestB = (uint32_t)_cB;                \
                                                 _bestPerpB = _perpB; _besttB = _tB; }                 \
                        }                                                                              \
                        if (_bestB == UINT32_MAX) continue;                                            \
                        /* tie-break (dof asc, plane-count desc, id asc) — same as R.2;  */            \
                        /* the more-constrained corner survives (M4(a) junction wins).   */            \
                        uint32_t _cB = _bestB;                                                         \
                        const uint8_t  _da = cs[_cA].dof,        _db = cs[_cB].dof;                    \
                        const uint32_t _pa = planes_of_n(_cA, corners_in, polys, n_polygon_paths),     \
                                       _pb = planes_of_n(_cB, corners_in, polys, n_polygon_paths);     \
                        const uint32_t _ia = corners_in[_cA].id, _ib = corners_in[_cB].id;            \
                        bool _a_wins = (_da < _db) || (_da == _db && _pa > _pb)                        \
                                                  || (_da == _db && _pa == _pb && _ia < _ib);          \
                        uint32_t _surv = _a_wins ? (uint32_t)_cA : _cB;                                \
                        uint32_t _lose = _a_wins ? _cB : (uint32_t)_cA;                                \
                        /* T5: constraint-aware POSITION via the corner-pool solver    */              \
                        /* (§4.1/§4.5(1)) — NOT a midpoint. Build a transient 2-member  */              \
                        /* array from cA/cB; authority from DOF (0→FULL pin [M4(a):     */              \
                        /* a junction survivor dominates → meet stays at its position], */              \
                        /* 1→AXIAL + crease axis, 2→NONE). Near-parallel axes are        */              \
                        /* guarded INSIDE v7p_solve_member_position.                     */              \
                        v7p_member_t _mm[2];                                                           \
                        for (int _mi = 0; _mi < 2; _mi++) {                                            \
                            uint32_t _ci = (_mi == 0) ? (uint32_t)_cA : _cB;                            \
                            memset(&_mm[_mi], 0, sizeof(_mm[_mi]));                                     \
                            _mm[_mi].x = cs[_ci].point[0];                                              \
                            _mm[_mi].y = cs[_ci].point[1];                                              \
                            _mm[_mi].z = cs[_ci].point[2];                                              \
                            if (cs[_ci].dof == 0) {                                                     \
                                _mm[_mi].authority = V7P_AUTHORITY_FULL;                                \
                            } else if (cs[_ci].dof == 1 && cs[_ci].has_axis_a) {                        \
                                _mm[_mi].authority = V7P_AUTHORITY_AXIAL;                               \
                                _mm[_mi].has_axis  = true;                                              \
                                _mm[_mi].axis[0]   = (float)cs[_ci].axis_a[0];                          \
                                _mm[_mi].axis[1]   = (float)cs[_ci].axis_a[1];                          \
                                _mm[_mi].axis[2]   = (float)cs[_ci].axis_a[2];                          \
                            } else {                                                                    \
                                _mm[_mi].authority = V7P_AUTHORITY_NONE;                                \
                            }                                                                          \
                        }                                                                              \
                        double _meet[3];                                                               \
                        v7p_solve_member_position(_mm, 2, _meet);                                      \
                        /* T5: PRE-COMMIT self-int reject (M5 tier-1). For each plane    */             \
                        /* the survivor will belong to (its planes ∪ loser's), build the */             \
                        /* would-be ring (lose.id→surv.id, survivor at _meet), project   */             \
                        /* to that plane's UV, run the local edge-cross test. On a cross, */             \
                        /* REJECT — nothing mutated yet, so revert is a no-op.            */             \
                        bool _mc_xint = false;                                                          \
                        uint32_t _mc_sp[PLANES_OF_SCRATCH], _mc_lp[PLANES_OF_SCRATCH];                  \
                        uint32_t _mc_sn = planes_of(_surv, corners_in, polys, n_polygon_paths,          \
                                                    _mc_sp, PLANES_OF_SCRATCH);                          \
                        uint32_t _mc_ln = planes_of(_lose, corners_in, polys, n_polygon_paths,          \
                                                    _mc_lp, PLANES_OF_SCRATCH);                          \
                        for (uint32_t _ps = 0; _ps < 2 && !_mc_xint; _ps++) {                           \
                            uint32_t _pn = (_ps == 0) ? _mc_sn : _mc_ln;                                \
                            uint32_t* _pl = (_ps == 0) ? _mc_sp : _mc_lp;                               \
                            for (uint32_t _pk = 0; _pk < _pn && !_mc_xint; _pk++) {                      \
                                uint32_t _pid = _pl[_pk];                                               \
                                uint32_t _pidx = id_lk(&plane_map, _pid);                               \
                                if (_pidx == UINT32_MAX) continue;                                      \
                                for (size_t _pp = 0; _pp < n_polygon_paths && !_mc_xint; _pp++) {        \
                                    poly_t* _pp_p = &polys[_pp];                                         \
                                    if (_pp_p->plane_id != _pid || _pp_p->n < 3) continue;               \
                                    if (merge_would_self_intersect(                                      \
                                            _pp_p->ids, _pp_p->n,                                        \
                                            corners_in[_lose].id, corners_in[_surv].id, _meet,           \
                                            &corner_map, cs, &pc[_pidx].basis,                           \
                                            mc_id_scratch, mc_u_scratch, mc_v_scratch))                  \
                                        _mc_xint = true;                                                 \
                                }                                                                       \
                            }                                                                          \
                        }                                                                              \
                        if (_mc_xint) {                                                                 \
                            mc_selfint_reject++;                                                        \
                            if (br4_trace)                                                              \
                                fprintf(stderr, "[mc-reject] ridge %u|%u (rid=%u) iter=%d cA=%u cB=%u " \
                                    "-> self-int, NOT merged\n", _A, _B, ridges[_ri].id, iter, _ia, _ib);\
                            continue;                                                                   \
                        }                                                                              \
                        /* commit: place survivor at the solved meet, then MERGE. */                    \
                        cs[_surv].point[0] = _meet[0];                                                  \
                        cs[_surv].point[1] = _meet[1];                                                  \
                        cs[_surv].point[2] = _meet[2];                                                  \
                        MERGE_CORNERS(_surv, _lose);                                                    \
                        out->stats.xplane_merged++;       /* cross-plane weld count (trace summary) */ \
                        out->stats.n_merge_corner++;        /* T0: MERGE_CORNER (cross-plane) */        \
                        if (ridge_xplane_hit && !ridge_xplane_hit[_ri]) {                                       \
                            ridge_xplane_hit[_ri] = true; out->stats.ridges_with_xplane_merge++;                \
                        }                                                                              \
                        if (br4_trace) {                                                               \
                            fprintf(stderr, "[merge_corner] ridge %u|%u (rid=%u type=%u) iter=%d "      \
                                "cA=%u cB=%u surv=%u perpA=%.4f perpB=%.4f d=%.4f -> merged "            \
                                "(meet=(%.3f,%.3f,%.3f) planes_of_n=%u dof=%u)\n",                       \
                                _A, _B, ridges[_ri].id, rg[_ri].type, iter, _ia, _ib,                   \
                                corners_in[_surv].id, _perpA, _bestPerpB, sqrt(_bestd2),                 \
                                _meet[0], _meet[1], _meet[2],                                            \
                                planes_of_n(_surv, corners_in, polys, n_polygon_paths), cs[_surv].dof);  \
                        }                                                                              \
                        (void)_besttB;                                                                  \
                        (_chase) = _surv;                                                               \
                        (_mc_fired) = true;                                                             \
                    }                                                                                  \
                }                                                                                      \
            }                                                                                          \
        } while (0)

    // ── SPLIT_TO_SHARE_CORNER (T7): the only GROWING op of the cascade
    //    (§4.0/§4.3). When an A-corner cA (single-plane, DOF-1, on an allow-
    //    listed ridge R) lies on a B-plane path-EDGE (u,v) whose BOTH endpoints
    //    carry ridge_id R, INSERT cA's EXISTING id into polys[B].ids between u,v.
    //    cA becomes shared {A,B} → forms/extends a shared edge. cA does NOT move
    //    (it is already on the edge by construction — the Phase-B dividend: the
    //    path-insert IS the membership update, planes_of derives {A,B} after).
    //
    //    compute-then-commit (§4.3c): PREPARE (fallible, mutates NO canonical
    //    store) re-confirms cond-A FULLY (§9 M1: single-plane, on R, the both-R
    //    B-edge bracket + PERP-to-segment ≤ τ_in + 3D-nearest disambiguation +
    //    tie-REFUSE for a folded ridge + M4(b) junction-REFUSE), runs the M5
    //    self-int reject in B's UV (cA INSERTED, not substituted), then reserves
    //    capacity (poly_reserve + ctx_reserve). COMMIT is allocation-free /
    //    infallible: memmove to open the slot, write cA.id, n++, total_ctx++,
    //    RETAG_DOF, REBUILD_CTXS/PW. Gated by BR_NO_SPLIT (default ON). One
    //    split per invocation. SCANS all corners as the candidate cA (like
    //    MERGE_CORNER_XPLANE's global ridge scan, not just the moved corner):
    //    cond-A candidates are reached via OTHER corners' moves / snap-ins
    //    (a B endpoint moving, or being tagged on R), so a strictly-local
    //    "test only chase_ci" trigger misses them (measured on dahouet: the
    //    census saw 2 cond-A candidates but a chase_ci-only op fired 0).
    #define SPLIT_TO_SHARE_CORNER(_sp_fired)                                                         \
        do {                                                                                         \
            (_sp_fired) = false;                                                                     \
            if (!_br_no_split && params->use_ridge_merge && rg && ridge_inc_buf) {                    \
                const double _sp_tau_in = params->ridge_merge_coplanar_m;                             \
              for (size_t _sp_cand = 0; _sp_cand < n_corners && !(_sp_fired); _sp_cand++) {           \
                if (!cs[_sp_cand].alive) continue;                                                    \
                /* (1) cA must be SINGLE-PLANE (the make-shared target).        */                    \
                uint32_t _sp_Ap[PLANES_OF_SCRATCH];                                                   \
                uint32_t _sp_np = planes_of(_sp_cand, corners_in, polys, n_polygon_paths,             \
                                            _sp_Ap, PLANES_OF_SCRATCH);                                \
                if (_sp_np == 1) {                                                                    \
                    uint32_t _sp_A = _sp_Ap[0];                                                       \
                    int _sp_ni = ridges_incident_to(_sp_cand, rg, n_ridges,                            \
                            cs[_sp_cand].ridge_ids, cs[_sp_cand].n_ridge_ids,                           \
                            cs[_sp_cand].point, _sp_tau_in, _sp_Ap, _sp_np,                            \
                            &plane_map, ridx_off, ridx, ridge_inc_buf);                               \
                    for (int _sp_ii = 0; _sp_ii < _sp_ni && !(_sp_fired); _sp_ii++) {                 \
                        uint32_t _sp_R = ridge_inc_buf[_sp_ii];                                       \
                        uint32_t _sp_B = (rg[_sp_R].plane_a == _sp_A) ? rg[_sp_R].plane_b             \
                                       : (rg[_sp_R].plane_b == _sp_A) ? rg[_sp_R].plane_a             \
                                       : UINT32_MAX;                                                   \
                        if (_sp_B == UINT32_MAX) continue;                                            \
                        if (rg[_sp_R].tmax <= 0.0) continue;                                          \
                        uint32_t _sp_Bidx = id_lk(&plane_map, _sp_B);                                 \
                        if (_sp_Bidx == UINT32_MAX) continue;                                         \
                        /* Decouple: never split-to-share across plane types. */                     \
                        { uint32_t _sp_Aidx = id_lk(&plane_map, _sp_A);                               \
                          if (_sp_Aidx >= (uint32_t)n_planes ||                                       \
                              !rg_plane_types_mergeable((rg_plane_type_t)pc[_sp_Aidx].plane_type,     \
                                                        (rg_plane_type_t)pc[_sp_Bidx].plane_type)) continue; } \
                        double _sp_tA, _sp_pA;                                                        \
                        ridge_proj(cs[_sp_cand].point, rg[_sp_R].rp, rg[_sp_R].rd, &_sp_tA, &_sp_pA);  \
                        if (_sp_pA > _sp_tau_in) continue;                                            \
                        if (_sp_tA < 0.0 || _sp_tA > rg[_sp_R].tmax) continue;                        \
                        /* (2) scan B's path for the both-R bracketing edge(s). Track   */            \
                        /*     the 3D-NEAREST (perp-to-SEGMENT) candidate and the 2nd-   */            \
                        /*     nearest so we can REFUSE a folded-ridge tie (don't guess  */            \
                        /*     the arm). insert_pos = the slot AFTER u in B's path.      */            \
                        uint32_t _sp_bestpi = UINT32_MAX, _sp_bestpos = UINT32_MAX;                   \
                        double _sp_bestd = _sp_tau_in, _sp_2ndd = 1e30;                               \
                        for (size_t _sp_pi = 0; _sp_pi < n_polygon_paths; _sp_pi++) {                 \
                            poly_t* _sp_p = &polys[_sp_pi];                                            \
                            if (_sp_p->plane_id != _sp_B || _sp_p->n < 3) continue;                   \
                            for (uint32_t _sp_e = 0; _sp_e < _sp_p->n; _sp_e++) {                     \
                                uint32_t _sp_uid = _sp_p->ids[_sp_e];                                 \
                                uint32_t _sp_vid = _sp_p->ids[(_sp_e + 1) % _sp_p->n];                \
                                uint32_t _sp_u = id_lk(&corner_map, _sp_uid);                         \
                                uint32_t _sp_v = id_lk(&corner_map, _sp_vid);                         \
                                if (_sp_u == UINT32_MAX || _sp_v == UINT32_MAX) continue;             \
                                /* cA must not already be an endpoint here */                         \
                                if (_sp_uid == corners_in[_sp_cand].id ||                              \
                                    _sp_vid == corners_in[_sp_cand].id) continue;                      \
                                bool _sp_uR = false, _sp_vR = false;                                  \
                                for (uint32_t _k = 0; _k < cs[_sp_u].n_ridge_ids; _k++)               \
                                    if (cs[_sp_u].ridge_ids[_k] == _sp_R) { _sp_uR = true; break; }   \
                                for (uint32_t _k = 0; _k < cs[_sp_v].n_ridge_ids; _k++)               \
                                    if (cs[_sp_v].ridge_ids[_k] == _sp_R) { _sp_vR = true; break; }   \
                                if (!_sp_uR || !_sp_vR) continue;                                     \
                                /* arc-length bracket on the ridge line */                            \
                                double _sp_tu, _sp_tv, _sp_pp;                                         \
                                ridge_proj(cs[_sp_u].point, rg[_sp_R].rp, rg[_sp_R].rd, &_sp_tu, &_sp_pp); \
                                ridge_proj(cs[_sp_v].point, rg[_sp_R].rp, rg[_sp_R].rd, &_sp_tv, &_sp_pp); \
                                double _sp_lo = _sp_tu < _sp_tv ? _sp_tu : _sp_tv;                    \
                                double _sp_hi = _sp_tu < _sp_tv ? _sp_tv : _sp_tu;                    \
                                if (_sp_tA < _sp_lo || _sp_tA > _sp_hi) continue;                     \
                                /* (a) PERP distance from cA to the ACTUAL segment u→v  */            \
                                /*     (clamped), not just the infinite ridge line.     */            \
                                double _sp_d = pt_seg_dist_3d(cs[_sp_cand].point,                      \
                                                              cs[_sp_u].point, cs[_sp_v].point);      \
                                if (_sp_d > _sp_tau_in) continue;                                      \
                                if (_sp_d < _sp_bestd) {                                              \
                                    _sp_2ndd = _sp_bestd;                                             \
                                    _sp_bestd = _sp_d;                                                 \
                                    _sp_bestpi = (uint32_t)_sp_pi;                                     \
                                    _sp_bestpos = (_sp_e + 1) % _sp_p->n;                             \
                                } else if (_sp_d < _sp_2ndd) {                                        \
                                    _sp_2ndd = _sp_d;                                                 \
                                }                                                                     \
                            }                                                                         \
                        }                                                                             \
                        if (_sp_bestpi == UINT32_MAX) continue;   /* no both-R edge brackets cA */     \
                        /* (b) folded-ridge tie: two both-R B edges within ε → REFUSE  */             \
                        /*     (don't guess which arm cA belongs to).                  */             \
                        if (_sp_2ndd - _sp_bestd < 1e-4) { sp_tie_refuse++;                           \
                            if (br4_trace) fprintf(stderr, "[sp-refuse] corner %u ridge %u: "         \
                                "folded-ridge tie (d1=%.4f d2=%.4f) -> REFUSE\n",                     \
                                corners_in[_sp_cand].id, ridges[_sp_R].id, _sp_bestd, _sp_2ndd);       \
                            continue; }                                                               \
                        poly_t* _sp_Bp = &polys[_sp_bestpi];                                          \
                        /* (M4(b)) junction guard. cA is SINGLE-plane (_sp_np==1) by    */             \
                        /*    construction, so inserting it into ONE B edge makes it     */             \
                        /*    exactly {A,B}=2 planes → DOF-1 (a clean ridge corner) after */            \
                        /*    RETAG; it is NEVER a ≥3-plane junction needing a meet-solve */            \
                        /*    here. cA's pre-split DOF is irrelevant (a FREE DOF-2 single-*/            \
                        /*    plane corner that has slid onto R is the COMMON target —    */            \
                        /*    the FREE→RIDGE transition the design wants). The explicit   */            \
                        /*    T8 junction-REFUSE only triggers in the (here-impossible)   */            \
                        /*    ≥3-plane case, kept as a defensive guard + counter.         */            \
                        if (_sp_np >= 3) { sp_junction_refuse++;                                        \
                            if (br4_trace) fprintf(stderr, "[sp-refuse] corner %u: ≥3-plane "          \
                                "junction (np=%u) -> needs meet-solve (M4(b), T8) -> REFUSE\n",        \
                                corners_in[_sp_cand].id, _sp_np);                                       \
                            continue; }                                                               \
                        /* (M5) self-int reject in B's UV with cA INSERTED at pos. */                 \
                        if (split_would_self_intersect(_sp_Bp->ids, _sp_Bp->n,                        \
                                corners_in[_sp_cand].id, _sp_bestpos, cs[_sp_cand].point,               \
                                &corner_map, cs, &pc[_sp_Bidx].basis,                                 \
                                mc_id_scratch, mc_u_scratch, mc_v_scratch)) {                         \
                            sp_selfint_reject++;                                                       \
                            if (br4_trace) fprintf(stderr, "[sp-reject] corner %u ridge %u in B=%u: " \
                                "self-int -> NOT split\n", corners_in[_sp_cand].id,                    \
                                ridges[_sp_R].id, _sp_B);                                             \
                            continue;                                                                 \
                        }                                                                             \
                        /* ── PREPARE reserves (fallible; touch NO canonical P3 store) ── */           \
                        if (_br_test_reserve_fail                                                     \
                                || !poly_reserve(_sp_Bp, _sp_Bp->n + 1)                                \
                                || !ctx_reserve(&ctxs, &cap_ctx, total_ctx + 1)) {                    \
                            sp_oom_reject++;                                                          \
                            if (br4_trace) fprintf(stderr, "[sp-reject] corner %u: reserve OOM "      \
                                "-> clean REJECT (nothing mutated)\n", corners_in[_sp_cand].id);       \
                            continue;                                                                 \
                        }                                                                             \
                        /* Maintain the on-ridge tag (P4): cA IS geometrically on R (the */            \
                        /* split's precondition) → additively tag it so T6 / ridges_      */           \
                        /* incident_to / the DOF coupling see a consistent tag. One-way,  */           \
                        /* idempotent; fallible only on OOM (a maintained side-store, not */           \
                        /* canonical P3 — abort here leaves paths untouched + a correct    */           \
                        /* tag). Done before COMMIT so COMMIT stays allocation-free.       */          \
                        if (!ridge_ids_push_unique(&cs[_sp_cand].ridge_ids,                            \
                                &cs[_sp_cand].n_ridge_ids, _sp_R)) { rc = -1; goto r1_traj_oom; }      \
                        /* ── COMMIT (allocation-free, infallible) ── */                              \
                        for (uint32_t _m = _sp_Bp->n; _m > _sp_bestpos; _m--)                         \
                            _sp_Bp->ids[_m] = _sp_Bp->ids[_m - 1];                                    \
                        _sp_Bp->ids[_sp_bestpos] = corners_in[_sp_cand].id;                            \
                        _sp_Bp->n++;                                                                  \
                        _sp_Bp->changed = true;                                                       \
                        total_ctx++;   /* pool pre-reserved; keeps it correctly sized */              \
                        RETAG_DOF(_sp_cand);   /* now {A,B} → DOF-1 (keeps it consistent) */            \
                        REBUILD_CTXS();                                                               \
                        REBUILD_PW_EDGES();                                                           \
                        joint_dirty = true;                                                           \
                        out->stats.n_split_share++;   /* T7: the REAL split count */                  \
                        if (br4_trace) fprintf(stderr, "[split_share] corner %u ridge %u (rid=%u) "   \
                            "inserted into B=%u at pos %u (d=%.4f) iter=%d -> shared {A=%u,B=%u} "     \
                            "planes_of_n=%u dof=%u\n", corners_in[_sp_cand].id, _sp_R,                 \
                            ridges[_sp_R].id, _sp_B, _sp_bestpos, _sp_bestd, iter, _sp_A, _sp_B,      \
                            planes_of_n(_sp_cand, corners_in, polys, n_polygon_paths),                 \
                            cs[_sp_cand].dof);                                                         \
                        (_sp_fired) = true;                                                           \
                    }                                                                                 \
                }                                                                                     \
              }                                                                                       \
            }                                                                                         \
        } while (0)

    // ── CHASE_AFTER_MOVE: the post-move "chase" (R.2 proximity-merge +
    //    R.3 collinear-removal + R.4 ridge-merge), run from the corner
    //    `_start_ci` that just moved. Loops until a full pass changes no
    //    topology. Factored out of the R.1 acceptance loop so BOTH accept
    //    paths (GREEDY_PER_CORNER and the GLOBAL_BEST post-sweep accept)
    //    share ONE implementation — previously each had a hand-inlined
    //    copy and an edit to the T0b/A2 hygiene (RECONCILE_PLANES_DOWN +
    //    RETAG_DOF after R.2/R.3) landed on the GREEDY copy only, leaving
    //    GLOBAL_BEST with a stale-plane bug. This body is the correct
    //    (GREEDY) version: it carries the reconcile/retag AND the BR
    //    profiling. On OOM jumps to r1_traj_oom (in scope inside R.1).
    #define CHASE_AFTER_MOVE(_start_ci)                                                             \
        do {                                                                                       \
            uint32_t chase_ci = (uint32_t)(_start_ci);                                              \
            while (cs[chase_ci].alive) {                                                            \
                bool topo_changed = false;                                                          \
                /* ── R.2 proximity merge ──────────────────────── */                              \
                uint64_t br_t_R2_start = 0; if (br_prof_on) BR_NOW(br_t_R2_start);                  \
                if (params->use_proximity_merge) {                                                  \
                    const double MERGE_D2 = params->proximity_merge_dist_m                          \
                                            * params->proximity_merge_dist_m;                       \
                    uint32_t best_other = UINT32_MAX;                                               \
                    double best_d2 = MERGE_D2;                                                      \
                    for (size_t oci = 0; oci < n_corners; oci++) {                                  \
                        if (oci == chase_ci || !cs[oci].alive) continue;                            \
                        /* Share a plane? (path-derived membership) */                              \
                        if (!corners_share_plane(chase_ci, oci, corners_in, polys, n_polygon_paths)) \
                            continue;                                                               \
                        double dx = cs[chase_ci].point[0] - cs[oci].point[0];                       \
                        double dy = cs[chase_ci].point[1] - cs[oci].point[1];                       \
                        double dz = cs[chase_ci].point[2] - cs[oci].point[2];                       \
                        double d2 = dx*dx + dy*dy + dz*dz;                                           \
                        if (d2 < best_d2) { best_d2 = d2; best_other = (uint32_t)oci; }              \
                    }                                                                               \
                    if (best_other != UINT32_MAX) {                                                 \
                        /* Score = (dof, -nplanes, id) — lower wins. */                             \
                        const uint8_t  da = cs[chase_ci].dof,    db = cs[best_other].dof;           \
                        const uint32_t pa = planes_of_n(chase_ci, corners_in, polys, n_polygon_paths), \
                                       pb = planes_of_n(best_other, corners_in, polys, n_polygon_paths); \
                        const uint32_t ia = corners_in[chase_ci].id, ib = corners_in[best_other].id;\
                        bool a_wins = (da < db) || (da == db && pa > pb)                            \
                                            || (da == db && pa == pb && ia < ib);                   \
                        uint32_t surv = a_wins ? chase_ci : best_other;                             \
                        uint32_t lose = a_wins ? best_other : chase_ci;                             \
                        /* Membership is path-derived (planes_of) — the rewrite below is */         \
                        /* the only update needed (no stored plane set to union).        */         \
                        /* Rewrite polys: loser id → survivor id, dedup consecutive (wrap-aware). */\
                        uint32_t loser_id = corners_in[lose].id;                                    \
                        uint32_t surv_id  = corners_in[surv].id;                                    \
                        for (size_t pi = 0; pi < n_polygon_paths; pi++) {                           \
                            poly_t* p = &polys[pi];                                                  \
                            bool touched = false;                                                    \
                            for (uint32_t i = 0; i < p->n; i++) {                                    \
                                if (p->ids[i] == loser_id) { p->ids[i] = surv_id; touched = true; }  \
                            }                                                                       \
                            if (!touched) continue;                                                 \
                            uint32_t w = 0;                                                          \
                            for (uint32_t r2 = 0; r2 < p->n; r2++) {                                 \
                                if (w == 0 || p->ids[w-1] != p->ids[r2]) p->ids[w++] = p->ids[r2];   \
                            }                                                                       \
                            if (w >= 2 && p->ids[0] == p->ids[w-1]) w--;                             \
                            p->n = (w >= 3) ? w : 0;                                                 \
                            p->changed = true;                                                       \
                        }                                                                           \
                        cs[lose].alive = false;                                                      \
                        chase_ci = surv;                                                             \
                        out->stats.r2_merged_count++;                                                \
                        out->stats.n_merge_corner++;  /* T0: MERGE_CORNER (same-plane, R.2) */       \
                        /* Membership path-derived — RETAG_DOF reads the fresh paths. */            \
                        RETAG_DOF(surv);                                                             \
                        REBUILD_CTXS();                                                              \
                        REBUILD_PW_EDGES();                                                          \
                        joint_dirty = true;                                                          \
                        topo_changed = true;                                                         \
                    }                                                                               \
                }                                                                                   \
                BR_ACCUM(br_p_R2, br_t_R2_start);                                                    \
                /* ── MERGE_CORNER (cross-plane weld, T5) — ABOVE R.3 to honor */                    \
                /*    the MERGE→…→DROP cascade priority (§4.1): the tag/angle   */                    \
                /*    drops must not eat a corner before it gets a chance to    */                    \
                /*    weld. Replaces the legacy R.4; still A/B-gated by BR_NO_R4.*/                   \
                if (!_br_no_r4) {                                                                   \
                    bool mc_fired; MERGE_CORNER_XPLANE(chase_ci, mc_fired);                         \
                    if (mc_fired) topo_changed = true;                                              \
                }                                                                                   \
                /* ── SPLIT_TO_SHARE_CORNER (T7) — the GROWING make-shared op,  */                  \
                /*    AFTER MERGE_CORNER and BEFORE T6/DROP_COLINEAR (§4.1       */                  \
                /*    priority): a single-plane on-R corner with NO cross-plane  */                  \
                /*    partner (so MERGE didn't fire) but lying ON a both-R B     */                  \
                /*    path-edge is inserted into that edge → becomes shared. The  */                 \
                /*    op SCANS all candidate corners (a cond-A candidate is       */                 \
                /*    typically reached by ANOTHER corner's move/snap-in, not by  */                 \
                /*    its own), like MERGE_CORNER_XPLANE. Gated by BR_NO_SPLIT     */                 \
                /*    (default ON). On a split the corner becomes {A,B} shared     */                 \
                /*    (it did NOT move) → topo_changed re-queues the chase so T6   */                 \
                /*    can sweep any freshly-redundant interior next pass.          */                 \
                {                                                                                   \
                    bool sp_fired; SPLIT_TO_SHARE_CORNER(sp_fired);                                 \
                    if (sp_fired) topo_changed = true;                                              \
                }                                                                                   \
                /* ── T6 DROP_REDUNDANT_ON_RIDGE (all-polys-agree) — AFTER     */                   \
                /*    MERGE, BEFORE DROP_COLINEAR (the MERGE→T6→DROP cascade    */                   \
                /*    priority, §4.1). Reacts to the move: a corner that just   */                   \
                /*    snapped onto a ridge / had a shared edge form can become  */                   \
                /*    a redundant on-ridge midpoint in EVERY containing plane.  */                   \
                /*    Same helper as the pre-loop batch (one impl). Capture     */                   \
                /*    chase_ci's neighbours BEFORE the drop so we can repoint +  */                   \
                /*    RETAG them after (chase_ci itself is killed on a drop).    */                  \
                if (cs[chase_ci].alive && corner_ctx_n[chase_ci] > 0) {                              \
                    uint32_t _t6_cx  = corner_ctx_indices[chase_ci][0];                              \
                    uint32_t _t6_prev = ctxs[_t6_cx].prev_idx;                                       \
                    uint32_t _t6_next = ctxs[_t6_cx].next_idx;                                       \
                    if (try_drop_redundant_on_ridge(chase_ci, corners_in, polys,                     \
                            n_polygon_paths, &corner_map, cs)) {                                     \
                        out->stats.redundant_ridge_removed++;                                        \
                        out->stats.n_drop_ridge++;   /* T6: DROP_REDUNDANT_ON_RIDGE (in-loop) */     \
                        /* chase_ci was killed by the drop → repoint to a surviving */               \
                        /* neighbour (mirror R.3's chase_ci==to_remove guard).      */               \
                        if (cs[_t6_prev].alive)      chase_ci = _t6_prev;                            \
                        else if (cs[_t6_next].alive) chase_ci = _t6_next;                            \
                        /* Membership path-derived — RETAG_DOF reads the fresh paths. */             \
                        if (cs[_t6_prev].alive) RETAG_DOF(_t6_prev);                                 \
                        if (cs[_t6_next].alive) RETAG_DOF(_t6_next);                                 \
                        REBUILD_CTXS();                                                              \
                        REBUILD_PW_EDGES();                                                          \
                        joint_dirty = true;                                                          \
                        topo_changed = true;                                                         \
                    }                                                                               \
                }                                                                                   \
                /* ── R.3 collinear-removal ────────────────────── */                              \
                uint64_t br_t_R3_start = 0; if (br_prof_on) BR_NOW(br_t_R3_start);                  \
                if (params->use_collinear_removal) {                                                \
                    /* Scan all alive single-plane corners (dof=2) with    */                       \
                    /* exactly one polygon context; drop if angle within   */                       \
                    /* threshold of 180°. Find ONE per chase iter, then    */                       \
                    /* re-rebuild + loop.                                   */                       \
                    uint32_t to_remove = UINT32_MAX;                                                 \
                    for (size_t oci = 0; oci < n_corners; oci++) {                                  \
                        if (!cs[oci].alive) continue;                                               \
                        if (planes_of_n(oci, corners_in, polys, n_polygon_paths) != 1) continue;    \
                        if (corner_ctx_n[oci] != 1) continue;                                       \
                        uint32_t cx_idx = corner_ctx_indices[oci][0];                               \
                        const corner_context_t* cx = &ctxs[cx_idx];                                 \
                        double a = corner_angle_rad(cs[oci].point,                                  \
                                                      cs[cx->prev_idx].point,                        \
                                                      cs[cx->next_idx].point);                       \
                        double a_deg = a * 180.0 / M_PI;                                            \
                        if (a_deg >= 180.0 - params->collinear_removal_threshold_deg) {             \
                            to_remove = (uint32_t)oci;                                              \
                            break;                                                                  \
                        }                                                                           \
                    }                                                                               \
                    if (to_remove != UINT32_MAX) {                                                  \
                        /* Redirect to prev neighbour. */                                           \
                        uint32_t cx_idx = corner_ctx_indices[to_remove][0];                         \
                        uint32_t prev_ci = ctxs[cx_idx].prev_idx;                                   \
                        uint32_t lose_id = corners_in[to_remove].id;                                \
                        uint32_t surv_id = corners_in[prev_ci].id;                                  \
                        /* Membership is path-derived (planes_of) — the id-rewrite below */         \
                        /* is the only update (no stored plane set to union into prev).   */         \
                        for (size_t pi = 0; pi < n_polygon_paths; pi++) {                           \
                            poly_t* p = &polys[pi];                                                  \
                            bool touched = false;                                                    \
                            for (uint32_t i = 0; i < p->n; i++) {                                    \
                                if (p->ids[i] == lose_id) { p->ids[i] = surv_id; touched = true; }   \
                            }                                                                       \
                            if (!touched) continue;                                                 \
                            uint32_t w = 0;                                                          \
                            for (uint32_t r2 = 0; r2 < p->n; r2++) {                                 \
                                if (w == 0 || p->ids[w-1] != p->ids[r2]) p->ids[w++] = p->ids[r2];   \
                            }                                                                       \
                            if (w >= 2 && p->ids[0] == p->ids[w-1]) w--;                             \
                            p->n = (w >= 3) ? w : 0;                                                 \
                            p->changed = true;                                                       \
                        }                                                                           \
                        cs[to_remove].alive = false;                                                 \
                        /* If chase_ci was the removed corner (impossible    */                      \
                        /* since it just moved with non-zero cost), guard:   */                      \
                        if (chase_ci == to_remove) chase_ci = prev_ci;                              \
                        out->stats.r3_collinear_removed++;                                           \
                        out->stats.n_drop_colinear++;  /* T0: DROP_COLINEAR (R.3) */                 \
                        /* Membership path-derived — RETAG_DOF reads the fresh paths. */            \
                        RETAG_DOF(prev_ci);                                                          \
                        REBUILD_CTXS();                                                              \
                        REBUILD_PW_EDGES();                                                          \
                        joint_dirty = true;                                                          \
                        topo_changed = true;                                                         \
                    }                                                                               \
                }                                                                                   \
                BR_ACCUM(br_p_R3, br_t_R3_start);                                                    \
                /* (MERGE_CORNER moved ABOVE R.3 — see the cascade-priority block) */               \
                /* ── T4 SNAP-IN (FREE→RIDGE tag) — in the chase, reacts to the  */                \
                /* move. For chase_ci: ridges_incident_to → additively tag any   */                \
                /* NEWLY reached allow-listed on-R ridge (one-way, idempotent) + */                \
                /* RETAG_DOF (min-DOF-couples → DOF-1 + rg.rd axis, no freeze).   */                \
                /* EFFECT gated by BR_SNAP_IN so OFF is byte-identical to HEAD.   */                \
                /* The read-only M1 guard PREVIEW is a SEPARATE post-loop census  */                \
                /* (distinct on-R single-plane corners), not run in this hot loop.*/                \
                if (br_snap_in && rg && ridge_inc_buf && cs[chase_ci].alive) {                      \
                    uint64_t br_t_MS_start = 0; if (br_prof_on) BR_NOW(br_t_MS_start);             \
                    uint32_t _si_pids[PLANES_OF_SCRATCH];                                           \
                    uint32_t _si_np = planes_of(chase_ci, corners_in, polys, n_polygon_paths,       \
                                                _si_pids, PLANES_OF_SCRATCH);                        \
                    int _ni = ridges_incident_to(chase_ci, rg, n_ridges,                            \
                            cs[chase_ci].ridge_ids, cs[chase_ci].n_ridge_ids,                       \
                            cs[chase_ci].point, params->ridge_merge_coplanar_m,                     \
                            _si_pids, _si_np,                                                        \
                            &plane_map, ridx_off, ridx, ridge_inc_buf);                             \
                    bool _si_snapped = false;                                                       \
                    for (int _ii = 0; _ii < _ni; _ii++) {                                          \
                        uint32_t _R = ridge_inc_buf[_ii];                                           \
                        bool _tagged = false;                                                       \
                        for (uint32_t _tk = 0; _tk < cs[chase_ci].n_ridge_ids; _tk++)               \
                            if (cs[chase_ci].ridge_ids[_tk] == _R) { _tagged = true; break; }       \
                        if (_tagged) continue;  /* NEWLY reached only */                            \
                        if (!ridge_ids_push_unique(&cs[chase_ci].ridge_ids,                          \
                                &cs[chase_ci].n_ridge_ids, _R)) { rc = -1; goto r1_traj_oom; }      \
                        RETAG_DOF(chase_ci);                                                         \
                        ms_snap_in_count++;                                                          \
                        topo_changed = true;                                                         \
                        _si_snapped = true;                                                          \
                        if (br4_trace)                                                               \
                            fprintf(stderr, "[ms-snap] corner %u snapped onto ridge idx=%u "         \
                                "(rid=%u type=%u) iter=%d -> dof=%u\n",                              \
                                corners_in[chase_ci].id, _R, ridges[_R].id, rg[_R].type,             \
                                iter, cs[chase_ci].dof);                                             \
                    }                                                                                \
                    /* Position-snap (design §2): the tag+DOF above only CONSTRAIN  */               \
                    /* motion (DOF-1 slides PARALLEL) — they never PUT the corner ON */              \
                    /* the crease. PROJECT onto the single binding ridge when DOF-1  */              \
                    /* (reuse ridge_dof_for_corner for the bind ridge + ridge_proj   */              \
                    /* for the foot, clamped to the clipped segment). DOF-0 junctions*/              \
                    /* are placed by merge/RI — left untouched. Moving the position  */              \
                    /* stales the ctx/pw caches → rebuild (as MERGE/SPLIT do).        */              \
                    if (br_snap_pos && _si_snapped && cs[chase_ci].dof == 1) {                       \
                        size_t _sb = (size_t)-1;                                                      \
                        ridge_dof_for_corner(rg, n_ridges, cs[chase_ci].ridge_ids,                    \
                                cs[chase_ci].n_ridge_ids, cs[chase_ci].point,                         \
                                params->ridge_merge_coplanar_m, &_sb);                                \
                        if (_sb != (size_t)-1) {                                                      \
                            double _st, _sp;                                                          \
                            ridge_proj(cs[chase_ci].point, rg[_sb].rp, rg[_sb].rd, &_st, &_sp);       \
                            if (_st < 0.0) _st = 0.0;                                                 \
                            else if (_st > rg[_sb].tmax) _st = rg[_sb].tmax;                          \
                            for (int _d = 0; _d < 3; _d++)                                            \
                                cs[chase_ci].point[_d] = rg[_sb].rp[_d] + _st * rg[_sb].rd[_d];        \
                            REBUILD_CTXS(); REBUILD_PW_EDGES(); joint_dirty = true;                   \
                        }                                                                            \
                    }                                                                                \
                    BR_ACCUM(br_p_MS, br_t_MS_start);                                                \
                }                                                                                    \
                if (!topo_changed) break;                                                           \
            }                                                                                       \
        } while (0)

    // ── ACCEPT_MOVE_AND_CHASE: commit `best_pt` for corner `_ci`, bump
    //    counters, record the trajectory step (realloc-grow as needed),
    //    and run CHASE_AFTER_MOVE. Factored from the GREEDY_PER_CORNER
    //    and GLOBAL_BEST accept paths which had the same 16-line body
    //    inline; tracked as task #72c (Family C self-dup). Same OOM
    //    jump label as CHASE_AFTER_MOVE — `r1_traj_oom` must be in
    //    scope at the call site.
    #define ACCEPT_MOVE_AND_CHASE(_ci, _best_pt)                                                    \
        do {                                                                                       \
            size_t _amac_ci = (size_t)(_ci);                                                       \
            cs[_amac_ci].point[0] = (_best_pt)[0];                                                 \
            cs[_amac_ci].point[1] = (_best_pt)[1];                                                 \
            cs[_amac_ci].point[2] = (_best_pt)[2];                                                 \
            cs[_amac_ci].moved = true;                                                             \
            any_move = true;                                                                       \
            total_moves++;                                                                         \
            if (traj[_amac_ci].n_steps == traj[_amac_ci].cap) {                                    \
                uint32_t _amac_nc = traj[_amac_ci].cap == 0 ? 4 : traj[_amac_ci].cap * 2;          \
                double* _amac_nb = (double*)safe_realloc_array(                                    \
                    traj[_amac_ci].steps, (size_t)_amac_nc * 3, sizeof(double));                   \
                if (!_amac_nb) { rc = -1; goto r1_traj_oom; }                                      \
                traj[_amac_ci].steps = _amac_nb;                                                   \
                traj[_amac_ci].cap = _amac_nc;                                                     \
            }                                                                                      \
            traj[_amac_ci].steps[3*traj[_amac_ci].n_steps + 0] = (_best_pt)[0];                    \
            traj[_amac_ci].steps[3*traj[_amac_ci].n_steps + 1] = (_best_pt)[1];                    \
            traj[_amac_ci].steps[3*traj[_amac_ci].n_steps + 2] = (_best_pt)[2];                    \
            traj[_amac_ci].n_steps++;                                                              \
            CHASE_AFTER_MOVE(_amac_ci);                                                            \
        } while (0)

    // Sample a random offset within a corner's DOF subspace. Per axis,
    // uniform in [-NUDGE_RANGE, +NUDGE_RANGE]. out[3] zeroed first.
    #define RANDOM_DOF_OFFSET(c_st, out)                                                            \
        do {                                                                                       \
            (out)[0] = (out)[1] = (out)[2] = 0.0;                                                  \
            if ((c_st).has_axis_a) {                                                                \
                double _s = (mulberry32_next(&mb_state) * 2.0 - 1.0) * NUDGE;                                     \
                (out)[0] += _s * (c_st).axis_a[0]; (out)[1] += _s * (c_st).axis_a[1]; (out)[2] += _s * (c_st).axis_a[2]; \
            }                                                                                       \
            if ((c_st).has_axis_b) {                                                                \
                double _s = (mulberry32_next(&mb_state) * 2.0 - 1.0) * NUDGE;                                     \
                (out)[0] += _s * (c_st).axis_b[0]; (out)[1] += _s * (c_st).axis_b[1]; (out)[2] += _s * (c_st).axis_b[2]; \
            }                                                                                       \
        } while (0)

    int32_t iter = 0;
    uint32_t total_moves = 0;
    while (iter < params->max_iterations) {
        iter++;
        bool any_move = false;

        // ── #25 in-loop insertion sweep ─────────────────────────────
        // Every 12 iters (up to 20 before the end), add ONE corner at each
        // under-covered plane's worst-fitting edge (misfit/length > threshold),
        // self-int-guarded + per-plane budget, then let the descent refine it
        // over the next iterations. Interleaving is required — the static sim
        // saturates + self-intersects when it stacks inserts without refinement.
        if (br_corner_insert && iter % 12 == 0 && iter + 20 <= params->max_iterations) {
            for (size_t _spi = 0; _spi < n_planes; _spi++) {
                if (!pc[_spi].have || plane_ins_cnt[_spi] >= 24) continue;
                size_t _spp = SIZE_MAX;
                for (size_t _q = 0; _q < n_polygon_paths; _q++)
                    if (polys[_q].plane_id == planes[_spi].id) { _spp = _q; break; }
                if (_spp == SIZE_MAX) continue;
                poly_t* _sp = &polys[_spp];
                if (_sp->n < 3 || _sp->n >= MAX_POLY_VERTS - 2) continue;
                bool _sok = true;
                for (uint32_t _k = 0; _k < _sp->n; _k++) {
                    uint32_t _ck = id_lk(&corner_map, _sp->ids[_k]);
                    if (_ck == UINT32_MAX) { _sok = false; break; }
                    project_uv(&pc[_spi].basis, cs[_ck].point[0], cs[_ck].point[1], cs[_ck].point[2], &polyU[_k], &polyV[_k]);
                }
                if (!_sok) continue;
                double _sm[MAX_POLY_VERTS]={0}, _scu[MAX_POLY_VERTS]={0}, _scv[MAX_POLY_VERTS]={0}, _scw[MAX_POLY_VERTS]={0};
                for (size_t _q = 0; _q < pc[_spi].n_owner; _q++) {
                    if (point_in_poly_uv(pc[_spi].own_u[_q], pc[_spi].own_v[_q], polyU, polyV, _sp->n)) continue;
                    double _w = pc[_spi].own_w ? pc[_spi].own_w[_q] : 1.0; if (_w <= 0) continue;
                    uint32_t _e; double _t;
                    double _d = pt_nearest_edge_uv(pc[_spi].own_u[_q], pc[_spi].own_v[_q], polyU, polyV, _sp->n, &_e, &_t);
                    _sm[_e]+=_w*_d; _scu[_e]+=_w*pc[_spi].own_u[_q]; _scv[_e]+=_w*pc[_spi].own_v[_q]; _scw[_e]+=_w;
                }
                int _sb=-1; double _sbpl=0,_sbu=0,_sbv=0;
                for (uint32_t _k=0;_k<_sp->n;_k++){
                    uint32_t _k1=(_k+1)%_sp->n; double _eu=polyU[_k1]-polyU[_k],_ev=polyV[_k1]-polyV[_k];
                    double _len=sqrt(_eu*_eu+_ev*_ev); if(_len<1e-6||_scw[_k]<=0)continue;
                    double _pl=_sm[_k]/_len; if(_pl>_sbpl){_sbpl=_pl;_sb=(int)_k;_sbu=_scu[_k]/_scw[_k];_sbv=_scv[_k]/_scw[_k];}
                }
                if (_sb < 0 || _sbpl < 5.0) continue;
                // self-int pre-check: would the ring with the new vertex self-intersect?
                double _su[MAX_POLY_VERTS+2], _sv[MAX_POLY_VERTS+2]; int _sn=0;
                for (uint32_t _k=0;_k<_sp->n;_k++){ _su[_sn]=polyU[_k];_sv[_sn]=polyV[_k];_sn++; if((int)_k==_sb){_su[_sn]=_sbu;_sv[_sn]=_sbv;_sn++;} }
                if (poly_self_int_uv(_su, _sv, _sn)) continue;
                INSERT_CORNER_AT(_spp, _spi, _sb, _sbu, _sbv);
                plane_ins_cnt[_spi]++;
                any_move = true;   // keep the loop alive to refine the new corner
            }
        }

        // Global-best tracking (used only in GLOBAL_BEST mode).
        double   gb_improvement = 0.0;
        uint32_t gb_corner_ci   = UINT32_MAX;
        double   gb_corner_pt[3] = {0, 0, 0};

        for (size_t ci = 0; ci < n_corners; ci++) {
            if (!cs[ci].alive) continue;
            if (cs[ci].dof == 0) continue;
            if (cs[ci].n_planes == 0) continue;
            // Same filter as the optimizable-set count above: skip
            // corners that don't appear in any polygon path. v2's
            // `cornerContext.has(c.id)` filter.
            if (corner_ctx_n[ci] == 0) continue;

            double cur_cost; TOTAL_COST(ci, cs[ci].point, cur_cost);
            double best_cost = cur_cost;
            double best_pt[3] = { cs[ci].point[0], cs[ci].point[1], cs[ci].point[2] };
            // Marginal-compactness gate reference: PER-PLANE current WSD & compactness
            // for each of this corner's planes (candidates vetoed against these).
            uint32_t _g_pl[32]; double _g_w[32], _g_c[32]; uint32_t _g_n = 0;
            if (br_compact_gate) { GATE_ADD_PLANES(ci); }

            // Cost probe: decompose cur_cost into its weighted terms for the
            // target corner, so the term that "dumbly" improves as it slides is
            // visible. Replicates TOTAL_COST term-by-term at the current point.
            if (br_cost_probe != UINT32_MAX && corners_in[ci].id == br_cost_probe) {
                double _wsd = 0, _own = 0, _foreign = 0;
                if (params->use_wsd_cost) {
                    double wsd_sum = 0;
                    for (uint32_t cci = 0; cci < corner_ctx_n[ci]; cci++) {
                        uint32_t plane_id = ctxs[corner_ctx_indices[ci][cci]].plane_id;
                        uint32_t pi_idx = id_lk(&plane_map, plane_id);
                        if (pi_idx == UINT32_MAX) continue;
                        bool seen = false;
                        for (uint32_t cck = 0; cck < cci; cck++)
                            if (ctxs[corner_ctx_indices[ci][cck]].plane_id == plane_id) { seen = true; break; }
                        if (seen) continue;
                        double _o, _f; WSD_EVAL_SPLIT(pi_idx, _o, _f);
                        // totals reflect the actual cost (normalized iff BR_WSD_NORM);
                        // the raw _o/_f are kept for the audit lines below.
                        double _nrm = (br_wsd_norm && pc[pi_idx].sum_w > 0.0) ? pc[pi_idx].sum_w : 1.0;
                        wsd_sum += (_o + _f) / _nrm; _own += _o / _nrm; _foreign += _f / _nrm;
                        if (iter == 1) {
                            double _sumw = 0; for (size_t _q=0;_q<pc[pi_idx].n_owner;_q++) _sumw += pc[pi_idx].own_w ? pc[pi_idx].own_w[_q] : 1.0;
                            double _raw = _o * params->lambda_wsd / params->wsd_sigma_m;  // as it enters cost
                            fprintf(stderr, "[costprobe.norm] plane=%u n_owner=%zu sum_w=%.2f  RAW_wsd=%.2f  per_count=%.4f  per_weight(mean_misfit_m)=%.4f\n",
                                plane_id, pc[pi_idx].n_owner, _sumw, _raw,
                                pc[pi_idx].n_owner ? _raw/(double)pc[pi_idx].n_owner : 0.0,
                                _sumw > 0 ? _raw/_sumw : 0.0);
                        }
                        // One-shot: histogram plane's uncovered owner points by edge-distance.
                        // polyU/polyV currently hold this plane's polygon (just built by SPLIT).
                        if (iter == 1) {
                            const poly_t* _pl = NULL; uint32_t _nrings = 0;
                            for (size_t _pp = 0; _pp < n_polygon_paths; _pp++)
                                if (polys[_pp].plane_id == plane_id) { if (!_pl) _pl = &polys[_pp]; _nrings++; }
                            fprintf(stderr, "[costprobe.rings] plane=%u n_rings=%u (WSD uses only the first)\n", plane_id, _nrings);
                            if (_pl && _pl->n >= 3) {
                                uint32_t nb[5] = {0,0,0,0,0}; double sb[5] = {0,0,0,0,0}; double dmax = 0;
                                for (size_t kk = 0; kk < pc[pi_idx].n_owner; kk++) {
                                    if (point_in_poly_uv(pc[pi_idx].own_u[kk], pc[pi_idx].own_v[kk], polyU, polyV, _pl->n)) continue;
                                    double w = pc[pi_idx].own_w ? pc[pi_idx].own_w[kk] : 1.0; if (w <= 0) continue;
                                    double d = pt_edge_dist_uv(pc[pi_idx].own_u[kk], pc[pi_idx].own_v[kk], polyU, polyV, _pl->n);
                                    int b = d < 1.0 ? 0 : d < 5.0 ? 1 : d < 20.0 ? 2 : d < 50.0 ? 3 : 4;
                                    nb[b]++; sb[b] += w * d; if (d > dmax) dmax = d;
                                }
                                fprintf(stderr, "[costprobe.hist] plane=%u dmax=%.1f  <1m:n=%u s=%.1f  1-5:n=%u s=%.1f  5-20:n=%u s=%.1f  20-50:n=%u s=%.1f  >50:n=%u s=%.1f\n",
                                    plane_id, dmax, nb[0],sb[0], nb[1],sb[1], nb[2],sb[2], nb[3],sb[3], nb[4],sb[4]);
                                // Polygon UV bbox vs owner-point UV bbox: too-small polygon vs over-assigned points.
                                double puL=1e30,puH=-1e30,pvL=1e30,pvH=-1e30;
                                for (uint32_t _k=0;_k<_pl->n;_k++){ if(polyU[_k]<puL)puL=polyU[_k]; if(polyU[_k]>puH)puH=polyU[_k]; if(polyV[_k]<pvL)pvL=polyV[_k]; if(polyV[_k]>pvH)pvH=polyV[_k]; }
                                double ouL=1e30,ouH=-1e30,ovL=1e30,ovH=-1e30;
                                for (size_t _k=0;_k<pc[pi_idx].n_owner;_k++){ double u=pc[pi_idx].own_u[_k],v=pc[pi_idx].own_v[_k]; if(u<ouL)ouL=u; if(u>ouH)ouH=u; if(v<ovL)ovL=v; if(v>ovH)ovH=v; }
                                fprintf(stderr, "[costprobe.bbox] plane=%u nverts=%u  POLY u[%.1f,%.1f] v[%.1f,%.1f] (%.1fx%.1f)  OWNERS u[%.1f,%.1f] v[%.1f,%.1f] (%.1fx%.1f)\n",
                                    plane_id, _pl->n, puL,puH,pvL,pvH,puH-puL,pvH-pvL, ouL,ouH,ovL,ovH,ouH-ouL,ovH-ovL);
                            }
                        }
                    }
                    _wsd = params->lambda_wsd * (wsd_sum / params->wsd_sigma_m);
                    _own *= params->lambda_wsd / params->wsd_sigma_m;
                    _foreign *= params->lambda_wsd / params->wsd_sigma_m;
                }
                double _ang = 0, _nang = 0, _horiz = 0;
                if (params->use_angle_cost)           { double ac;  ANGLE_COST(ci, cs[ci].point, ac);  _ang  = params->lambda_angle * ac; }
                if (params->use_neighbor_angle_cost)  { double nac; NEIGHBOR_ANGLE_COST(ci, cs[ci].point, nac); _nang = params->lambda_neighbor_angle * nac; }
                if (params->use_horizontal_edge_cost) { double hc;  HORIZ_EDGE_COST(ci, cs[ci].point, hc); _horiz = params->lambda_horizontal_edge * hc; }
                double _hcop = 0, _vcop = 0, _collin = 0;
                if (params->use_horiz_coplanar_cost) { double _s2 = params->horiz_coplanar_sigma_m*params->horiz_coplanar_sigma_m;
                    _hcop = params->lambda_horiz_coplanar * pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, corner_to_pw_idxs[ci], corner_to_pw_n[ci], incident_mark, pair_hcop_cost, 2.0*_s2, params->cost_shape); }
                if (params->use_vert_coplanar_cost) { double _s2 = params->vert_coplanar_sigma_m*params->vert_coplanar_sigma_m;
                    _vcop = params->lambda_vert_coplanar * pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, corner_to_pw_idxs[ci], corner_to_pw_n[ci], incident_mark, pair_vcop_cost, 2.0*_s2, params->cost_shape); }
                if (params->use_collinear_cost) { double _s2 = params->collinear_sigma_m*params->collinear_sigma_m;
                    _collin = params->lambda_collinear * pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw, corner_to_pw_idxs[ci], corner_to_pw_n[ci], incident_mark, pair_collinear_cost, 2.0*_s2, params->cost_shape); }
                double _t = 0;
                for (uint32_t _ti = 0; _ti < cs[ci].n_ridge_ids; _ti++) {
                    size_t _ri = cs[ci].ridge_ids[_ti];
                    if (_ri >= n_ridges || rg[_ri].tmax <= 0.0 || rg[_ri].type == RIDGE_TYPE_COPLANAR) continue;
                    double _d = rg[_ri].rd[0]*cs[ci].axis_a[0] + rg[_ri].rd[1]*cs[ci].axis_a[1] + rg[_ri].rd[2]*cs[ci].axis_a[2];
                    if (_d > 0.999 || _d < -0.999) { double _sp; ridge_proj(cs[ci].point, rg[_ri].rp, rg[_ri].rd, &_t, &_sp); break; }
                }
                fprintf(stderr, "[costprobe] iter=%d id=%u t=%.2f total=%.4f | wsd=%.4f (own_out=%.4f foreign_in=%.4f) ang=%.4f nang=%.4f horiz=%.4f hcop=%.4f vcop=%.4f collin=%.4f\n",
                    iter, corners_in[ci].id, _t, cur_cost, _wsd, _own, _foreign, _ang, _nang, _horiz, _hcop, _vcop, _collin);
            }

            // ── BR_PIN_TRACE: why doesn't this corner move toward its points? ──
            if (br_pin_trace != UINT32_MAX && corners_in[ci].id == br_pin_trace && iter == params->max_iterations) {
                // Worst-supported plane + nearest owner in it → 0.5 m inward candidate.
                uint32_t _wp = UINT32_MAX; double _wd = -1, _tou = 0, _tov = 0, _tcu = 0, _tcv = 0;
                for (uint32_t cci = 0; cci < corner_ctx_n[ci]; cci++) {
                    uint32_t pl = ctxs[corner_ctx_indices[ci][cci]].plane_id;
                    uint32_t pidx = id_lk(&plane_map, pl); if (pidx == UINT32_MAX || !pc[pidx].have) continue;
                    double _cu, _cv; project_uv(&pc[pidx].basis, cs[ci].point[0], cs[ci].point[1], cs[ci].point[2], &_cu, &_cv);
                    double _md = 1e30, _bu = _cu, _bv = _cv;
                    for (size_t q = 0; q < pc[pidx].n_owner; q++) {
                        double du = _cu - pc[pidx].own_u[q], dv = _cv - pc[pidx].own_v[q]; double d2 = du*du + dv*dv;
                        if (d2 < _md) { _md = d2; _bu = pc[pidx].own_u[q]; _bv = pc[pidx].own_v[q]; }
                    }
                    if (sqrt(_md) > _wd) { _wd = sqrt(_md); _wp = pidx; _tou = _bu; _tov = _bv; _tcu = _cu; _tcv = _cv; }
                }
                double cand[3] = { cs[ci].point[0], cs[ci].point[1], cs[ci].point[2] };
                if (_wp != UINT32_MAX && _wd > 1e-6) {
                    double _dl = sqrt((_tou-_tcu)*(_tou-_tcu)+(_tov-_tcv)*(_tov-_tcv));
                    double _stp = _dl > 0.5 ? 0.5 : _dl;
                    double _nu = _tcu + _stp*(_tou-_tcu)/_dl, _nv = _tcv + _stp*(_tov-_tcv)/_dl;
                    uv_to_3d(&pc[_wp].basis, _nu, _nv, cand);
                }
                // Decompose all terms at a given position (sets cs[ci], restores).
                #define _PIN_DECOMP(POS, W,A,NA,H,HC,VC,CO,SUP) do {                                 \
                    double _sv[3]={cs[ci].point[0],cs[ci].point[1],cs[ci].point[2]};                  \
                    cs[ci].point[0]=(POS)[0];cs[ci].point[1]=(POS)[1];cs[ci].point[2]=(POS)[2];        \
                    double _ws=0; for(uint32_t z=0;z<corner_ctx_n[ci];z++){uint32_t _plid=ctxs[corner_ctx_indices[ci][z]].plane_id;uint32_t px=id_lk(&plane_map,_plid);if(px==UINT32_MAX)continue;bool sn=false;for(uint32_t y=0;y<z;y++)if(ctxs[corner_ctx_indices[ci][y]].plane_id==_plid){sn=true;break;}if(sn)continue;double wp;WSD_EVAL(px,wp);_ws+=wp;} \
                    (W)=params->lambda_wsd*(_ws/params->wsd_sigma_m);                                  \
                    {double c;ANGLE_COST(ci,cs[ci].point,c);(A)=params->lambda_angle*c;}               \
                    {double c;NEIGHBOR_ANGLE_COST(ci,cs[ci].point,c);(NA)=params->lambda_neighbor_angle*c;} \
                    {double c;HORIZ_EDGE_COST(ci,cs[ci].point,c);(H)=params->lambda_horizontal_edge*c;} \
                    {double s2=params->horiz_coplanar_sigma_m*params->horiz_coplanar_sigma_m;(HC)=params->lambda_horiz_coplanar*pairwise_cost_for_corner(cs,pw_edges,(uint32_t)n_pw,corner_to_pw_idxs[ci],corner_to_pw_n[ci],incident_mark,pair_hcop_cost,2.0*s2,params->cost_shape);} \
                    {double s2=params->vert_coplanar_sigma_m*params->vert_coplanar_sigma_m;(VC)=params->lambda_vert_coplanar*pairwise_cost_for_corner(cs,pw_edges,(uint32_t)n_pw,corner_to_pw_idxs[ci],corner_to_pw_n[ci],incident_mark,pair_vcop_cost,2.0*s2,params->cost_shape);} \
                    {double s2=params->collinear_sigma_m*params->collinear_sigma_m;(CO)=params->lambda_collinear*pairwise_cost_for_corner(cs,pw_edges,(uint32_t)n_pw,corner_to_pw_idxs[ci],corner_to_pw_n[ci],incident_mark,pair_collinear_cost,2.0*s2,params->cost_shape);} \
                    (SUP)=0; for(uint32_t z=0;z<corner_ctx_n[ci];z++){uint32_t _plid=ctxs[corner_ctx_indices[ci][z]].plane_id;uint32_t px=id_lk(&plane_map,_plid);if(px==UINT32_MAX||!pc[px].have)continue;bool sn=false;for(uint32_t y=0;y<z;y++)if(ctxs[corner_ctx_indices[ci][y]].plane_id==_plid){sn=true;break;}if(sn)continue;double su,sv;project_uv(&pc[px].basis,cs[ci].point[0],cs[ci].point[1],cs[ci].point[2],&su,&sv);double md=1e30;for(size_t q=0;q<pc[px].n_owner;q++){double du=su-pc[px].own_u[q],dv=sv-pc[px].own_v[q];double d2=du*du+dv*dv;if(d2<md)md=d2;}double sd=sqrt(md);double ls=(_ls_env?atof(_ls_env):0.0);double tl=(_st_env?atof(_st_env):1.0);if(sd>tl)(SUP)+=ls*(sd-tl);} \
                    cs[ci].point[0]=_sv[0];cs[ci].point[1]=_sv[1];cs[ci].point[2]=_sv[2];              \
                } while(0)
                double W0,A0,N0,H0,C0,V0,L0,S0, W1,A1,N1,H1,C1,V1,L1,S1;
                _PIN_DECOMP(cs[ci].point, W0,A0,N0,H0,C0,V0,L0,S0);
                _PIN_DECOMP(cand,         W1,A1,N1,H1,C1,V1,L1,S1);
                double _T0=W0+A0+N0+H0+C0+V0+L0+S0, _T1=W1+A1+N1+H1+C1+V1+L1+S1;
                fprintf(stderr, "[pintrace] id=%u worst_support=%.2fm  inward step 0.5m toward nearest point\n", br_pin_trace, _wd);
                fprintf(stderr, "[pintrace]   term |   current |  inward   |   Δ(inward-cur)  (Δ>0 = RESISTS the move)\n");
                fprintf(stderr, "[pintrace]   wsd  | %9.4f | %9.4f | %+9.4f\n", W0, W1, W1-W0);
                fprintf(stderr, "[pintrace]   ang  | %9.4f | %9.4f | %+9.4f\n", A0, A1, A1-A0);
                fprintf(stderr, "[pintrace]   nang | %9.4f | %9.4f | %+9.4f\n", N0, N1, N1-N0);
                fprintf(stderr, "[pintrace]   horiz| %9.4f | %9.4f | %+9.4f\n", H0, H1, H1-H0);
                fprintf(stderr, "[pintrace]   hcop | %9.4f | %9.4f | %+9.4f\n", C0, C1, C1-C0);
                fprintf(stderr, "[pintrace]   vcop | %9.4f | %9.4f | %+9.4f\n", V0, V1, V1-V0);
                fprintf(stderr, "[pintrace]   coll | %9.4f | %9.4f | %+9.4f\n", L0, L1, L1-L0);
                fprintf(stderr, "[pintrace]   supp | %9.4f | %9.4f | %+9.4f\n", S0, S1, S1-S0);
                fprintf(stderr, "[pintrace]  TOTAL | %9.4f | %9.4f | %+9.4f  (%s)\n", _T0, _T1, _T1-_T0, _T1<_T0?"inward ACCEPTED":"inward REJECTED");
                #undef _PIN_DECOMP
            }

            // Candidate sampler. DOF=1: along axis_a only. DOF=2: along axis_a +
            // axis_b lines (NOT a 2D grid — see DOF=2 note below).
            // Factored to TRY_AXIS_LINE so the 3 axis-loops below share one
            // body (task #72c, Family C self-dup). Reads {ci, n_samp, STEP,
            // cs, best_cost, best_pt} from the enclosing scope.
            int n_samp = (int)floor(NUDGE / STEP);
            // (merge: main's TRY_AXIS_LINE refactor + EVAL_CAND (compactness gate)
            // + this branch's optional DOF-1 slide-clamp. _do_clamp is br_slide_clamp
            // for the DOF-1 axis, false for the DOF-2 axes.)
            #define TRY_AXIS_LINE(_axis_field, _do_clamp)                               \
                do {                                                                    \
                    for (int _tas = -n_samp; _tas <= n_samp; _tas++) {                  \
                        if (_tas == 0) continue;                                        \
                        double _off = _tas * STEP;                                      \
                        double _cand[3] = {                                             \
                            cs[ci].point[0] + _off * cs[ci]._axis_field[0],             \
                            cs[ci].point[1] + _off * cs[ci]._axis_field[1],             \
                            cs[ci].point[2] + _off * cs[ci]._axis_field[2],             \
                        };                                                              \
                        if (_do_clamp) clamp_dof1_slide(&cs[ci], rg, n_ridges, _cand);   \
                        EVAL_CAND(_cand);                                               \
                    }                                                                   \
                } while (0)
            if (cs[ci].dof == 1 && cs[ci].has_axis_a) {
                // DOF-1: slide along axis_a, bounded to the binding ridge segment
                // when br_slide_clamp is on (a candidate past an endpoint is pulled back).
                TRY_AXIS_LINE(axis_a, br_slide_clamp);
            } else if (cs[ci].dof == 2) {
                // v2 generates LINE candidates per axis (NOT a 2D grid):
                // `for (axis of freedomAxes) for (s ...)` →
                //   {axis_a, ±NUDGE_STEP·k for k∈[1,n_samp]}
                //   {axis_b, ±NUDGE_STEP·k for k∈[1,n_samp]}
                // Earlier v3 versions iterated an (n×n) grid — wrong
                // candidate set, found improvements v2 missed → caused
                // 2-4× more accepted moves per iter on dahouet iso.
                // See journal 2026-05-27-boundary-regularize-iso-baseline.
                if (cs[ci].has_axis_a) TRY_AXIS_LINE(axis_a, false);
                if (cs[ci].has_axis_b) TRY_AXIS_LINE(axis_b, false);
            }
            #undef TRY_AXIS_LINE
            if (best_cost < cur_cost - 1e-9) {
                // GLOBAL_BEST (default): track best across the iter,
                // accept ONCE after the sweep. GREEDY_PER_CORNER: also
                // track for telemetry, but accept now (below).
                double improvement = cur_cost - best_cost;
                if (improvement > gb_improvement) {
                    gb_improvement = improvement;
                    gb_corner_ci   = (uint32_t)ci;
                    gb_corner_pt[0] = best_pt[0];
                    gb_corner_pt[1] = best_pt[1];
                    gb_corner_pt[2] = best_pt[2];
                }
                if (params->r1_acceptance_strategy == 1) {
                    // GREEDY_PER_CORNER: accept per-corner best, chase, continue scanning.
                    ACCEPT_MOVE_AND_CHASE(ci, best_pt);
                }
            }
        }

        // GLOBAL_BEST post-sweep accept (single move per iter, v2 default).
        if (params->r1_acceptance_strategy != 1 && gb_corner_ci != UINT32_MAX) {
            size_t ci = gb_corner_ci;
            double best_pt[3] = { gb_corner_pt[0], gb_corner_pt[1], gb_corner_pt[2] };
            ACCEPT_MOVE_AND_CHASE(ci, best_pt);
        }

        // ── Joint two-corner sweep (end of iter) ────────────────────
        // For each (a, b) joint edge: sample joint_samples_per_edge random
        // (offset_a, offset_b) pairs within each corner's DOF subspace,
        // accept the best pair if it strictly reduces cost(a) + cost(b).
        //
        // Path B (deviates from v2's global-best-per-iter): joint moves
        // are an ADDENDUM to the per-corner sweep, not co-evaluated. Each
        // joint move is accepted greedily if it improves the local
        // (a, b) pair cost. Validation in 5.7g.5 will quantify the
        // semantic drift from v2's policy.
        if (params->use_joint_moves) {
            if (joint_dirty) REBUILD_JOINT_EDGES();
            for (size_t je = 0; je < n_joint_edges; je++) {
                uint32_t a_ci = joint_edges[je].a, b_ci = joint_edges[je].b;
                if (!cs[a_ci].alive || !cs[b_ci].alive) continue;
                if (cs[a_ci].dof == 0 || cs[b_ci].dof == 0) continue;
                double ca, cb; TOTAL_COST(a_ci, cs[a_ci].point, ca); TOTAL_COST(b_ci, cs[b_ci].point, cb);
                double base = ca + cb;
                double best_imp = 0;
                double best_pa[3] = { cs[a_ci].point[0], cs[a_ci].point[1], cs[a_ci].point[2] };
                double best_pb[3] = { cs[b_ci].point[0], cs[b_ci].point[1], cs[b_ci].point[2] };
                const double saved_a[3] = { cs[a_ci].point[0], cs[a_ci].point[1], cs[a_ci].point[2] };
                const double saved_b[3] = { cs[b_ci].point[0], cs[b_ci].point[1], cs[b_ci].point[2] };
                // Marginal-compactness gate reference over the UNION of both endpoints'
                // planes, captured at the saved (pre-move) state. The joint sweep is the
                // OTHER slide path — id=136 drifts here — so it needs the same veto.
                uint32_t _g_pl[32]; double _g_w[32], _g_c[32]; uint32_t _g_n = 0;
                if (br_compact_gate) { GATE_ADD_PLANES(a_ci); GATE_ADD_PLANES(b_ci); }
                for (int32_t js = 0; js < params->joint_samples_per_edge; js++) {
                    double da[3], db[3];
                    RANDOM_DOF_OFFSET(cs[a_ci], da);
                    RANDOM_DOF_OFFSET(cs[b_ci], db);
                    cs[a_ci].point[0] = saved_a[0] + da[0]; cs[a_ci].point[1] = saved_a[1] + da[1]; cs[a_ci].point[2] = saved_a[2] + da[2];
                    cs[b_ci].point[0] = saved_b[0] + db[0]; cs[b_ci].point[1] = saved_b[1] + db[1]; cs[b_ci].point[2] = saved_b[2] + db[2];
                    // Same DOF-1 segment bound as the single-corner sampler:
                    // the joint sweep is the OTHER slide path (RANDOM_DOF_OFFSET
                    // along axis_a/axis_b), so it must clamp too or a corner
                    // escapes the crease here instead. Gated BR_SLIDE_CLAMP.
                    if (br_slide_clamp) {
                        if (cs[a_ci].dof == 1) clamp_dof1_slide(&cs[a_ci], rg, n_ridges, cs[a_ci].point);
                        if (cs[b_ci].dof == 1) clamp_dof1_slide(&cs[b_ci], rg, n_ridges, cs[b_ci].point);
                    }
                    double pca, pcb;
                    TOTAL_COST(a_ci, cs[a_ci].point, pca);
                    TOTAL_COST(b_ci, cs[b_ci].point, pcb);
                    bool _j_veto = false;
                    if (br_compact_gate) GATE_VETO_NOW(_j_veto);  // both endpoints at candidate now
                    double imp = base - (pca + pcb);
                    if (!_j_veto && imp > best_imp) {
                        best_imp = imp;
                        memcpy(best_pa, cs[a_ci].point, 24);
                        memcpy(best_pb, cs[b_ci].point, 24);
                    }
                }
                cs[a_ci].point[0] = saved_a[0]; cs[a_ci].point[1] = saved_a[1]; cs[a_ci].point[2] = saved_a[2];
                cs[b_ci].point[0] = saved_b[0]; cs[b_ci].point[1] = saved_b[1]; cs[b_ci].point[2] = saved_b[2];
                if (best_imp > 1e-9) {
                    memcpy(cs[a_ci].point, best_pa, 24);
                    memcpy(cs[b_ci].point, best_pb, 24);
                    cs[a_ci].moved = true; cs[b_ci].moved = true;
                    any_move = true;
                    total_moves++;
                    out->stats.r1_joint_moves++;
                    // Trajectory record for both endpoints.
                    for (int side = 0; side < 2; side++) {
                        uint32_t ci_t = side == 0 ? a_ci : b_ci;
                        const double* pt_t = side == 0 ? best_pa : best_pb;
                        if (traj[ci_t].n_steps == traj[ci_t].cap) {
                            uint32_t nc = traj[ci_t].cap == 0 ? 4 : traj[ci_t].cap * 2;
                            double* nb = (double*)safe_realloc_array(traj[ci_t].steps, (size_t)nc * 3, sizeof(double));
                            if (!nb) { rc = -1; goto r1_joint_oom; }
                            traj[ci_t].steps = nb; traj[ci_t].cap = nc;
                        }
                        traj[ci_t].steps[3*traj[ci_t].n_steps + 0] = pt_t[0];
                        traj[ci_t].steps[3*traj[ci_t].n_steps + 1] = pt_t[1];
                        traj[ci_t].steps[3*traj[ci_t].n_steps + 2] = pt_t[2];
                        traj[ci_t].n_steps++;
                    }
                    // 5.7g.4 (implemented 2026-06-24): run the proximity-merge /
                    // collinear chase on BOTH joint-moved endpoints. Single-corner
                    // moves already chase (CHASE_AFTER_MOVE), but joint moves
                    // previously skipped it — so corners nudged close/coincident by a
                    // joint move (and there is no final merge sweep) were never merged
                    // (e.g. dahouet P4X5/P4C12 @3cm and the COINCIDENT RE33+RE71 /
                    // P6C12+P24CP6). Chase a, then b if still alive (a's chase may have
                    // merged b into a). If a chase changed topology, joint_edges[] is
                    // now stale (the chase set joint_dirty) — stop iterating this list;
                    // it rebuilds next iteration.
                    uint32_t _jm_topo0 = out->stats.n_merge_corner
                                       + out->stats.n_drop_ridge + out->stats.n_drop_colinear;
                    CHASE_AFTER_MOVE(a_ci);
                    if (cs[b_ci].alive) CHASE_AFTER_MOVE(b_ci);
                    if (out->stats.n_merge_corner + out->stats.n_drop_ridge
                            + out->stats.n_drop_colinear != _jm_topo0) {
                        joint_dirty = true;
                        break;
                    }
                }
            }
        }

        // ── Convergence trace ───────────────────────────────────────
        // Every iteration, sum TOTAL_COST over all alive optimisable
        // corners at their CURRENT (committed) positions. This gives a
        // single scalar per iter that you can plot against iter to see
        // the convergence curve. Cost components are whichever are
        // currently enabled — so cross-config curves are NOT directly
        // comparable, but within a single config the curve is the right
        // shape. Adds ~n_corners × cost_per_eval per iter (≈ same order
        // as a single corner's candidate scan, so <10% overhead).
        if (br_prof_on) {
            double iter_cost = 0.0;
            for (size_t ci = 0; ci < n_corners; ci++) {
                if (!cs[ci].alive) continue;
                if (cs[ci].dof == 0) continue;
                if (cs[ci].n_planes == 0) continue;
                if (corner_ctx_n[ci] == 0) continue;
                double c; TOTAL_COST(ci, cs[ci].point, c);
                iter_cost += c;
            }
            // Count this iter's accepted moves separately by comparing
            // total_moves to its value at iter start. We didn't capture
            // that — simplest is to log total_moves cumulative. The
            // delta between iters tells you moves-this-iter.
            fprintf(stderr, "[br-conv] iter=%d cost=%.9g cum_moves=%u\n",
                    iter, iter_cost, total_moves);

            // ── ALL-metric per-iter tracking ─────────────────────────
            // Evaluate every one of the 7 metrics at the current
            // (committed) corner positions, regardless of which were
            // enabled in the search. This lets us answer: "does metric
            // X get driven down by another metric Y, even when X is
            // disabled in the search?" If yes → X is redundant.
            // Values are RAW (no lambda) so they're independent of the
            // user-tunable weights.
            double am_wsd = 0, am_angle = 0, am_nbr = 0, am_he = 0;
            double am_hcop = 0, am_vcop = 0, am_collin = 0;
            double am_hcop_sigsq = 2.0 * params->horiz_coplanar_sigma_m * params->horiz_coplanar_sigma_m;
            double am_vcop_sigsq = 2.0 * params->vert_coplanar_sigma_m  * params->vert_coplanar_sigma_m;
            double am_col_sigsq  = 2.0 * params->collinear_sigma_m       * params->collinear_sigma_m;
            for (size_t ci = 0; ci < n_corners; ci++) {
                if (!cs[ci].alive)         continue;
                if (cs[ci].dof == 0)        continue;
                if (cs[ci].n_planes == 0)   continue;
                if (corner_ctx_n[ci] == 0)  continue;
                // WSD — sum unique-plane WSD contributions for this corner
                double w_sum = 0;
                for (uint32_t cci = 0; cci < corner_ctx_n[ci]; cci++) {
                    uint32_t plane_id = ctxs[corner_ctx_indices[ci][cci]].plane_id;
                    uint32_t pi_idx = id_lk(&plane_map, plane_id);
                    if (pi_idx == UINT32_MAX) continue;
                    bool seen = false;
                    for (uint32_t cck = 0; cck < cci; cck++) {
                        if (ctxs[corner_ctx_indices[ci][cck]].plane_id == plane_id) { seen = true; break; }
                    }
                    if (seen) continue;
                    double wsd_p; WSD_EVAL(pi_idx, wsd_p);
                    w_sum += (br_wsd_norm && pc[pi_idx].sum_w > 0.0)
                               ? wsd_p / pc[pi_idx].sum_w : wsd_p;
                }
                am_wsd += w_sum / params->wsd_sigma_m;
                // Single-corner cost metrics
                double ac, nac, hec;
                ANGLE_COST(ci, cs[ci].point, ac);            am_angle += ac;
                NEIGHBOR_ANGLE_COST(ci, cs[ci].point, nac);  am_nbr   += nac;
                HORIZ_EDGE_COST(ci, cs[ci].point, hec);      am_he    += hec;
                // Pairwise (R.7) metrics
                am_hcop   += pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw,
                    corner_to_pw_idxs[ci], corner_to_pw_n[ci],
                    incident_mark, pair_hcop_cost, am_hcop_sigsq, params->cost_shape);
                am_vcop   += pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw,
                    corner_to_pw_idxs[ci], corner_to_pw_n[ci],
                    incident_mark, pair_vcop_cost, am_vcop_sigsq, params->cost_shape);
                am_collin += pairwise_cost_for_corner(cs, pw_edges, (uint32_t)n_pw,
                    corner_to_pw_idxs[ci], corner_to_pw_n[ci],
                    incident_mark, pair_collinear_cost, am_col_sigsq, params->cost_shape);
            }
            fprintf(stderr,
                "[br-allmetrics] iter=%d wsd=%.9g angle=%.9g nbr_angle=%.9g "
                "horiz_edge=%.9g hcop=%.9g vcop=%.9g collin=%.9g\n",
                iter, am_wsd, am_angle, am_nbr, am_he, am_hcop, am_vcop, am_collin);
            // Per-plane WSD (own_out+foreign_in) this iter — which planes
            // converge (WSD→0) vs stay stuck (curriculum test). Only planes with
            // a non-trivial residual are listed.
            {
                char _pb[1200]; int _pl = 0; _pb[0] = '\0';
                for (size_t _pi = 0; _pi < n_planes && _pl < 1100; _pi++) {
                    if (!pc[_pi].have) continue;
                    double _o, _f; WSD_EVAL_SPLIT(_pi, _o, _f);
                    if (_o + _f < 0.1) continue;
                    _pl += snprintf(_pb + _pl, sizeof(_pb) - (size_t)_pl,
                                    "%s%u:%.1f", _pl ? " " : "", planes[_pi].id, _o + _f);
                }
                fprintf(stderr, "[planewsd] iter=%d %s\n", iter, _pb);
            }
            // Per-plane compactness + WSD (marginal-compactness analysis): does a
            // plane's compactness WORSEN while its WSD (own_out+foreign_in) stays
            // flat? = "drift for nothing" a marginal-compactness guard would block.
            if (iter == 1 || iter % 10 == 0 || iter == params->max_iterations) {
                for (size_t _pi = 0; _pi < n_planes; _pi++) {
                    if (!pc[_pi].have) continue;
                    const poly_t* _cp = NULL;
                    for (size_t _pp = 0; _pp < n_polygon_paths; _pp++)
                        if (polys[_pp].plane_id == planes[_pi].id) { _cp = &polys[_pp]; break; }
                    if (!_cp || _cp->n < 3 || _cp->n > MAX_POLY_VERTS) continue;
                    bool _ok = true;
                    for (uint32_t _k = 0; _k < _cp->n; _k++) {
                        uint32_t _ck = id_lk(&corner_map, _cp->ids[_k]);
                        if (_ck == UINT32_MAX) { _ok = false; break; }
                        project_uv(&pc[_pi].basis, cs[_ck].point[0], cs[_ck].point[1], cs[_ck].point[2], &polyU[_k], &polyV[_k]);
                    }
                    if (!_ok) continue;
                    double _a2 = 0, _pm = 0;
                    for (uint32_t _k = 0; _k < _cp->n; _k++) { uint32_t _k1 = (_k+1)%_cp->n;
                        _a2 += polyU[_k]*polyV[_k1] - polyU[_k1]*polyV[_k];
                        double _du=polyU[_k1]-polyU[_k],_dv=polyV[_k1]-polyV[_k]; _pm += sqrt(_du*_du+_dv*_dv); }
                    double _ar = fabs(_a2)*0.5;
                    double _cmp = _ar > 1e-9 ? (_pm*_pm)/(4.0*M_PI*_ar) : INFINITY;
                    double _o, _f; WSD_EVAL_SPLIT(_pi, _o, _f);
                    fprintf(stderr, "[ptrace] iter=%d plane=%u own_out=%.2f foreign_in=%.2f compact=%.2f area=%.1f nverts=%u\n",
                        iter, planes[_pi].id, _o, _f, _cmp, _ar, _cp->n);
                }
            }
        }
        if (!any_move) break;
    }
    BR_ACCUM(br_p_R1_total, br_t_R1_start);
    if (br_prof_on) {
        struct timespec _ts = {0};  // see BR_NOW macro for the Frama-C rationale
        clock_gettime(CLOCK_MONOTONIC_RAW, &_ts);
        uint64_t br_wall_end = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
        uint64_t wall_ns = br_wall_end - br_wall_start;
        uint64_t cpu_total = br_p_R0.ns + br_p_R0b.ns + br_p_R1_total.ns;
        uint64_t r1_non_score = br_p_R1_total.ns > br_p_R1_score.ns ? br_p_R1_total.ns - br_p_R1_score.ns : 0;
        uint64_t r1_non_score_min_chase = r1_non_score > (br_p_R2.ns + br_p_R3.ns) ? r1_non_score - br_p_R2.ns - br_p_R3.ns : 0;
        #define BR_PCT(part, whole) ((whole) ? (100.0 * (double)(part) / (double)(whole)) : 0.0)
        fprintf(stderr, "\n[br-profile] ──────────── boundary-regularize timing (CPU time via CLOCK_THREAD_CPUTIME_ID) ────────────\n");
        fprintf(stderr, "[br-profile] wall total:      %8.2f ms\n", wall_ns / 1.0e6);
        fprintf(stderr, "[br-profile] cpu total (sum): %8.2f ms (%.1f%% of wall)\n", cpu_total / 1.0e6, BR_PCT(cpu_total, wall_ns));
        fprintf(stderr, "[br-profile] ──── phases ──────────────────────────────────────────────────\n");
        fprintf(stderr, "[br-profile] R.0   DOF tagging:      %8.2f ms (%6.2f%%) %8" PRIu64 " calls\n",
                br_p_R0.ns / 1.0e6,   BR_PCT(br_p_R0.ns,    cpu_total), br_p_R0.calls);
        fprintf(stderr, "[br-profile] R.0b  polygon cleanup:  %8.2f ms (%6.2f%%) %8" PRIu64 " calls\n",
                br_p_R0b.ns / 1.0e6,  BR_PCT(br_p_R0b.ns,   cpu_total), br_p_R0b.calls);
        fprintf(stderr, "[br-profile] R.1   coord descent:    %8.2f ms (%6.2f%%) %8" PRIu64 " iter\n",
                br_p_R1_total.ns / 1.0e6, BR_PCT(br_p_R1_total.ns, cpu_total), (uint64_t)iter);
        fprintf(stderr, "[br-profile] ──── inside R.1 (score vs proposal vs chase) ───────────────\n");
        fprintf(stderr, "[br-profile]   score (sum TOTAL_COST):       %8.2f ms (%6.2f%% of R.1) %10" PRIu64 " calls\n",
                br_p_R1_score.ns / 1.0e6, BR_PCT(br_p_R1_score.ns, br_p_R1_total.ns), br_p_R1_score.calls);
        fprintf(stderr, "[br-profile]   R.2 proximity merge chase:    %8.2f ms (%6.2f%% of R.1) %10" PRIu64 " calls\n",
                br_p_R2.ns / 1.0e6, BR_PCT(br_p_R2.ns, br_p_R1_total.ns), br_p_R2.calls);
        fprintf(stderr, "[br-profile]   R.3 collinear-removal chase:  %8.2f ms (%6.2f%% of R.1) %10" PRIu64 " calls\n",
                br_p_R3.ns / 1.0e6, BR_PCT(br_p_R3.ns, br_p_R1_total.ns), br_p_R3.calls);
        fprintf(stderr, "[br-profile]   make-shared (SHARE/SPLIT):    %8.2f ms (%6.2f%% of R.1) %10" PRIu64 " calls  [T0 scaffold]\n",
                br_p_MS.ns / 1.0e6, BR_PCT(br_p_MS.ns, br_p_R1_total.ns), br_p_MS.calls);
        fprintf(stderr, "[br-profile]   proposal/overhead (R.1 − score − R.2 − R.3): %8.2f ms (%6.2f%% of R.1)\n",
                r1_non_score_min_chase / 1.0e6, BR_PCT(r1_non_score_min_chase, br_p_R1_total.ns));
        fprintf(stderr, "[br-profile] ──── per-metric (within score time) ────────────────────────\n");
        uint64_t metric_sum = br_m_wsd.ns + br_m_angle.ns + br_m_nbr_angle.ns + br_m_horiz_edge.ns + br_m_hcop.ns + br_m_vcop.ns + br_m_collin.ns;
        fprintf(stderr, "[br-profile]   WSD:                          %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_wsd.ns / 1.0e6, BR_PCT(br_m_wsd.ns, br_p_R1_score.ns), br_m_wsd.calls);
        fprintf(stderr, "[br-profile]   angle:                        %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_angle.ns / 1.0e6, BR_PCT(br_m_angle.ns, br_p_R1_score.ns), br_m_angle.calls);
        fprintf(stderr, "[br-profile]   neighbor-angle:               %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_nbr_angle.ns / 1.0e6, BR_PCT(br_m_nbr_angle.ns, br_p_R1_score.ns), br_m_nbr_angle.calls);
        fprintf(stderr, "[br-profile]   horizontal-edge:              %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_horiz_edge.ns / 1.0e6, BR_PCT(br_m_horiz_edge.ns, br_p_R1_score.ns), br_m_horiz_edge.calls);
        fprintf(stderr, "[br-profile]   horiz-coplanar (R.7):         %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_hcop.ns / 1.0e6, BR_PCT(br_m_hcop.ns, br_p_R1_score.ns), br_m_hcop.calls);
        fprintf(stderr, "[br-profile]   vert-coplanar (R.7):          %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_vcop.ns / 1.0e6, BR_PCT(br_m_vcop.ns, br_p_R1_score.ns), br_m_vcop.calls);
        fprintf(stderr, "[br-profile]   collinear (R.7):              %8.2f ms (%6.2f%% of score) %10" PRIu64 " calls\n",
                br_m_collin.ns / 1.0e6, BR_PCT(br_m_collin.ns, br_p_R1_score.ns), br_m_collin.calls);
        fprintf(stderr, "[br-profile]   (sum of metrics %.2f ms = %.2f%% of score — gap = TOTAL_COST overhead)\n",
                metric_sum / 1.0e6, BR_PCT(metric_sum, br_p_R1_score.ns));
        fprintf(stderr, "[br-profile] ────────────────────────────────────────────────────────────────\n");
        #undef BR_PCT
    }
    out->stats.r1_iterations  = (uint32_t)iter;
    out->stats.r1_total_moves = total_moves;
    // Recompute optimizable count post-R.1/R.2/R.3 (matches v2's final
    // `optimizableCorners.length` semantic — re-filtered each iter as
    // R.2/R.3 dropped corners).
    out->stats.r1_corners_optimisable = 0;
    for (size_t ci = 0; ci < n_corners; ci++) {
        if (cs[ci].alive && cs[ci].dof > 0 && corner_ctx_n[ci] > 0) {
            out->stats.r1_corners_optimisable++;
        }
    }

    // R.4 final shared-corner count: alive corners with >=2 planes (path-derived).
    out->stats.shared_corners = 0;
    for (size_t ci = 0; ci < n_corners; ci++) {
        if (cs[ci].alive && planes_of_n(ci, corners_in, polys, n_polygon_paths) >= 2)
            out->stats.shared_corners++;
    }

    // ── T4 read-only M1 share-guard PREVIEW census (no mutation) ──────
    // A final-state preview of the make-shared yield (T5/T7): for each ALIVE
    // SINGLE-PLANE corner on an allow-listed ridge R, test the two-condition
    // guard (§9). cond (B): a B corner within τ_partner of cA in 3D carrying R
    // (cross-plane, not already sharing a plane) → would MERGE_CORNER. else
    // cond (A): cA falls inside a B path-edge whose BOTH consecutive endpoints
    // carry ridge_id==R → would SPLIT_TO_SHARE_CORNER. Tallies DISTINCT corners
    // (a corner counts once, B taking priority over A). Confirms make-shared
    // will fire when T5/T7 land. ridge_inc_buf reused as scratch.
    uint32_t ms_cand_on_r = 0;   // single-plane corners on an allow-listed ridge (SHARE candidates)
    if (rg && ridge_inc_buf) {
        const double _tp2 = params->ridge_merge_dist_m * params->ridge_merge_dist_m;
        for (size_t cA = 0; cA < n_corners; cA++) {
            if (!cs[cA].alive) continue;
            uint32_t _A_pids[PLANES_OF_SCRATCH];
            uint32_t _A_np = planes_of(cA, corners_in, polys, n_polygon_paths,
                                       _A_pids, PLANES_OF_SCRATCH);
            if (_A_np != 1) continue;
            uint32_t _Acorner = _A_pids[0];
            int _ni = ridges_incident_to(cA, rg, n_ridges,
                    cs[cA].ridge_ids, cs[cA].n_ridge_ids, cs[cA].point,
                    params->ridge_merge_coplanar_m, _A_pids, _A_np,
                    &plane_map, ridx_off, ridx, ridge_inc_buf);
            if (_ni > 0) ms_cand_on_r++;
            bool corner_B = false, corner_A = false;
            for (int _ii = 0; _ii < _ni; _ii++) {
                uint32_t _R = ridge_inc_buf[_ii];
                uint32_t _B = (rg[_R].plane_a == _Acorner) ? rg[_R].plane_b
                            : (rg[_R].plane_b == _Acorner) ? rg[_R].plane_a
                            : UINT32_MAX;
                if (_B == UINT32_MAX) continue;
                // cond (B) — proximity weld (mirrors R.4's geometric scan; §9:
                // "no tag needed — cA is ON R, so within-τ_partner of cA ⟹ cB is
                // within-τ_partner of R ⟹ on the crease"). So we test cB's
                // GEOMETRIC on-R-ness (perp ≤ τ_in, t in segment), NOT a tag —
                // tags are sparse (seed + the 2 snap-ins) and would under-count.
                bool _condB = false;
                const double _tau_in = params->ridge_merge_coplanar_m;
                for (size_t cBi = 0; cBi < n_corners && !_condB; cBi++) {
                    if (cBi == cA || !cs[cBi].alive) continue;
                    if (!corner_has_plane(cBi, _B, corners_in, polys, n_polygon_paths)) continue;
                    if (corners_share_plane(cA, cBi, corners_in, polys, n_polygon_paths)) continue;
                    double _tB, _perpB; ridge_proj(cs[cBi].point, rg[_R].rp, rg[_R].rd, &_tB, &_perpB);
                    if (_perpB > _tau_in) continue;
                    if (_tB < 0.0 || _tB > rg[_R].tmax) continue;
                    double _dx = cs[cA].point[0] - cs[cBi].point[0];
                    double _dy = cs[cA].point[1] - cs[cBi].point[1];
                    double _dz = cs[cA].point[2] - cs[cBi].point[2];
                    if (_dx*_dx + _dy*_dy + _dz*_dz <= _tp2) _condB = true;
                }
                if (_condB) { corner_B = true; continue; }
                // cond (A): both-R B path-edge bracketing cA's proj_t
                bool _condA = false;
                for (size_t _pi = 0; _pi < n_polygon_paths && !_condA; _pi++) {
                    poly_t* _p = &polys[_pi];
                    if (_p->plane_id != _B || _p->n < 3) continue;
                    for (uint32_t _e = 0; _e < _p->n && !_condA; _e++) {
                        uint32_t _u = id_lk(&corner_map, _p->ids[_e]);
                        uint32_t _v = id_lk(&corner_map, _p->ids[(_e + 1) % _p->n]);
                        if (_u == UINT32_MAX || _v == UINT32_MAX) continue;
                        bool _uR = false, _vR = false;
                        for (uint32_t _rk = 0; _rk < cs[_u].n_ridge_ids; _rk++)
                            if (cs[_u].ridge_ids[_rk] == _R) { _uR = true; break; }
                        for (uint32_t _rk = 0; _rk < cs[_v].n_ridge_ids; _rk++)
                            if (cs[_v].ridge_ids[_rk] == _R) { _vR = true; break; }
                        if (!_uR || !_vR) continue;
                        double _ta, _ptmp; ridge_proj(cs[cA].point, rg[_R].rp, rg[_R].rd, &_ta, &_ptmp);
                        double _tu;        ridge_proj(cs[_u].point, rg[_R].rp, rg[_R].rd, &_tu, &_ptmp);
                        double _tv;        ridge_proj(cs[_v].point, rg[_R].rp, rg[_R].rd, &_tv, &_ptmp);
                        double _lo = _tu < _tv ? _tu : _tv;
                        double _hi = _tu < _tv ? _tv : _tu;
                        if (_ta >= _lo && _ta <= _hi) _condA = true;
                    }
                }
                if (_condA) corner_A = true;
            }
            if (corner_B) { ms_would_share_B++; out->stats.n_would_merge++; }
            // T7 (2026-06-19): n_split_share is now incremented by the REAL
            // SPLIT_TO_SHARE_CORNER op in the loop (no double-count). This
            // post-loop census is a PREVIEW only: it tallies the residual
            // single-plane on-R corners that STILL bracket a both-R B edge at
            // the final state (candidates the loop refused or didn't reach) into
            // the local ms_would_share_A for the [ms-t4] trace — it does NOT
            // touch out->stats.n_split_share anymore.
            else if (corner_A) { ms_would_share_A++; }
        }
    }

    if (br4_trace) {
        fprintf(stderr,
            "[br4-summary] ridge_merges=%u ridges_with_merge=%u/%zu "
            "shared_corners(n_planes>=2)=%u r2_merges=%u r3_removed=%u\n",
            out->stats.xplane_merged, out->stats.ridges_with_xplane_merge,
            n_ridges, out->stats.shared_corners,
            out->stats.r2_merged_count, out->stats.r3_collinear_removed);
        fprintf(stderr,
            "[br-ops] merge_corner=%u split_share=%u drop_ridge=%u drop_colinear=%u "
            "(xplane_merges=%u self_int_rejects=%u)\n",
            out->stats.n_merge_corner, out->stats.n_split_share,
            out->stats.n_drop_ridge, out->stats.n_drop_colinear,
            out->stats.xplane_merged, mc_selfint_reject);
        // T7 SPLIT decomposition: committed (= n_split_share) vs the REJECT/REFUSE
        // tallies (self-int M5, folded-ridge tie, junction M4(b)→T8, reserve OOM).
        fprintf(stderr,
            "[sp-ops] split_share=%u  rejects(self_int=%u tie=%u junction=%u oom=%u) "
            "(split %s)\n",
            out->stats.n_split_share, sp_selfint_reject, sp_tie_refuse,
            sp_junction_refuse, sp_oom_reject, _br_no_split ? "OFF" : "ON");
        // T4: snap-in count + read-only make-shared PREVIEW (would-share) tallies.
        fprintf(stderr,
            "[ms-t4] snap_in=%u  candidates(single-plane on-R)=%u  would_share "
            "condB(merge)=%u condA(split)=%u total=%u (snap_in %s%s)\n",
            ms_snap_in_count, ms_cand_on_r, ms_would_share_B, ms_would_share_A,
            ms_would_share_B + ms_would_share_A,
            br_snap_in ? "ON" : "OFF", _br_no_r4 ? ", R4 OFF" : "");
    }


    // L1: final orphan sweep — a corner left alive but in no path by any in-loop
    // collapse (MERGE dedup / R.3 / T6 splice that zeroed a ring) must not be
    // emitted alive with stale data. Mid-loop these are inert (path-derived
    // membership skips them everywhere); this is the one place it matters.
    kill_orphaned_corners(cs, n_corners, corners_in, polys, n_polygon_paths);

    // Diagnostic (BR_DUMP_COVERAGE=1): per-plane FINAL polygon coverage — how many
    // owner points end up OUTSIDE the (optimized) polygon, their weighted distance,
    // and polygon-vs-owners UV bbox. For the "which planes degenerate" investigation.
    if (getenv("BR_DUMP_COVERAGE")) {
        for (size_t _pi = 0; _pi < n_planes; _pi++) {
            if (!pc[_pi].have) continue;
            const poly_t* _pl = NULL;
            for (size_t _pp = 0; _pp < n_polygon_paths; _pp++)
                if (polys[_pp].plane_id == planes[_pi].id) { _pl = &polys[_pp]; break; }
            if (!_pl || _pl->n < 3 || _pl->n > MAX_POLY_VERTS) continue;
            bool _ok = true;
            for (uint32_t _k = 0; _k < _pl->n; _k++) {
                uint32_t _ck = id_lk(&corner_map, _pl->ids[_k]);
                if (_ck == UINT32_MAX) { _ok = false; break; }
                project_uv(&pc[_pi].basis, cs[_ck].point[0], cs[_ck].point[1], cs[_ck].point[2], &polyU[_k], &polyV[_k]);
            }
            if (!_ok) continue;
            uint32_t _n_out = 0; double _sum_out = 0, _dmax = 0, _sumw = 0;
            for (size_t _q = 0; _q < pc[_pi].n_owner; _q++) {
                double _w = pc[_pi].own_w ? pc[_pi].own_w[_q] : 1.0; _sumw += _w;
                if (point_in_poly_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], polyU, polyV, _pl->n)) continue;
                if (_w <= 0) continue;
                double _d = pt_edge_dist_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], polyU, polyV, _pl->n);
                _n_out++; _sum_out += _w * _d; if (_d > _dmax) _dmax = _d;
            }
            double puL=1e30,puH=-1e30,pvL=1e30,pvH=-1e30,ouL=1e30,ouH=-1e30,ovL=1e30,ovH=-1e30;
            for (uint32_t _k=0;_k<_pl->n;_k++){ if(polyU[_k]<puL)puL=polyU[_k]; if(polyU[_k]>puH)puH=polyU[_k]; if(polyV[_k]<pvL)pvL=polyV[_k]; if(polyV[_k]>pvH)pvH=polyV[_k]; }
            for (size_t _q=0;_q<pc[_pi].n_owner;_q++){ double u=pc[_pi].own_u[_q],v=pc[_pi].own_v[_q]; if(u<ouL)ouL=u; if(u>ouH)ouH=u; if(v<ovL)ovL=v; if(v>ovH)ovH=v; }
            // compactness = perim²/(4π·area): shoelace area + edge-length perimeter (scale-invariant)
            double _area2 = 0, _perim = 0;
            for (uint32_t _k=0;_k<_pl->n;_k++){ uint32_t _k1=(_k+1)%_pl->n;
                _area2 += polyU[_k]*polyV[_k1] - polyU[_k1]*polyV[_k];
                double _du=polyU[_k1]-polyU[_k],_dv=polyV[_k1]-polyV[_k]; _perim += sqrt(_du*_du+_dv*_dv); }
            double _area = fabs(_area2)*0.5;
            double _comp = _area > 1e-9 ? (_perim*_perim)/(4.0*M_PI*_area) : INFINITY;
            // Per-vertex SUPPORT distance: for each polygon vertex, distance (UV) to
            // the nearest owner point. The MAX over vertices flags an unsupported
            // vertex (one sitting in empty space) — the vertex-support signal that
            // compactness is blind to. Reports the max + which corner id.
            double _maxsup = 0; uint32_t _maxsup_id = 0;
            for (uint32_t _k = 0; _k < _pl->n; _k++) {
                double _mind = 1e30;
                for (size_t _q = 0; _q < pc[_pi].n_owner; _q++) {
                    double _du = polyU[_k] - pc[_pi].own_u[_q], _dv = polyV[_k] - pc[_pi].own_v[_q];
                    double _d2 = _du*_du + _dv*_dv; if (_d2 < _mind) _mind = _d2;
                }
                double _d = sqrt(_mind);
                if (_d > _maxsup) { _maxsup = _d; _maxsup_id = _pl->ids[_k]; }
            }
            fprintf(stderr, "[cov] plane=%u nverts=%u n_owner=%zu Sw=%.1f n_out=%u(%.0f%%) own_out=%.1f mean_misfit=%.3f dmax=%.1f POLY(%.1fx%.1f) OWNERS(%.1fx%.1f) COMPACT=%.2f perim=%.1f area=%.1f MAXSUP=%.2f@id%u\n",
                planes[_pi].id, _pl->n, pc[_pi].n_owner, _sumw, _n_out,
                pc[_pi].n_owner ? 100.0*_n_out/(double)pc[_pi].n_owner : 0.0,
                _sum_out, _sumw>0?_sum_out/_sumw:0.0, _dmax,
                puH-puL, pvH-pvL, ouH-ouL, ovH-ovL, _comp, _perim, _area, _maxsup, _maxsup_id);
        }
    }

    // Diagnostic (BR_DUMP_EDGEMISFIT=1): the #25 per-edge fit-misfit metric. For
    // each plane polygon edge, the SIGNED misfit = Σ(owner OUTSIDE charged to
    // this edge: +w·d) − Σ(foreign INSIDE charged here: −w·d). A large POSITIVE
    // edge is slicing through its own cloud (→ insert a vertex + bow OUT);
    // negative = over-reaching foreign territory (→ bow IN). mass_uv = the
    // weighted centroid of the outside points charged to the edge = the natural
    // insertion target. This is the measurement that validates the metric before
    // wiring the insertion op.
    if (getenv("BR_DUMP_EDGEMISFIT")) {
        for (size_t _pi = 0; _pi < n_planes; _pi++) {
            if (!pc[_pi].have) continue;
            const poly_t* _pl = NULL;
            for (size_t _pp = 0; _pp < n_polygon_paths; _pp++)
                if (polys[_pp].plane_id == planes[_pi].id) { _pl = &polys[_pp]; break; }
            if (!_pl || _pl->n < 3 || _pl->n > MAX_POLY_VERTS) continue;
            bool _ok = true;
            for (uint32_t _k = 0; _k < _pl->n; _k++) {
                uint32_t _ck = id_lk(&corner_map, _pl->ids[_k]);
                if (_ck == UINT32_MAX) { _ok = false; break; }
                project_uv(&pc[_pi].basis, cs[_ck].point[0], cs[_ck].point[1], cs[_ck].point[2], &polyU[_k], &polyV[_k]);
            }
            if (!_ok) continue;
            double _mis[MAX_POLY_VERTS] = {0}, _cu[MAX_POLY_VERTS] = {0}, _cv[MAX_POLY_VERTS] = {0}, _cw[MAX_POLY_VERTS] = {0};
            uint32_t _no[MAX_POLY_VERTS] = {0};
            for (size_t _q = 0; _q < pc[_pi].n_owner; _q++) {
                if (point_in_poly_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], polyU, polyV, _pl->n)) continue;
                double _w = pc[_pi].own_w ? pc[_pi].own_w[_q] : 1.0; if (_w <= 0) continue;
                uint32_t _e; double _t;
                double _d = pt_nearest_edge_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], polyU, polyV, _pl->n, &_e, &_t);
                _mis[_e] += _w*_d; _no[_e]++; _cu[_e] += _w*pc[_pi].own_u[_q]; _cv[_e] += _w*pc[_pi].own_v[_q]; _cw[_e] += _w;
            }
            if (!pc[_pi].is_wall) for (size_t _q = 0; _q < pc[_pi].n_nearby; _q++) {
                if (!point_in_poly_uv(pc[_pi].near_u[_q], pc[_pi].near_v[_q], polyU, polyV, _pl->n)) continue;
                double _w = pc[_pi].near_w ? pc[_pi].near_w[_q] : 1.0; if (_w <= 0) continue;
                uint32_t _e; double _t;
                double _d = pt_nearest_edge_uv(pc[_pi].near_u[_q], pc[_pi].near_v[_q], polyU, polyV, _pl->n, &_e, &_t);
                _mis[_e] -= _w*_d;
            }
            for (uint32_t _k = 0; _k < _pl->n; _k++) {
                if (_mis[_k] < 1.0 && _mis[_k] > -1.0) continue;
                uint32_t _k1 = (_k+1) % _pl->n;
                double _eu = polyU[_k1]-polyU[_k], _ev = polyV[_k1]-polyV[_k];
                double _len = sqrt(_eu*_eu + _ev*_ev);
                fprintf(stderr, "[emisfit] plane=%u edge=%u(c%u->c%u) len=%.2f misfit=%+.1f n_out=%u perlen=%+.1f mass_uv=%.2f,%.2f\n",
                    planes[_pi].id, _k, _pl->ids[_k], _pl->ids[_k1], _len, _mis[_k], _no[_k],
                    _len > 0 ? _mis[_k]/_len : 0.0,
                    _cw[_k] > 0 ? _cu[_k]/_cw[_k] : 0.0, _cw[_k] > 0 ? _cv[_k]/_cw[_k] : 0.0);
            }
        }
    }

    // Diagnostic (BR_SIM_INSERT=<plane_id>|all): READ-ONLY simulation of the #25
    // corner-insertion op on the FINAL polygon — proves the metric + mass-centroid
    // placement drive own_out down before we build the real (array-growing) op.
    // Works entirely in a local UV copy; emits nothing. Each step: measure per-edge
    // misfit on the working polygon, insert one vertex at the worst edge's uncovered
    // mass centroid, self-int guard, re-measure. Trace = own_out trajectory.
    if (getenv("BR_SIM_INSERT")) {
        const char* _siv = getenv("BR_SIM_INSERT");
        bool _all = (strcmp(_siv, "all") == 0);
        int _target = _all ? -1 : atoi(_siv);
        const double _PERLEN_THRESH = 5.0;   // insert where misfit-per-length exceeds this
        const int _MAX_INSERT = 30;
        for (size_t _pi = 0; _pi < n_planes; _pi++) {
            if (!pc[_pi].have) continue;
            if (!_all && (int)planes[_pi].id != _target) continue;
            const poly_t* _pl = NULL;
            for (size_t _pp = 0; _pp < n_polygon_paths; _pp++)
                if (polys[_pp].plane_id == planes[_pi].id) { _pl = &polys[_pp]; break; }
            if (!_pl || _pl->n < 3 || _pl->n > MAX_POLY_VERTS) continue;
            double lu[320], lv[320]; int ln = (int)_pl->n;
            bool _ok = true;
            for (int _k = 0; _k < ln; _k++) {
                uint32_t _ck = id_lk(&corner_map, _pl->ids[_k]);
                if (_ck == UINT32_MAX) { _ok = false; break; }
                project_uv(&pc[_pi].basis, cs[_ck].point[0], cs[_ck].point[1], cs[_ck].point[2], &lu[_k], &lv[_k]);
            }
            if (!_ok) continue;
            for (int _step = 0; _step <= _MAX_INSERT; _step++) {
                double mis[320] = {0}, cu[320] = {0}, cv[320] = {0}, cw[320] = {0};
                double own_out = 0; uint32_t nout = 0;
                for (size_t _q = 0; _q < pc[_pi].n_owner; _q++) {
                    if (point_in_poly_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], lu, lv, (uint32_t)ln)) continue;
                    double _w = pc[_pi].own_w ? pc[_pi].own_w[_q] : 1.0; if (_w <= 0) continue;
                    uint32_t _e; double _t;
                    double _d = pt_nearest_edge_uv(pc[_pi].own_u[_q], pc[_pi].own_v[_q], lu, lv, (uint32_t)ln, &_e, &_t);
                    own_out += _w*_d; nout++;
                    mis[_e] += _w*_d; cu[_e] += _w*pc[_pi].own_u[_q]; cv[_e] += _w*pc[_pi].own_v[_q]; cw[_e] += _w;
                }
                int best = -1; double best_pl = 0, best_mu = 0, best_mv = 0;
                for (int _k = 0; _k < ln; _k++) {
                    int _k1 = (_k+1) % ln; double eu = lu[_k1]-lu[_k], ev = lv[_k1]-lv[_k];
                    double len = sqrt(eu*eu + ev*ev);
                    if (len < 1e-6 || cw[_k] <= 0) continue;
                    double pl = mis[_k] / len;
                    if (pl > best_pl) { best_pl = pl; best = _k; best_mu = cu[_k]/cw[_k]; best_mv = cv[_k]/cw[_k]; }
                }
                fprintf(stderr, "[siminsert] plane=%u step=%d nverts=%d own_out=%.1f n_out=%u worst_edge=%d perlen=%.1f\n",
                    planes[_pi].id, _step, ln, own_out, nout, best, best_pl);
                if (best < 0 || best_pl < _PERLEN_THRESH || _step == _MAX_INSERT || ln >= 318) break;
                int at = best + 1;
                for (int _k = ln; _k > at; _k--) { lu[_k] = lu[_k-1]; lv[_k] = lv[_k-1]; }
                lu[at] = best_mu; lv[at] = best_mv; ln++;
                if (poly_self_int_uv(lu, lv, ln)) {
                    for (int _k = at; _k < ln-1; _k++) { lu[_k] = lu[_k+1]; lv[_k] = lv[_k+1]; } ln--;
                    fprintf(stderr, "[siminsert]   edge=%d insert at (%.2f,%.2f) SELF-INTERSECTS -> revert, stop\n",
                        best, best_mu, best_mv);
                    break;
                }
            }
        }
    }

    // Diagnostic (BR_DUMP_CM=1): per-corner final state + ridge geometry, for the
    // "why don't X and Y merge / why is X weird" corner investigations. Env-gated,
    // stderr-only, zero output change. Each [cdump] line carries the corner's
    // path-derived membership, ridge tags WITH their perpendicular distance + t
    // (so off-crease tagged corners are visible), DOF, and the upstream name.
    if (getenv("BR_DUMP_CM")) {
        for (size_t _ri = 0; _ri < n_ridges; _ri++)
            fprintf(stderr, "[rgdump] rid=%zu pa=%u pb=%u type=%u rp=%.4f,%.4f,%.4f rd=%.4f,%.4f,%.4f tmax=%.4f\n",
                _ri, rg[_ri].plane_a, rg[_ri].plane_b, rg[_ri].type,
                rg[_ri].rp[0], rg[_ri].rp[1], rg[_ri].rp[2], rg[_ri].rd[0], rg[_ri].rd[1], rg[_ri].rd[2], rg[_ri].tmax);
        uint32_t _dpids[PLANES_OF_SCRATCH];
        for (size_t _ci = 0; _ci < n_corners; _ci++) {
            if (!cs[_ci].alive) continue;
            uint32_t _np = planes_of(_ci, corners_in, polys, n_polygon_paths, _dpids, PLANES_OF_SCRATCH);
            char _pbuf[256]; int _pl = 0;
            _pbuf[0] = '\0';
            for (uint32_t _k = 0; _k < _np && _pl < (int)sizeof(_pbuf) - 1; _k++)
                _pl += snprintf(_pbuf+_pl, sizeof(_pbuf)-(size_t)_pl, "%s%u", _k?",":"", _dpids[_k]);
            char _rbuf[512]; int _rl = 0;
            _rbuf[0] = '\0';
            for (uint32_t _k = 0; _k < cs[_ci].n_ridge_ids && _rl < (int)sizeof(_rbuf) - 1; _k++) {
                uint32_t _R = cs[_ci].ridge_ids[_k];
                double _t = 0, _pp = -1;
                if (_R < n_ridges) ridge_proj(cs[_ci].point, rg[_R].rp, rg[_R].rd, &_t, &_pp);
                _rl += snprintf(_rbuf+_rl, sizeof(_rbuf)-(size_t)_rl, "%s%u(t%u perp%.0fcm t=%.2f/%.2f)",
                    _k?",":"", _R, _R<n_ridges?rg[_R].type:0, _pp*100, _t, _R<n_ridges?rg[_R].tmax:0.0);
            }
            fprintf(stderr, "[cdump] id=%u dof=%u axa=%d axb=%d pos=%.4f,%.4f,%.4f planes_of=[%s] ridges=[%s] name=%s\n",
                corners_in[_ci].id, cs[_ci].dof, cs[_ci].has_axis_a, cs[_ci].has_axis_b,
                cs[_ci].point[0], cs[_ci].point[1], cs[_ci].point[2],
                _pbuf, _rbuf, corners_in[_ci].name ? corners_in[_ci].name : "(null)");
            // Outlier trajectory: a corner that ran far from the building — dump every
            // accepted move so we can see whether it slid gradually or jumped.
            if (cs[_ci].point[0] < -30 || cs[_ci].point[0] > 30 ||
                cs[_ci].point[2] < -30 || cs[_ci].point[2] > 30) {
                fprintf(stderr, "[ctraj] id=%u nsteps=%u name=%s\n", corners_in[_ci].id,
                    traj[_ci].n_steps, corners_in[_ci].name ? corners_in[_ci].name : "(null)");
                for (uint32_t _s = 0; _s < traj[_ci].n_steps; _s++)
                    fprintf(stderr, "[ctraj]   %u: %.3f,%.3f,%.3f\n", _s,
                        traj[_ci].steps[3*_s+0], traj[_ci].steps[3*_s+1], traj[_ci].steps[3*_s+2]);
            }
        }
    }

    // Emit corner positions (parallel to corners_in).
    out->corner_positions = (v7_br_corner_pos_t*)safe_calloc_array(n_corners, sizeof(v7_br_corner_pos_t));
    if (!out->corner_positions) { rc = -1; goto r1_traj_oom; }
    for (size_t ci = 0; ci < n_corners; ci++) {
        out->corner_positions[ci].corner_id = corners_in[ci].id;
        memcpy(out->corner_positions[ci].point, cs[ci].point, 24);
    }
    out->n_corner_positions = n_corners;

    // Emit post-merge plane sets (parallel to corner_positions) so the
    // wrapper can re-emit CORNERS with grown plane_ids — this is how R.4's
    // unified ridges surface as shared (n_plane_ids>=2) corners downstream.
    out->corner_planes = (v7_br_corner_planes_t*)safe_calloc_array(n_corners, sizeof(v7_br_corner_planes_t));
    if (!out->corner_planes) { rc = -1; goto r1_traj_oom; }
    for (size_t ci = 0; ci < n_corners; ci++) {
        uint32_t _cp_pids[PLANES_OF_SCRATCH];
        uint32_t _cp_n = planes_of(ci, corners_in, polys, n_polygon_paths,
                                   _cp_pids, PLANES_OF_SCRATCH);
        out->corner_planes[ci].corner_id   = corners_in[ci].id;
        out->corner_planes[ci].n_plane_ids = _cp_n;
        if (_cp_n > 0) {
            out->corner_planes[ci].plane_ids =
                (uint32_t*)safe_malloc_array(_cp_n, sizeof(uint32_t));
            if (!out->corner_planes[ci].plane_ids) { rc = -1; goto r1_traj_oom; }
            memcpy(out->corner_planes[ci].plane_ids, _cp_pids,
                   _cp_n * sizeof(uint32_t));
        }
    }
    out->n_corner_planes = n_corners;

    // ── T0: emit polygon_paths from final polys[] (post-loop) ──────────
    // Re-emit EVERY plane's path from the end-of-loop polys[] so the emitted
    // graph reflects all in-loop R.2/R.3/R.4 mutations (was a pre-loop snapshot
    // that only captured R.0b → 39 dead-id refs on dahouet). We emit a record
    // for EVERY upstream path (even if it shrank to n<3) so the wrapper's
    // plane_id match always succeeds and an emptied path is NOT resurrected
    // from the stale upstream path; the wrapper drops n<3 records itself.
    // Each path is filtered to LIVE corner ids (cs[].alive) — a defensive
    // belt-and-braces over MERGE_CORNERS, which already keeps polys[] live.
    if (n_polygon_paths > 0) {
        out->polygon_paths = (v7_br_polygon_path_t*)safe_calloc_array(
            n_polygon_paths, sizeof(*out->polygon_paths));
        if (!out->polygon_paths) { rc = -1; goto r1_traj_oom; }
        for (size_t pi = 0; pi < n_polygon_paths; pi++) {
            poly_t* p = &polys[pi];
            out->polygon_paths[pi].plane_id = p->plane_id;
            uint32_t* dst = (uint32_t*)safe_malloc_array(p->n ? p->n : 1, sizeof(uint32_t));
            if (!dst) { rc = -1; goto r1_traj_oom; }
            uint32_t w = 0;
            for (uint32_t k = 0; k < p->n; k++) {
                uint32_t ci = id_lk(&corner_map, p->ids[k]);
                if (ci == UINT32_MAX || !cs[ci].alive) continue;  // drop dead ids
                dst[w++] = p->ids[k];
            }
            out->polygon_paths[pi].corner_ids = dst;
            out->polygon_paths[pi].n_corners  = w;
        }
        out->n_polygon_paths = n_polygon_paths;
    }

    // Stamp final alive bit onto each dof record now that R.0b + R.2 +
    // R.3 have all settled. Dropped corners stay in out->dof so the
    // record array remains parallel to corners_in, but consumers can
    // filter to alive=true for v2-equivalent comparisons.
    //
    // T4: also re-emit the final DOF class + freedom axes from the settled
    // cs[] (was pre-loop-only — the §9 output-emission gap). This makes the
    // diagnostic dof.json reflect in-loop RETAG_DOF + the T4 snap-in (FREE→
    // RIDGE). out->dof is DIAGNOSTIC-only (NOT a roof-graph section; the
    // wrapper reads only .alive — :459), so this does NOT change the emitted
    // boundary-regularize-roof-graph bytes; the BR_SNAP_IN=0 byte-equivalence
    // to HEAD is preserved.
    for (size_t ci = 0; ci < n_corners; ci++) {
        out->dof[ci].alive      = cs[ci].alive;
        out->dof[ci].dof        = cs[ci].dof;
        out->dof[ci].has_axis_a = cs[ci].has_axis_a;
        out->dof[ci].has_axis_b = cs[ci].has_axis_b;
        if (cs[ci].has_axis_a) memcpy(out->dof[ci].axis_a, cs[ci].axis_a, 24);
        if (cs[ci].has_axis_b) memcpy(out->dof[ci].axis_b, cs[ci].axis_b, 24);
    }
    out->n_dof = n_corners;  // #25: grown by insertion — the wrapper reads .alive for
                             // the minted corners too (else they'd default alive=true).

    // Emit trajectories for moved corners only.
    size_t n_traj = 0;
    for (size_t ci = 0; ci < n_corners; ci++) if (traj[ci].n_steps > 0) n_traj++;
    if (n_traj > 0) {
        out->trajectories = (v7_br_trajectory_t*)safe_calloc_array(n_traj, sizeof(v7_br_trajectory_t));
        if (!out->trajectories) { rc = -1; goto r1_traj_oom; }
        size_t k = 0;
        for (size_t ci = 0; ci < n_corners; ci++) {
            if (traj[ci].n_steps == 0) continue;
            out->trajectories[k].corner_id = corners_in[ci].id;
            out->trajectories[k].n_steps   = traj[ci].n_steps;
            out->trajectories[k].steps     = (v7_br_trajectory_step_t*)safe_malloc_array(traj[ci].n_steps, sizeof(v7_br_trajectory_step_t));
            if (!out->trajectories[k].steps) { rc = -1; goto r1_traj_oom; }
            for (uint32_t s = 0; s < traj[ci].n_steps; s++) {
                memcpy(out->trajectories[k].steps[s].point, &traj[ci].steps[3*s], 24);
            }
            k++;
        }
        out->n_trajectories = n_traj;
    }
    rc = 0;

r1_xplane_oom:
    free(ridge_xplane_hit);
    free(ridge_inc_buf);   // T4
    free(mc_id_scratch);   // T5: self-int guard scratch
    free(mc_u_scratch);
    free(mc_v_scratch);
r1_pw_oom:
    free(pw_edges);
    if (corner_to_pw_idxs) {
        for (size_t ci = 0; ci < n_corners; ci++) free(corner_to_pw_idxs[ci]);
        free(corner_to_pw_idxs);
    }
    free(corner_to_pw_n);
    free(corner_to_pw_cap);
    free(incident_mark);
r1_joint_oom:
    free(joint_edges);
r1_traj_oom:
    for (size_t ci = 0; ci < n_corners; ci++) free(traj[ci].steps);
    free(traj);
r1_ctx_oom:
r1_oom:
    for (size_t ci = 0; ci < n_corners; ci++) free(corner_ctx_indices[ci]);
    free(corner_ctx_indices); free(corner_ctx_n); free(corner_ctx_cap);
    free(ctxs);
    for (size_t pj = 0; pj < n_polygon_paths; pj++) free(polys[pj].ids);
    free(polys);
    for (size_t pj = 0; pj < n_planes; pj++) {
        free(pc[pj].own_u); free(pc[pj].own_v); free(pc[pj].own_w);
        free(pc[pj].near_u); free(pc[pj].near_v); free(pc[pj].near_w);
    }
    free(pc);
    for (size_t ci = 0; ci < n_corners; ci++) free(cs[ci].ridge_ids);  // A2
    free(cs);

    if (rc != 0) v7_br_outputs_free(out);
cleanup:
    free(rg);                 // A2: freed here so all exit paths release it once
    free(ridx_off);           // T4: plane→ridges CSR index
    free(ridx);
    free(corners_owned);      // #25: over-sized mutable corners copy (free(NULL) safe)
    free(plane_ins_cnt);      // #25: per-plane insertion budget
    id_map_free(&plane_map);
    id_map_free(&corner_map);
    return rc;
}
