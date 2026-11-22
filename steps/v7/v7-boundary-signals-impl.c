// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-signals — pure compute implementation. See impl.h.
//
// First pass (this commit):
//   - RI corner detection (closest-approach + dedup, ported from
//     bench-v2 boundary-edges/roof-graph.js detectCorners)
//   - Per-plane setup (project group points, build (u,v) bucket index)
//   - Phase B nearby gathering (beam-project tile points onto every
//     alive plane, bucket-prune, filter |sd|>=plane_dist_filter)
//
// Deferred to follow-up commits:
//   - Phase C signal grids (OA/CoM/balance via computeSignalGrids)
//   - Concave/convex anchor synthesis (OA-gradient local maxima)
//   - Anchor validation + corner-pool merge

#include "v7-boundary-signals-impl.h"
#include "v3/safe_alloc.h"
#include "v7-grid-contour.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Params defaults ────────────────────────────────────────────────

void v7_bs_params_default(v7_bs_params_t* p) {
    p->nearby_radius          = 2.0;
    p->plane_dist_filter      = 0.20;
    p->corner_dist_threshold  = 1.0;
    p->corner_merge_threshold = 0.25;
    p->anchor_dedup_radius    = 0.4;
    p->anchor_valid_radius    = 0.5;
    p->anchor_balance_min     = 0.25;
    p->grid_cell_size         = 0.10;
    p->grid_eval_radius       = 1.0;
    p->angular_span_min_deg   = 150.0;
    p->local_max_half_window  = 2;
    p->min_plane_pts          = 6;
    p->up_axis                = 1;
    // NaN sentinel — caller MUST set this when compute_contour is true.
    // Aborts loudly rather than silently scaling cell sizes by
    // sqrt(real/fallback). See 2026-05-22 silent-fallback audit.
    p->lidar_density          = NAN;
    p->compute_contour        = true;
}

// ── Outputs lifecycle ──────────────────────────────────────────────

void v7_bs_outputs_init(v7_bs_outputs_t* out) {
    memset(out, 0, sizeof(*out));
}

static void corner_free(v7_bs_corner_t* c) {
    free(c->plane_ids);
    free(c->ridge_ids);
    free(c->dirs);
    free(c->com_bisectors);
    free(c->name);
    memset(c, 0, sizeof(*c));
}

// Allocate a NUL-terminated heap copy of a formatted name. Caller owns.
static char* corner_name_alloc(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
static char* corner_name_alloc(const char* fmt, ...) {
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return NULL;
    size_t len = (size_t)n + 1;
    char* p = (char*)malloc(len);
    if (!p) return NULL;
    memcpy(p, buf, len);
    return p;
}

void v7_bs_grid_field_free(v7_bs_grid_field_t* f) {
    if (!f) return;
    free(f->oa_field);       free(f->oa_span_field);
    free(f->com_dist_field); free(f->pt_count_field);
    free(f->out_u);          free(f->out_v);
    free(f->best_left_field); free(f->best_right_field);
    free(f->node_x); free(f->node_y); free(f->node_z);
    memset(f, 0, sizeof(*f));
}

void v7_bs_plane_contour_free(v7_bs_plane_contour_t* c) {
    if (!c) return;
    free(c->density); free(c->smoothed); free(c->binary);
    free(c->density_nearby); free(c->smoothed_nearby);
    free(c->outer.pts);
    for (uint32_t i = 0; i < c->n_holes; i++) free(c->holes[i].pts);
    free(c->holes);
    memset(c, 0, sizeof(*c));
}

void v7_bs_plane_grid_free(v7_bs_plane_grid_t* g) {
    if (!g) return;
    v7_bs_grid_field_free(&g->group_field);
    v7_bs_grid_field_free(&g->nearby_field);
    free(g->bal_field);
    v7_bs_plane_contour_free(&g->contour);
    memset(g, 0, sizeof(*g));
}

void v7_bs_outputs_free(v7_bs_outputs_t* out) {
    if (!out) return;
    for (size_t i = 0; i < out->n_corners; i++) corner_free(&out->corners[i]);
    free(out->corners);
    for (size_t i = 0; i < out->n_nearby; i++) free(out->nearby[i].items);
    free(out->nearby);
    for (size_t i = 0; i < out->n_grids; i++) v7_bs_plane_grid_free(&out->grids[i]);
    free(out->grids);
    memset(out, 0, sizeof(*out));
}

// ── Closest-approach (line-line) ────────────────────────────────────
// Port of geometry.js#closestApproach. Returns 0 on success with
// out_midpoint, out_dist, out_t (param on line 1), out_s (param on
// line 2). Returns -1 if lines are parallel.

static int closest_approach(const double P1[3], const double D1[3],
                            const double P2[3], const double D2[3],
                            double out_midpoint[3],
                            double* out_dist,
                            double* out_t,
                            double* out_s) {
    double wx = P1[0] - P2[0], wy = P1[1] - P2[1], wz = P1[2] - P2[2];
    double a = D1[0]*D1[0] + D1[1]*D1[1] + D1[2]*D1[2];
    double b = D1[0]*D2[0] + D1[1]*D2[1] + D1[2]*D2[2];
    double c = D2[0]*D2[0] + D2[1]*D2[1] + D2[2]*D2[2];
    double dd = D1[0]*wx + D1[1]*wy + D1[2]*wz;
    double e  = D2[0]*wx + D2[1]*wy + D2[2]*wz;
    double denom = a*c - b*b;
    if (fabs(denom) < 1e-10) return -1;
    double t = (b*e - c*dd) / denom;
    double s = (a*e - b*dd) / denom;
    double c1x = P1[0] + t*D1[0], c1y = P1[1] + t*D1[1], c1z = P1[2] + t*D1[2];
    double c2x = P2[0] + s*D2[0], c2y = P2[1] + s*D2[1], c2z = P2[2] + s*D2[2];
    double dx = c1x - c2x, dy = c1y - c2y, dz = c1z - c2z;
    out_midpoint[0] = (c1x + c2x) / 2.0;
    out_midpoint[1] = (c1y + c2y) / 2.0;
    out_midpoint[2] = (c1z + c2z) / 2.0;
    *out_dist = sqrt(dx*dx + dy*dy + dz*dz);
    *out_t = t;
    *out_s = s;
    return 0;
}

// ── RI corner detection ─────────────────────────────────────────────
// Port of boundary-edges/roof-graph.js#detectCorners. For every pair
// of ridge-lines that share at least one plane, find their closest-
// approach midpoint, gate on closest-approach distance + per-side
// margin, then dedup within merge threshold (merging accumulates
// plane + ridge id sets).

typedef struct ri_raw {
    double  point[3];
    double  dist;
    uint32_t ridges[2];   // ridge indices into upstream RIDGE_LINES
    uint32_t planes[4];   // plane group ids touched (4 max; dedup'd)
    uint32_t n_planes;
    uint32_t n_merged;
} ri_raw_t;

// Naming counters — global RI/RE/MRG indices + per-plane concave/convex
// indices. Mirrors bench-v2 boundary-signals.js#L506-529 (`nameCounters`).
// RI grows monotonically as detect_ri_corners deduplicates; concave +
// convex grow per-plane inside the Phase C sweep.
typedef struct name_ctrs {
    uint32_t  ri;
    uint32_t* concave;     // length n_planes
    uint32_t* convex;      // length n_planes
    size_t    n_planes;
} name_ctrs_t;

static int detect_ri_corners(const v7_bs_input_ridge_line_t* lines, size_t n_lines,
                             double dist_thresh, double merge_thresh,
                             name_ctrs_t* ctrs,
                             v7_bs_corner_t** out, size_t* out_n) {
    *out = NULL;
    *out_n = 0;

    // Max raw pairs = N*(N-1)/2. Bench-v2 dahouet has ~30 ridges →
    // ~435 raw candidates; alloc generously.
    size_t cap = (n_lines * (n_lines - 1)) / 2 + 16;
    ri_raw_t* raw = (ri_raw_t*)safe_calloc_array(cap, sizeof(ri_raw_t));
    if (!raw && cap > 0) return -1;
    size_t n_raw = 0;
    const double margin = 1.0;

    for (size_t i = 0; i < n_lines; i++) {
        if (!lines[i].has_line) continue;
        for (size_t j = i + 1; j < n_lines; j++) {
            if (!lines[j].has_line) continue;
            // Must share at least one plane group.
            uint32_t Ai = lines[i].plane_a, Bi = lines[i].plane_b;
            uint32_t Aj = lines[j].plane_a, Bj = lines[j].plane_b;
            int shared = (Ai == Aj) | (Ai == Bj) | (Bi == Aj) | (Bi == Bj);
            if (!shared) continue;

            double mid[3], dist, t, s;
            if (closest_approach(lines[i].line_point, lines[i].line_direction,
                                  lines[j].line_point, lines[j].line_direction,
                                  mid, &dist, &t, &s) != 0) continue;
            if (dist > dist_thresh) continue;

            // Margin test against the (clipped) extents on each ridge.
            double iMin = fmax((double)lines[i].t_min_a, (double)lines[i].t_min_b);
            double iMax = fmin((double)lines[i].t_max_a, (double)lines[i].t_max_b);
            double jMin = fmax((double)lines[j].t_min_a, (double)lines[j].t_min_b);
            double jMax = fmin((double)lines[j].t_max_a, (double)lines[j].t_max_b);
            if (t < iMin - margin || t > iMax + margin) continue;
            if (s < jMin - margin || s > jMax + margin) continue;

            if (n_raw >= cap) {
                cap *= 2;
                ri_raw_t* np = (ri_raw_t*)safe_realloc_array(raw, cap, sizeof(ri_raw_t));
                if (!np) { free(raw); return -1; }
                raw = np;
            }
            ri_raw_t* r = &raw[n_raw++];
            r->point[0] = mid[0]; r->point[1] = mid[1]; r->point[2] = mid[2];
            r->dist = dist;
            r->ridges[0] = (uint32_t)i; r->ridges[1] = (uint32_t)j;
            // Dedup planes within {Ai,Bi,Aj,Bj}.
            uint32_t cand[4] = { Ai, Bi, Aj, Bj };
            r->n_planes = 0;
            for (int k = 0; k < 4; k++) {
                int dup = 0;
                for (uint32_t m = 0; m < r->n_planes; m++) {
                    if (r->planes[m] == cand[k]) { dup = 1; break; }
                }
                if (!dup) r->planes[r->n_planes++] = cand[k];
            }
            r->n_merged = 1;
        }
    }

    // Sort raw by ascending dist (prefer tighter intersections in dedup).
    // Simple in-place selection sort — n_raw is small.
    for (size_t i = 0; i < n_raw; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n_raw; j++) {
            if (raw[j].dist < raw[best].dist) best = j;
        }
        if (best != i) {
            ri_raw_t tmp = raw[i]; raw[i] = raw[best]; raw[best] = tmp;
        }
    }

    // Dedup: walk raw in dist order, merge into already-emitted corners
    // within merge_thresh.
    v7_bs_corner_t* corners = (v7_bs_corner_t*)calloc(n_raw + 1, sizeof(v7_bs_corner_t));
    if (!corners && n_raw > 0) { free(raw); return -1; }
    size_t n_corners = 0;
    double merge_sq = merge_thresh * merge_thresh;

    for (size_t i = 0; i < n_raw; i++) {
        const ri_raw_t* r = &raw[i];
        // Find existing corner within merge_thresh.
        int merged_into = -1;
        for (size_t k = 0; k < n_corners; k++) {
            double dx = r->point[0] - corners[k].point[0];
            double dy = r->point[1] - corners[k].point[1];
            double dz = r->point[2] - corners[k].point[2];
            if (dx*dx + dy*dy + dz*dz < merge_sq) { merged_into = (int)k; break; }
        }
        if (merged_into >= 0) {
            v7_bs_corner_t* C = &corners[merged_into];
            // Average position (v2 averages incrementally; we approximate
            // by per-step averaging — matches v2 byte-for-byte: average
            // of incoming with existing, then accumulate n_merged).
            C->point[0] = (C->point[0] + r->point[0]) / 2.0;
            C->point[1] = (C->point[1] + r->point[1]) / 2.0;
            C->point[2] = (C->point[2] + r->point[2]) / 2.0;
            // Append unique ridge ids.
            for (int rk = 0; rk < 2; rk++) {
                uint32_t rid = r->ridges[rk];
                int dup = 0;
                for (uint32_t m = 0; m < C->n_ridge_ids; m++) {
                    if (C->ridge_ids[m] == rid) { dup = 1; break; }
                }
                if (!dup) {
                    uint32_t* p = (uint32_t*)realloc(C->ridge_ids, (C->n_ridge_ids + 1) * sizeof(uint32_t));
                    if (!p) { /* leak the partial work and bail */ free(raw); return -1; }
                    p[C->n_ridge_ids++] = rid;
                    C->ridge_ids = p;
                }
            }
            // Append unique plane ids.
            for (uint32_t pk = 0; pk < r->n_planes; pk++) {
                uint32_t pid = r->planes[pk];
                int dup = 0;
                for (uint32_t m = 0; m < C->n_plane_ids; m++) {
                    if (C->plane_ids[m] == pid) { dup = 1; break; }
                }
                if (!dup) {
                    uint32_t* p = (uint32_t*)realloc(C->plane_ids, (C->n_plane_ids + 1) * sizeof(uint32_t));
                    if (!p) { free(raw); return -1; }
                    p[C->n_plane_ids++] = pid;
                    C->plane_ids = p;
                }
            }
            C->n_merged++;
        } else {
            // New corner.
            v7_bs_corner_t* C = &corners[n_corners];
            C->id = (uint32_t)n_corners;
            C->source = 1;  // ridge-intersection
            C->point[0] = r->point[0];
            C->point[1] = r->point[1];
            C->point[2] = r->point[2];
            C->n_merged = (uint16_t)r->n_merged;
            if (ctrs) C->name = corner_name_alloc("RI%u", ctrs->ri++);
            C->plane_ids = (uint32_t*)safe_malloc_array(r->n_planes, sizeof(uint32_t));
            if (!C->plane_ids) { free(raw); return -1; }
            memcpy(C->plane_ids, r->planes, r->n_planes * sizeof(uint32_t));
            C->n_plane_ids = r->n_planes;
            C->ridge_ids = (uint32_t*)malloc(2 * sizeof(uint32_t));
            if (!C->ridge_ids) { free(raw); return -1; }
            C->ridge_ids[0] = r->ridges[0];
            C->ridge_ids[1] = r->ridges[1];
            C->n_ridge_ids = 2;
            n_corners++;
        }
    }
    free(raw);

    *out = corners;
    *out_n = n_corners;
    return 0;
}

// ── Per-plane setup + bucket index ──────────────────────────────────

typedef struct plane_data {
    uint32_t input_idx;     // index into v7_bs_input_plane_t[]
    uint32_t plane_id;      // upstream plane id
    // group point arrays (length n_pts).
    float*    group_u;
    float*    group_v;
    double*   group_x;      // 3D positions on the plane (in input coord frame)
    double*   group_y;
    double*   group_z;
    uint32_t* group_indices;  // building-local point indices
    uint32_t  n_pts;
    // bucket index over (groupU, groupV) with cell size = nearby_radius.
    uint32_t* bucket_offsets; // length n_cells + 1 (CSR-style)
    uint32_t* bucket_items;   // length n_pts; each holds an index in [0, n_pts)
    uint32_t  b_cols, b_rows;
    float     b_min_u, b_min_v;
    float     bucket_size;
    // group set: O(1) "is this tile-local point in our group?".
    uint8_t*  in_group;     // length n_pts_local
} plane_data_t;

static void plane_data_free(plane_data_t* pd) {
    if (!pd) return;
    free(pd->group_u); free(pd->group_v);
    free(pd->group_x); free(pd->group_y); free(pd->group_z);
    free(pd->group_indices);
    free(pd->bucket_offsets); free(pd->bucket_items);
    free(pd->in_group);
    memset(pd, 0, sizeof(*pd));
}

static int build_plane_data(plane_data_t* pd,
                            uint32_t input_idx,
                            const v7_bs_input_plane_t* P,
                            const double* positions, size_t n_pts_local,
                            const int16_t* owner,
                            float bucket_size,
                            int min_pts) {
    memset(pd, 0, sizeof(*pd));
    pd->input_idx = input_idx;
    pd->plane_id = P->id;
    pd->bucket_size = bucket_size;

    // Count group points + build in_group bitmap.
    pd->in_group = (uint8_t*)calloc(n_pts_local, 1);
    if (!pd->in_group) return -1;
    uint32_t n = 0;
    for (size_t i = 0; i < n_pts_local; i++) {
        if (owner[i] >= 0 && (uint32_t)owner[i] == P->id) {
            pd->in_group[i] = 1;
            n++;
        }
    }
    if ((int)n < min_pts) {
        // Skip — return success with n_pts=0 so callers can fast-path it.
        return 0;
    }

    pd->group_u = (float*) safe_malloc_array(n, sizeof(float));
    pd->group_v = (float*) safe_malloc_array(n, sizeof(float));
    pd->group_x = (double*)safe_malloc_array(n, sizeof(double));
    pd->group_y = (double*)safe_malloc_array(n, sizeof(double));
    pd->group_z = (double*)safe_malloc_array(n, sizeof(double));
    pd->group_indices = (uint32_t*)safe_malloc_array(n, sizeof(uint32_t));
    if (!pd->group_u || !pd->group_v || !pd->group_x || !pd->group_y || !pd->group_z || !pd->group_indices) {
        plane_data_free(pd); return -1;
    }
    pd->n_pts = n;

    const double e1x = P->basis_e1[0], e1y = P->basis_e1[1], e1z = P->basis_e1[2];
    const double e2x = P->basis_e2[0], e2y = P->basis_e2[1], e2z = P->basis_e2[2];
    const double nx = P->normal[0], ny = P->normal[1], nz = P->normal[2];
    const double d = P->d;

    float min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    uint32_t k = 0;
    for (size_t i = 0; i < n_pts_local; i++) {
        if (!pd->in_group[i]) continue;
        double px = positions[i * 3 + 0];
        double py = positions[i * 3 + 1];
        double pz = positions[i * 3 + 2];
        // Project to plane: shift along normal so the point lies on the plane.
        double sd = nx * px + ny * py + nz * pz + d;
        double sx = px - sd * nx;
        double sy = py - sd * ny;
        double sz = pz - sd * nz;
        pd->group_x[k] = sx;
        pd->group_y[k] = sy;
        pd->group_z[k] = sz;
        // Project to (u, v) on basis.
        double u = sx * e1x + sy * e1y + sz * e1z;
        double v = sx * e2x + sy * e2y + sz * e2z;
        pd->group_u[k] = (float)u;
        pd->group_v[k] = (float)v;
        pd->group_indices[k] = (uint32_t)i;
        if (u < min_u) min_u = (float)u;
        if (u > max_u) max_u = (float)u;
        if (v < min_v) min_v = (float)v;
        if (v > max_v) max_v = (float)v;
        k++;
    }

    // Bucket grid spans group bbox plus one bucket-size margin so
    // disc lookups can spill safely.
    pd->b_min_u = min_u - bucket_size;
    pd->b_min_v = min_v - bucket_size;
    float b_max_u = max_u + bucket_size;
    float b_max_v = max_v + bucket_size;
    pd->b_cols = (uint32_t)fmax(1, ceilf((b_max_u - pd->b_min_u) / bucket_size));
    pd->b_rows = (uint32_t)fmax(1, ceilf((b_max_v - pd->b_min_v) / bucket_size));
    size_t n_cells = (size_t)pd->b_cols * (size_t)pd->b_rows;

    pd->bucket_offsets = (uint32_t*)calloc(n_cells + 1, sizeof(uint32_t));
    pd->bucket_items   = (uint32_t*)safe_malloc_array(n, sizeof(uint32_t));
    if (!pd->bucket_offsets || !pd->bucket_items) {
        plane_data_free(pd); return -1;
    }
    // Count per cell.
    for (uint32_t i = 0; i < n; i++) {
        int bc = (int)floorf((pd->group_u[i] - pd->b_min_u) / bucket_size);
        int br = (int)floorf((pd->group_v[i] - pd->b_min_v) / bucket_size);
        if (bc < 0) bc = 0; else if ((uint32_t)bc >= pd->b_cols) bc = (int)pd->b_cols - 1;
        if (br < 0) br = 0; else if ((uint32_t)br >= pd->b_rows) br = (int)pd->b_rows - 1;
        pd->bucket_offsets[(size_t)br * pd->b_cols + (size_t)bc + 1]++;
    }
    // Prefix-sum to get CSR offsets.
    for (size_t i = 1; i <= n_cells; i++) {
        pd->bucket_offsets[i] += pd->bucket_offsets[i - 1];
    }
    // Fill — use a copy of offsets as write cursor, restore at end.
    uint32_t* cur = (uint32_t*)safe_malloc_array(n_cells, sizeof(uint32_t));
    if (!cur) { plane_data_free(pd); return -1; }
    memcpy(cur, pd->bucket_offsets, n_cells * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        int bc = (int)floorf((pd->group_u[i] - pd->b_min_u) / bucket_size);
        int br = (int)floorf((pd->group_v[i] - pd->b_min_v) / bucket_size);
        if (bc < 0) bc = 0; else if ((uint32_t)bc >= pd->b_cols) bc = (int)pd->b_cols - 1;
        if (br < 0) br = 0; else if ((uint32_t)br >= pd->b_rows) br = (int)pd->b_rows - 1;
        size_t cell = (size_t)br * pd->b_cols + (size_t)bc;
        pd->bucket_items[cur[cell]++] = i;
    }
    free(cur);

    return 0;
}

// "Any group point of plane pd within `radius` of (u, v)?" — bucket-pruned.
static bool any_in_disc(const plane_data_t* pd, float u, float v, float radius) {
    float radSq = radius * radius;
    int bc = (int)floorf((u - pd->b_min_u) / pd->bucket_size);
    int br = (int)floorf((v - pd->b_min_v) / pd->bucket_size);
    for (int dr = -1; dr <= 1; dr++) {
        int rr = br + dr;
        if (rr < 0 || (uint32_t)rr >= pd->b_rows) continue;
        for (int dc = -1; dc <= 1; dc++) {
            int cc = bc + dc;
            if (cc < 0 || (uint32_t)cc >= pd->b_cols) continue;
            size_t cell = (size_t)rr * pd->b_cols + (size_t)cc;
            uint32_t lo = pd->bucket_offsets[cell];
            uint32_t hi = pd->bucket_offsets[cell + 1];
            for (uint32_t k = lo; k < hi; k++) {
                uint32_t pi = pd->bucket_items[k];
                float du = pd->group_u[pi] - u;
                float dv = pd->group_v[pi] - v;
                if (du * du + dv * dv <= radSq) return true;
            }
        }
    }
    return false;
}

// Sibling structure for nearby items (same shape as plane_data's group
// bucket, just over the filtered nearby projections).
typedef struct nearby_bucket {
    float*    u;
    float*    v;
    uint32_t* offsets;
    uint32_t* items;
    uint32_t  cols, rows;
    float     min_u, min_v;
    float     bucket_size;
    uint32_t  n;
} nearby_bucket_t;

static void nearby_bucket_free(nearby_bucket_t* nb) {
    if (!nb) return;
    free(nb->u); free(nb->v); free(nb->offsets); free(nb->items);
    memset(nb, 0, sizeof(*nb));
}

static int build_nearby_bucket(nearby_bucket_t* nb,
                                const v7_bs_nearby_item_t* items, uint32_t n_items,
                                const double basis_e1[3], const double basis_e2[3],
                                float bucket_size) {
    memset(nb, 0, sizeof(*nb));
    nb->bucket_size = bucket_size;
    nb->n = n_items;
    if (n_items == 0) return 0;
    nb->u = (float*)safe_malloc_array(n_items, sizeof(float));
    nb->v = (float*)safe_malloc_array(n_items, sizeof(float));
    if (!nb->u || !nb->v) { nearby_bucket_free(nb); return -1; }
    float min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    for (uint32_t i = 0; i < n_items; i++) {
        double u = items[i].position[0] * basis_e1[0]
                 + items[i].position[1] * basis_e1[1]
                 + items[i].position[2] * basis_e1[2];
        double v = items[i].position[0] * basis_e2[0]
                 + items[i].position[1] * basis_e2[1]
                 + items[i].position[2] * basis_e2[2];
        nb->u[i] = (float)u; nb->v[i] = (float)v;
        if (u < min_u) min_u = (float)u;
        if (u > max_u) max_u = (float)u;
        if (v < min_v) min_v = (float)v;
        if (v > max_v) max_v = (float)v;
    }
    nb->min_u = min_u - bucket_size;
    nb->min_v = min_v - bucket_size;
    float max_u_ext = max_u + bucket_size;
    float max_v_ext = max_v + bucket_size;
    nb->cols = (uint32_t)fmaxf(1.0f, ceilf((max_u_ext - nb->min_u) / bucket_size));
    nb->rows = (uint32_t)fmaxf(1.0f, ceilf((max_v_ext - nb->min_v) / bucket_size));
    size_t n_cells = (size_t)nb->cols * (size_t)nb->rows;
    nb->offsets = (uint32_t*)calloc(n_cells + 1, sizeof(uint32_t));
    nb->items = (uint32_t*)safe_malloc_array(n_items, sizeof(uint32_t));
    if (!nb->offsets || !nb->items) { nearby_bucket_free(nb); return -1; }
    for (uint32_t i = 0; i < n_items; i++) {
        int bc = (int)floorf((nb->u[i] - nb->min_u) / bucket_size);
        int br = (int)floorf((nb->v[i] - nb->min_v) / bucket_size);
        if (bc < 0) bc = 0; else if ((uint32_t)bc >= nb->cols) bc = (int)nb->cols - 1;
        if (br < 0) br = 0; else if ((uint32_t)br >= nb->rows) br = (int)nb->rows - 1;
        nb->offsets[(size_t)br * nb->cols + (size_t)bc + 1]++;
    }
    for (size_t i = 1; i <= n_cells; i++) nb->offsets[i] += nb->offsets[i - 1];
    uint32_t* cur = (uint32_t*)safe_malloc_array(n_cells, sizeof(uint32_t));
    if (!cur) { nearby_bucket_free(nb); return -1; }
    memcpy(cur, nb->offsets, n_cells * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_items; i++) {
        int bc = (int)floorf((nb->u[i] - nb->min_u) / bucket_size);
        int br = (int)floorf((nb->v[i] - nb->min_v) / bucket_size);
        if (bc < 0) bc = 0; else if ((uint32_t)bc >= nb->cols) bc = (int)nb->cols - 1;
        if (br < 0) br = 0; else if ((uint32_t)br >= nb->rows) br = (int)nb->rows - 1;
        size_t cell = (size_t)br * nb->cols + (size_t)bc;
        nb->items[cur[cell]++] = i;
    }
    free(cur);
    return 0;
}

static bool nb_any_in_disc(const nearby_bucket_t* nb, float u, float v, float radius) {
    if (nb->n == 0) return false;
    float radSq = radius * radius;
    int bc = (int)floorf((u - nb->min_u) / nb->bucket_size);
    int br = (int)floorf((v - nb->min_v) / nb->bucket_size);
    for (int dr = -1; dr <= 1; dr++) {
        int rr = br + dr;
        if (rr < 0 || (uint32_t)rr >= nb->rows) continue;
        for (int dc = -1; dc <= 1; dc++) {
            int cc = bc + dc;
            if (cc < 0 || (uint32_t)cc >= nb->cols) continue;
            size_t cell = (size_t)rr * nb->cols + (size_t)cc;
            uint32_t lo = nb->offsets[cell];
            uint32_t hi = nb->offsets[cell + 1];
            for (uint32_t k = lo; k < hi; k++) {
                uint32_t pi = nb->items[k];
                float du = nb->u[pi] - u;
                float dv = nb->v[pi] - v;
                if (du * du + dv * dv <= radSq) return true;
            }
        }
    }
    return false;
}

// ── Signal grids (Phase C inner loop) ──────────────────────────────
// Port of bench-v2 computeSignalGrids (boundary-signals.js#L70-373).
// All point inputs in caller's coord frame (typically building-local,
// projected onto the plane). Produces field arrays + candidate corners
// (OA-gradient local maxima with span ≥ SPAN_MIN, deduped within 0.4 m).

typedef struct sg_corner_cand {
    double center[3];
    float  dir_a[3];
    float  dir_b[3];
    float  span_deg;
    float  oa_deg;
} sg_corner_cand_t;

typedef struct sg_result {
    v7_bs_grid_field_t field;
    sg_corner_cand_t* candidates;
    uint32_t n_candidates;
} sg_result_t;

static void sg_result_free(sg_result_t* r) {
    if (!r) return;
    v7_bs_grid_field_free(&r->field);
    free(r->candidates);
    memset(r, 0, sizeof(*r));
}

static int compute_signal_grids(const double normal[3], double d,
                                 const double e1[3], const double e2[3],
                                 const double* px, const double* py, const double* pz,
                                 size_t n_pts,
                                 const v7_bs_params_t* params,
                                 sg_result_t* out) {
    memset(out, 0, sizeof(*out));
    if (n_pts == 0) return 0;

    // (u, v) bounding box.
    double min_u = INFINITY, max_u = -INFINITY, min_v = INFINITY, max_v = -INFINITY;
    for (size_t i = 0; i < n_pts; i++) {
        double u = px[i] * e1[0] + py[i] * e1[1] + pz[i] * e1[2];
        double v = px[i] * e2[0] + py[i] * e2[1] + pz[i] * e2[2];
        if (u < min_u) min_u = u;
        if (u > max_u) max_u = u;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    const double cell_size = params->grid_cell_size;
    const double eval_radius = params->grid_eval_radius;
    const double eval_sq = eval_radius * eval_radius;
    const double margin = eval_radius;
    const double g_min_u = min_u - margin;
    const double g_min_v = min_v - margin;
    const double g_max_u = max_u + margin;
    const double g_max_v = max_v + margin;
    uint32_t n_cols = (uint32_t)(ceil((g_max_u - g_min_u) / cell_size) + 1);
    uint32_t n_rows = (uint32_t)(ceil((g_max_v - g_min_v) / cell_size) + 1);
    size_t n_cells = (size_t)n_cols * (size_t)n_rows;
    if (n_cells == 0 || n_cells > (size_t)1u << 24) return 0;  // sanity cap

    v7_bs_grid_field_t* F = &out->field;
    F->n_cols = n_cols; F->n_rows = n_rows;
    F->cell_size = (float)cell_size;
    F->g_min_u = (float)g_min_u; F->g_min_v = (float)g_min_v;
    F->oa_field        = (float*)safe_malloc_array(n_cells, sizeof(float));
    F->oa_span_field   = (float*)safe_malloc_array(n_cells, sizeof(float));
    F->com_dist_field  = (float*)safe_malloc_array(n_cells, sizeof(float));
    F->pt_count_field  = (uint16_t*)safe_calloc_array(n_cells, sizeof(uint16_t));
    F->out_u           = (float*)safe_calloc_array(n_cells, sizeof(float));
    F->out_v           = (float*)safe_calloc_array(n_cells, sizeof(float));
    F->best_left_field = (float*)safe_malloc_array(n_cells, sizeof(float));
    F->best_right_field = (float*)safe_malloc_array(n_cells, sizeof(float));
    F->node_x = (float*)safe_calloc_array(n_cells, sizeof(float));
    F->node_y = (float*)safe_calloc_array(n_cells, sizeof(float));
    F->node_z = (float*)safe_calloc_array(n_cells, sizeof(float));
    if (!F->oa_field || !F->oa_span_field || !F->com_dist_field ||
        !F->pt_count_field || !F->out_u || !F->out_v ||
        !F->best_left_field || !F->best_right_field ||
        !F->node_x || !F->node_y || !F->node_z) {
        sg_result_free(out); return -1;
    }
    for (size_t i = 0; i < n_cells; i++) {
        F->oa_field[i] = NAN;
        F->oa_span_field[i] = NAN;
        F->com_dist_field[i] = NAN;
        F->best_left_field[i]  = (float)M_PI;
        F->best_right_field[i] = -(float)M_PI;
    }

    const double nx = normal[0], ny = normal[1], nz = normal[2];

    // Main cell sweep — for each cell compute centroid + outward angle +
    // best-left/right + open angle.
    for (uint32_t row = 0; row < n_rows; row++) {
        double v_at = g_min_v + (double)row * cell_size;
        for (uint32_t col = 0; col < n_cols; col++) {
            double u_at = g_min_u + (double)col * cell_size;
            // 3D node position projected onto plane.
            double x = u_at * e1[0] + v_at * e2[0];
            double y = u_at * e1[1] + v_at * e2[1];
            double z = u_at * e1[2] + v_at * e2[2];
            double sd = nx * x + ny * y + nz * z + d;
            double node_x = x - sd * nx;
            double node_y = y - sd * ny;
            double node_z = z - sd * nz;

            // First pass: centroid + count.
            double cx = 0, cy = 0, cz = 0;
            int n_inside = 0;
            for (size_t i = 0; i < n_pts; i++) {
                double dx = px[i] - node_x, dy = py[i] - node_y, dz = pz[i] - node_z;
                if (dx*dx + dy*dy + dz*dz > eval_sq) continue;
                cx += px[i]; cy += py[i]; cz += pz[i]; n_inside++;
            }
            if (n_inside == 0) continue;
            cx /= n_inside; cy /= n_inside; cz /= n_inside;

            double oX = node_x - cx, oY = node_y - cy, oZ = node_z - cz;
            double com_dist = sqrt(oX*oX + oY*oY + oZ*oZ);
            size_t idx = (size_t)row * n_cols + (size_t)col;
            F->com_dist_field[idx] = (float)com_dist;
            F->pt_count_field[idx] = (uint16_t)(n_inside > 0xFFFF ? 0xFFFF : n_inside);
            F->node_x[idx] = (float)node_x;
            F->node_y[idx] = (float)node_y;
            F->node_z[idx] = (float)node_z;

            if (com_dist < 1e-6) continue;

            // FP-trap audit #274 note: atan2 + angle subtraction below
            // looks suspicious but is safe given the guards. atan2(y, x)
            // is well-defined everywhere except (0, 0) where IEEE-754
            // returns 0; the com_dist guard above prevents that for
            // `outward`. The inner-loop atan2 of (rV, rU) is similarly
            // safe via the d2 < 1e-12 guard below. The angle-subtraction
            // is normalized to [-π, π] via the standard while-loop
            // wrap; no catastrophic cancellation because the values are
            // bounded magnitudes from atan2's [-π, π] range.
            double oU = oX * e1[0] + oY * e1[1] + oZ * e1[2];
            double oV = oX * e2[0] + oY * e2[1] + oZ * e2[2];
            double outward = atan2(oV, oU);
            double oU_len = sqrt(oU*oU + oV*oV);

            // Second pass: bestLeft / bestRight relative to outward angle.
            double best_left = M_PI, best_right = -M_PI;
            bool has_left = false, has_right = false;
            for (size_t i = 0; i < n_pts; i++) {
                double dx = px[i] - node_x, dy = py[i] - node_y, dz = pz[i] - node_z;
                double d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > eval_sq || d2 < 1e-12) continue;
                double rU = dx * e1[0] + dy * e1[1] + dz * e1[2];
                double rV = dx * e2[0] + dy * e2[1] + dz * e2[2];
                double rel = atan2(rV, rU) - outward;
                while (rel >   M_PI) rel -= 2.0 * M_PI;
                while (rel <= -M_PI) rel += 2.0 * M_PI;
                if (rel > 0 && rel < best_left)  { best_left = rel;  has_left  = true; }
                if (rel < 0 && rel > best_right) { best_right = rel; has_right = true; }
            }
            // FP-trap audit #274 note: when has_left/has_right is false
            // (sparse cell — no points found on that angular side), the
            // fallback to π means "fully open on that side". Conservative
            // and intentional — anchors in low-density cells get a wide
            // open_angle and the downstream span-min filter handles them.
            double open_angle = (has_left ? best_left : M_PI)
                              + (has_right ? -best_right : M_PI);
            F->oa_field[idx]      = (float)open_angle;
            F->out_u[idx]         = (float)(oU / oU_len);
            F->out_v[idx]         = (float)(oV / oU_len);
            F->best_left_field[idx]  = (float)(has_left  ? best_left  :  M_PI);
            F->best_right_field[idx] = (float)(has_right ? best_right : -M_PI);
        }
    }

    // Post-pass — directional OA gradient (max abs diff across opposing pairs).
    static const int NB_OFFS[4][2][2] = {
        {{ 0, -1}, { 0,  1}}, {{-1,  0}, { 1,  0}},
        {{-1, -1}, { 1,  1}}, {{-1,  1}, { 1, -1}},
    };
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < n_cols; col++) {
            size_t idx = (size_t)row * n_cols + (size_t)col;
            float oa = F->oa_field[idx];
            if (isnan(oa)) continue;
            float max_grad = 0;
            for (int p = 0; p < 4; p++) {
                int rA = (int)row + NB_OFFS[p][0][1], cA = (int)col + NB_OFFS[p][0][0];
                int rB = (int)row + NB_OFFS[p][1][1], cB = (int)col + NB_OFFS[p][1][0];
                float vA = oa, vB = oa;
                if (rA >= 0 && (uint32_t)rA < n_rows && cA >= 0 && (uint32_t)cA < n_cols) {
                    float va = F->oa_field[(size_t)rA * n_cols + (size_t)cA];
                    if (!isnan(va)) vA = va;
                }
                if (rB >= 0 && (uint32_t)rB < n_rows && cB >= 0 && (uint32_t)cB < n_cols) {
                    float vb = F->oa_field[(size_t)rB * n_cols + (size_t)cB];
                    if (!isnan(vb)) vB = vb;
                }
                float g = fabsf(vA - vB);
                if (g > max_grad) max_grad = g;
            }
            F->oa_span_field[idx] = max_grad;
        }
    }

    // Corner synthesis — local maxima of oaSpanField with span ≥ SPAN_MIN
    // in a (2H+1)×(2H+1) window. For each peak emit cornerCand at the
    // cell's 3D node with two edge directions taken from the cell's
    // best-OA neighbour's outward+bestLeft/bestRight rotated by basis.
    const float span_min = (float)(params->angular_span_min_deg * M_PI / 180.0);
    const int H = params->local_max_half_window;
    static const int NB8[8][2] = {
        {-1,-1},{ 0,-1},{ 1,-1},{-1, 0},{ 1, 0},{-1, 1},{ 0, 1},{ 1, 1},
    };
    const float edge_len = 0.5f;  // unit-length internally; we keep dir vectors
    sg_corner_cand_t* raw = NULL;
    size_t n_raw = 0, cap = 0;
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < n_cols; col++) {
            size_t idx = (size_t)row * n_cols + (size_t)col;
            float span = F->oa_span_field[idx];
            if (isnan(span) || span < span_min) continue;
            bool is_max = true;
            for (int dr = -H; dr <= H && is_max; dr++) {
                for (int dc = -H; dc <= H && is_max; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = (int)row + dr, nc = (int)col + dc;
                    if (nr < 0 || (uint32_t)nr >= n_rows ||
                        nc < 0 || (uint32_t)nc >= n_cols) continue;
                    float nv = F->oa_span_field[(size_t)nr * n_cols + (size_t)nc];
                    if (!isnan(nv) && nv > span) is_max = false;
                }
            }
            if (!is_max) continue;

            size_t dir_idx = idx;
            float best_nb_oa = F->oa_field[idx];
            for (int k = 0; k < 8; k++) {
                int nr = (int)row + NB8[k][1], nc = (int)col + NB8[k][0];
                if (nr < 0 || (uint32_t)nr >= n_rows ||
                    nc < 0 || (uint32_t)nc >= n_cols) continue;
                size_t j = (size_t)nr * n_cols + (size_t)nc;
                float v = F->oa_field[j];
                if (isnan(v)) continue;
                if (v > best_nb_oa) { best_nb_oa = v; dir_idx = j; }
            }

            double outward = atan2((double)F->out_v[dir_idx], (double)F->out_u[dir_idx]);
            double left_ang  = outward + (double)F->best_left_field[dir_idx];
            double right_ang = outward + (double)F->best_right_field[dir_idx];
            double lcos = cos(left_ang),  lsin = sin(left_ang);
            double rcos = cos(right_ang), rsin = sin(right_ang);
            double ldx = lcos * e1[0] + lsin * e2[0];
            double ldy = lcos * e1[1] + lsin * e2[1];
            double ldz = lcos * e1[2] + lsin * e2[2];
            double rdx = rcos * e1[0] + rsin * e2[0];
            double rdy = rcos * e1[1] + rsin * e2[1];
            double rdz = rcos * e1[2] + rsin * e2[2];

            if (n_raw >= cap) {
                cap = cap ? cap * 2 : 32;
                sg_corner_cand_t* p = (sg_corner_cand_t*)realloc(raw, cap * sizeof(*p));
                if (!p) { free(raw); sg_result_free(out); return -1; }
                raw = p;
            }
            sg_corner_cand_t* c = &raw[n_raw++];
            c->center[0] = (double)F->node_x[idx];
            c->center[1] = (double)F->node_y[idx];
            c->center[2] = (double)F->node_z[idx];
            c->dir_a[0] = (float)ldx; c->dir_a[1] = (float)ldy; c->dir_a[2] = (float)ldz;
            c->dir_b[0] = (float)rdx; c->dir_b[1] = (float)rdy; c->dir_b[2] = (float)rdz;
            c->span_deg = (float)(span * 180.0 / M_PI);
            c->oa_deg   = (float)((double)F->oa_field[idx] * 180.0 / M_PI);
            (void)edge_len;
        }
    }

    // Per-plane dedup — sort raw by descending span, drop near-duplicates
    // (<0.4 m apart).
    for (size_t i = 0; i < n_raw; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n_raw; j++) {
            if (raw[j].span_deg > raw[best].span_deg) best = j;
        }
        if (best != i) { sg_corner_cand_t t = raw[i]; raw[i] = raw[best]; raw[best] = t; }
    }
    const double dedup_sq = params->anchor_dedup_radius * params->anchor_dedup_radius;
    sg_corner_cand_t* kept = (sg_corner_cand_t*)malloc((n_raw + 1) * sizeof(*kept));
    if (!kept && n_raw > 0) { free(raw); sg_result_free(out); return -1; }
    size_t n_kept = 0;
    for (size_t i = 0; i < n_raw; i++) {
        bool dup = false;
        for (size_t k = 0; k < n_kept; k++) {
            double dx = raw[i].center[0] - kept[k].center[0];
            double dy = raw[i].center[1] - kept[k].center[1];
            double dz = raw[i].center[2] - kept[k].center[2];
            if (dx*dx + dy*dy + dz*dz < dedup_sq) { dup = true; break; }
        }
        if (!dup) kept[n_kept++] = raw[i];
    }
    free(raw);
    out->candidates = kept;
    out->n_candidates = (uint32_t)n_kept;
    return 0;
}

// ── Balance field ──────────────────────────────────────────────────
// Combines group + nearby CoM with point-count trust blending + opposing-
// direction reward. Port of v2 boundary-signals.js#L784-844.

static int compute_balance_field(const v7_bs_grid_field_t* gf,
                                  const v7_bs_grid_field_t* nf,
                                  float ref_com_dist,
                                  float** out_bal) {
    if (!gf || gf->n_cols == 0 || gf->n_rows == 0) { *out_bal = NULL; return 0; }
    size_t n_cells = (size_t)gf->n_cols * (size_t)gf->n_rows;
    float* bal = (float*)safe_malloc_array(n_cells, sizeof(float));
    if (!bal) return -1;
    for (size_t i = 0; i < n_cells; i++) bal[i] = NAN;
    const int MIN_PTS_FULL_TRUST = 5;
    const float half_width = ref_com_dist * 0.5f;
    for (uint32_t row = 0; row < gf->n_rows; row++) {
        for (uint32_t col = 0; col < gf->n_cols; col++) {
            size_t i = (size_t)row * gf->n_cols + col;
            float gCom = gf->com_dist_field[i];
            bool hasG = !isnan(gCom) && !isnan(gf->oa_field[i]);
            if (!hasG) continue;
            // Sibling-cell lookup in nearby grid.
            float nCom = NAN;
            bool hasN = false;
            size_t nIdx = (size_t)-1;
            if (nf && nf->n_cols > 0 && nf->n_rows > 0) {
                float u = gf->g_min_u + (float)col * gf->cell_size;
                float v = gf->g_min_v + (float)row * gf->cell_size;
                int nc = (int)floorf((u - nf->g_min_u) / nf->cell_size);
                int nr = (int)floorf((v - nf->g_min_v) / nf->cell_size);
                if (nc >= 0 && (uint32_t)nc < nf->n_cols && nr >= 0 && (uint32_t)nr < nf->n_rows) {
                    nIdx = (size_t)nr * nf->n_cols + (size_t)nc;
                    nCom = nf->com_dist_field[nIdx];
                    hasN = !isnan(nCom) && !isnan(nf->oa_field[nIdx]);
                }
            }

            float balance;
            if (hasG && hasN) {
                int cntG = gf->pt_count_field[i];
                int cntN = nf->pt_count_field[nIdx];
                float trustG = fminf(1.0f, (float)cntG / MIN_PTS_FULL_TRUST);
                float trustN = fminf(1.0f, (float)cntN / MIN_PTS_FULL_TRUST);
                float effG = trustG * gCom + (1.0f - trustG) * ref_com_dist;
                float effN = trustN * nCom + (1.0f - trustN) * ref_com_dist;
                float sum = effG + effN;
                if (sum < 1e-6f) continue;
                float raw_bal = 1.0f - fabsf(effG - effN) / sum;
                float raw_opp = -(gf->out_u[i] * nf->out_u[nIdx]
                                + gf->out_v[i] * nf->out_v[nIdx]);
                float opp_trust = fminf(1.0f, (float)(cntG < cntN ? cntG : cntN) / MIN_PTS_FULL_TRUST);
                float opposition = opp_trust * raw_opp + (1.0f - opp_trust) * 0.5f;
                balance = raw_bal * fmaxf(0.0f, opposition);
            } else {
                int cntG = gf->pt_count_field[i];
                float trustG = fminf(1.0f, (float)cntG / MIN_PTS_FULL_TRUST);
                float effCom = trustG * gCom + (1.0f - trustG) * ref_com_dist;
                float dev = fabsf(effCom - ref_com_dist);
                if (dev >= half_width) continue;
                balance = (1.0f - dev / half_width) * 0.5f;
                float oa_deg = gf->oa_field[i] * 180.0f / (float)M_PI;
                if (oa_deg < 90.0f) balance *= oa_deg / 90.0f;
            }
            bal[i] = balance;
        }
    }
    *out_bal = bal;
    return 0;
}

// ── Anchor validation ─────────────────────────────────────────────
// Return true if the corner candidate is valid: within VALID_RADIUS of a
// group point AND of a nearby item, AND balance ≥ BALANCE_MIN at the
// projected (u, v) cell.

static bool anchor_valid(const sg_corner_cand_t* c,
                          const plane_data_t* pd,
                          const nearby_bucket_t* nb,
                          const v7_bs_grid_field_t* gf, const float* bal_field,
                          const double e1[3], const double e2[3],
                          double valid_radius, double balance_min) {
    float u = (float)(c->center[0] * e1[0] + c->center[1] * e1[1] + c->center[2] * e1[2]);
    float v = (float)(c->center[0] * e2[0] + c->center[1] * e2[1] + c->center[2] * e2[2]);
    if (!any_in_disc(pd, u, v, (float)valid_radius)) return false;
    if (!nb_any_in_disc(nb, u, v, (float)valid_radius)) return false;
    if (bal_field && gf && gf->n_cols > 0 && gf->n_rows > 0) {
        int col = (int)floorf((u - gf->g_min_u) / gf->cell_size);
        int row = (int)floorf((v - gf->g_min_v) / gf->cell_size);
        if (col >= 0 && (uint32_t)col < gf->n_cols && row >= 0 && (uint32_t)row < gf->n_rows) {
            float bal = bal_field[(size_t)row * gf->n_cols + (size_t)col];
            if (!isnan(bal) && bal < (float)balance_min) return false;
        }
    }
    return true;
}

// ── Phase B: nearby gathering ───────────────────────────────────────

typedef struct nearby_accum {
    v7_bs_nearby_item_t* items;
    size_t n;
    size_t cap;
} nearby_accum_t;

static int accum_push(nearby_accum_t* a, const v7_bs_nearby_item_t* item) {
    if (a->n >= a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 64;
        v7_bs_nearby_item_t* p = (v7_bs_nearby_item_t*)realloc(a->items, nc * sizeof(*p));
        if (!p) return -1;
        a->items = p;
        a->cap = nc;
    }
    a->items[a->n++] = *item;
    return 0;
}

// For each tile point with beam direction, project onto every alive
// plane (skip if point is IN that plane's group), bucket-prune, push
// keepers. After the full pass, filter by |signed_dist| >= plane_dist_filter.

static int gather_nearby(const double* positions,
                         const float*  beam_dirs,
                         size_t n_pts_local,
                         plane_data_t* pds, size_t n_pds,
                         double nearby_radius,
                         double plane_dist_filter,
                         const v7_bs_input_plane_t* planes,
                         nearby_accum_t* per_plane) {
    if (!beam_dirs) return 0;  // Phase B skipped without beams.

    for (size_t pi = 0; pi < n_pts_local; pi++) {
        const double px = positions[pi * 3 + 0];
        const double py = positions[pi * 3 + 1];
        const double pz = positions[pi * 3 + 2];
        const float bx = beam_dirs[pi * 3 + 0];
        const float by = beam_dirs[pi * 3 + 1];
        const float bz = beam_dirs[pi * 3 + 2];

        for (size_t g = 0; g < n_pds; g++) {
            const plane_data_t* pd = &pds[g];
            if (pd->n_pts == 0) continue;
            if (pd->in_group[pi]) continue;  // skip own group's points

            const v7_bs_input_plane_t* P = &planes[pd->input_idx];
            double nx = P->normal[0], ny = P->normal[1], nz = P->normal[2];
            double denom = nx * bx + ny * by + nz * bz;
            if (fabs(denom) < 1e-6) continue;

            double sd = nx * px + ny * py + nz * pz + P->d;
            double t = -sd / denom;
            if (t >= 0) continue;  // v2: keep only t<0 hits (beam from sensor)

            double hit_x = px + t * bx;
            double hit_y = py + t * by;
            double hit_z = pz + t * bz;

            double e1x = P->basis_e1[0], e1y = P->basis_e1[1], e1z = P->basis_e1[2];
            double e2x = P->basis_e2[0], e2y = P->basis_e2[1], e2z = P->basis_e2[2];
            float u = (float)(hit_x * e1x + hit_y * e1y + hit_z * e1z);
            float v = (float)(hit_x * e2x + hit_y * e2y + hit_z * e2z);

            if (!any_in_disc(pd, u, v, (float)nearby_radius)) continue;

            // NOTE: |sd|>=plane_dist_filter is NOT applied here. v2 keeps
            // all items in pd.nearbyItems for the anchor_valid proximity
            // check (boundary-signals.js#L668-672 unconditional push);
            // the |sd| filter is applied later, *only* when building the
            // signal-grid input (line ~770). Phase C below mirrors that
            // split: full set powers the nb_idx bucket, filtered subset
            // feeds compute_signal_grids.
            (void)plane_dist_filter;

            v7_bs_nearby_item_t item;
            item.position[0] = hit_x;
            item.position[1] = hit_y;
            item.position[2] = hit_z;
            item.signed_dist = (float)sd;
            item.src_idx     = (uint32_t)pi;
            if (accum_push(&per_plane[g], &item) != 0) return -1;
        }
    }
    return 0;
}

// ── Top-level compute ──────────────────────────────────────────────

int v7_bs_compute(const double* positions,
                  const float*  beam_dirs,
                  const float*  point_weights,
                  size_t n_pts_local,
                  const int16_t* owner,
                  const v7_bs_input_plane_t* planes, size_t n_planes,
                  const v7_bs_input_ridge_line_t* ridge_lines, size_t n_ridge_lines,
                  const v7_bs_void_hull_t*   voids,        size_t n_voids,
                  const v7_bs_plane_voids_t* plane_voids,  size_t n_plane_voids,
                  const v7_bs_params_t* params,
                  v7_bs_outputs_t* out) {
    v7_bs_outputs_init(out);
    // point_weights is REQUIRED ONLY when there are points to weight:
    // the only read site (line ~1332: `point_weights[idx]`) is guarded
    // by `idx < n_pts_local`. With n_pts_local == 0 that loop body is
    // unreachable, so a NULL weights pointer is legitimate — empty-
    // point-cloud flows (ridge-line-only RI corners, determinism tests
    // around line 319, all-empty no-crash) all hit this case.
    if (n_pts_local > 0 && !point_weights) return -1;

    // Naming counters — RI is global, concave/convex are per-plane.
    name_ctrs_t ctrs = {0};
    ctrs.n_planes = n_planes;
    if (n_planes > 0) {
        ctrs.concave = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
        ctrs.convex  = (uint32_t*)safe_calloc_array(n_planes, sizeof(uint32_t));
        if (!ctrs.concave || !ctrs.convex) {
            free(ctrs.concave); free(ctrs.convex);
            return -1;
        }
    }

    // 1. RI corners from upstream RIDGE_LINES.
    v7_bs_corner_t* ri_corners = NULL;
    size_t n_ri = 0;
    if (detect_ri_corners(ridge_lines, n_ridge_lines,
                           params->corner_dist_threshold,
                           params->corner_merge_threshold,
                           &ctrs,
                           &ri_corners, &n_ri) != 0) {
        free(ctrs.concave); free(ctrs.convex);
        return -1;
    }

    // 2. Per-plane setup (only for surviving planes).
    plane_data_t* pds = (plane_data_t*)safe_calloc_array(n_planes, sizeof(plane_data_t));
    if (!pds && n_planes > 0) {
        for (size_t i = 0; i < n_ri; i++) corner_free(&ri_corners[i]);
        free(ri_corners); return -1;
    }
    for (size_t i = 0; i < n_planes; i++) {
        if (planes[i].rejected) continue;
        if (build_plane_data(&pds[i], (uint32_t)i, &planes[i],
                              positions, n_pts_local, owner,
                              (float)params->nearby_radius,
                              params->min_plane_pts) != 0) {
            for (size_t j = 0; j < n_planes; j++) plane_data_free(&pds[j]);
            free(pds);
            for (size_t i2 = 0; i2 < n_ri; i2++) corner_free(&ri_corners[i2]);
            free(ri_corners); return -1;
        }
    }

    // 3. Phase B nearby gathering.
    nearby_accum_t* accum = (nearby_accum_t*)safe_calloc_array(n_planes, sizeof(nearby_accum_t));
    if (!accum && n_planes > 0) {
        for (size_t j = 0; j < n_planes; j++) plane_data_free(&pds[j]);
        free(pds);
        for (size_t i2 = 0; i2 < n_ri; i2++) corner_free(&ri_corners[i2]);
        free(ri_corners);
        free(ctrs.concave); free(ctrs.convex);
        return -1;
    }
    if (gather_nearby(positions, beam_dirs, n_pts_local,
                       pds, n_planes,
                       params->nearby_radius, params->plane_dist_filter,
                       planes, accum) != 0) {
        for (size_t j = 0; j < n_planes; j++) free(accum[j].items);
        free(accum);
        for (size_t j = 0; j < n_planes; j++) plane_data_free(&pds[j]);
        free(pds);
        for (size_t i2 = 0; i2 < n_ri; i2++) corner_free(&ri_corners[i2]);
        free(ri_corners);
        free(ctrs.concave); free(ctrs.convex);
        return -1;
    }

    // Seed the corner pool with the RI corners.
    out->corners = ri_corners;
    out->n_corners = n_ri;
    ri_corners = NULL;  // ownership transferred to out

    // 4. Per-plane nearby — one entry per surviving plane (rejected skipped).
    size_t n_alive = 0;
    for (size_t i = 0; i < n_planes; i++) if (pds[i].n_pts > 0) n_alive++;
    out->nearby = (v7_bs_nearby_per_plane_t*)calloc(n_alive ? n_alive : 1, sizeof(*out->nearby));
    if (!out->nearby) goto out_fail;
    out->n_nearby = 0;
    for (size_t i = 0; i < n_planes; i++) {
        if (pds[i].n_pts == 0) { free(accum[i].items); continue; }
        v7_bs_nearby_per_plane_t* npp = &out->nearby[out->n_nearby++];
        npp->plane_id = planes[i].id;
        npp->items    = accum[i].items;
        npp->n_items  = (uint32_t)accum[i].n;
        accum[i].items = NULL;
        accum[i].n = accum[i].cap = 0;
    }
    free(accum);

    // 5. Phase C — per-plane signal grids + concave/convex synthesis.
    // We allocate per-plane grids matching n_alive, and per-plane anchors
    // are accumulated into a single flat list with plane_id back-pointers.
    out->grids = (v7_bs_plane_grid_t*)calloc(out->n_nearby ? out->n_nearby : 1,
                                              sizeof(*out->grids));
    if (!out->grids && out->n_nearby > 0) goto out_fail;
    out->n_grids = out->n_nearby;

    // Reference CoM distance: matches v2 REF_COM_DIST = 4*evalRadius/(3π).
    const float REF_COM_DIST = (float)(4.0 * params->grid_eval_radius / (3.0 * M_PI));

    // Append-only accumulator for concave/convex pool corners.
    // We grow out->corners (already contains RI corners).
    size_t corners_cap = out->n_corners;

    // Build a parallel filtered-nearby array per plane (|sd|>=plane_dist_filter)
    // — Phase C grids are computed on this subset, not the full Phase B list.
    size_t alive_i = 0;
    for (size_t pi = 0; pi < n_planes; pi++) {
        const plane_data_t* pd = &pds[pi];
        if (pd->n_pts == 0) continue;
        const v7_bs_input_plane_t* P = &planes[pi];
        v7_bs_plane_grid_t* PG = &out->grids[alive_i];
        PG->plane_id = P->id;
        const v7_bs_nearby_per_plane_t* npp = &out->nearby[alive_i];

        // Filtered nearby positions (|sd|>=plane_dist_filter) for the
        // signal-grid input. npp->items now holds the FULL nearby list
        // (matching v2 boundary-signals.js pd.nearbyItems) — we filter
        // here, only when feeding compute_signal_grids, matching v2's
        // line ~770 split. The full set still powers the nb_idx bucket
        // below so anchor_valid sees the same items v2 does.
        double* nb_px = NULL; double* nb_py = NULL; double* nb_pz = NULL;
        uint32_t n_nb_filtered = 0;
        if (npp->n_items > 0) {
            nb_px = (double*)safe_malloc_array(npp->n_items, sizeof(double));
            nb_py = (double*)safe_malloc_array(npp->n_items, sizeof(double));
            nb_pz = (double*)safe_malloc_array(npp->n_items, sizeof(double));
            if (!nb_px || !nb_py || !nb_pz) { free(nb_px); free(nb_py); free(nb_pz); goto out_fail; }
            const float thresh = (float)params->plane_dist_filter;
            for (uint32_t k = 0; k < npp->n_items; k++) {
                if (fabsf(npp->items[k].signed_dist) < thresh) continue;
                nb_px[n_nb_filtered] = npp->items[k].position[0];
                nb_py[n_nb_filtered] = npp->items[k].position[1];
                nb_pz[n_nb_filtered] = npp->items[k].position[2];
                n_nb_filtered++;
            }
        }

        // Group signal grids.
        sg_result_t gsg, nsg; memset(&gsg, 0, sizeof(gsg)); memset(&nsg, 0, sizeof(nsg));
        if (compute_signal_grids(P->normal, P->d, P->basis_e1, P->basis_e2,
                                  pd->group_x, pd->group_y, pd->group_z, pd->n_pts,
                                  params, &gsg) != 0) {
            free(nb_px); free(nb_py); free(nb_pz); goto out_fail;
        }
        // Nearby signal grids (optional — only if any filtered items).
        if (n_nb_filtered > 0) {
            if (compute_signal_grids(P->normal, P->d, P->basis_e1, P->basis_e2,
                                      nb_px, nb_py, nb_pz, n_nb_filtered,
                                      params, &nsg) != 0) {
                sg_result_free(&gsg);
                free(nb_px); free(nb_py); free(nb_pz); goto out_fail;
            }
        }
        free(nb_px); free(nb_py); free(nb_pz);

        // Move grid fields into the per-plane output bundle (transferring
        // ownership — sg_result keeps its `field` zeroed after move).
        PG->has_group_field = (gsg.field.n_cols > 0);
        PG->group_field = gsg.field;
        memset(&gsg.field, 0, sizeof(gsg.field));
        PG->has_nearby_field = (nsg.field.n_cols > 0);
        PG->nearby_field = nsg.field;
        memset(&nsg.field, 0, sizeof(nsg.field));

        // Balance field.
        float* bal_field = NULL;
        if (PG->has_group_field) {
            if (compute_balance_field(&PG->group_field,
                                       PG->has_nearby_field ? &PG->nearby_field : NULL,
                                       REF_COM_DIST, &bal_field) != 0) {
                free(gsg.candidates); free(nsg.candidates); goto out_fail;
            }
        }
        PG->bal_field = bal_field;
        PG->has_bal_field = (bal_field != NULL);

        // ── Grid-fill contour (topology source for boundary-polygons-2) ──
        // Density-weighted rasterise of group UV → marching-squares closed
        // contour. Adaptive cell size, no shrink (raw cell-edge midpoints).
        // Mirrors v2 boundary-signals.js (added 2026-05-22).
        //
        // Eligibility: admit planes with ≥3 real group points OR with at
        // least one owned void hull. The void-fill loop below injects
        // synthetic own-points inside the hull, so ghost-mostly planes
        // (real points sparse because the plane IS the optical-dropout
        // void) still get a usable contour from the synthetic samples.
        const v7_bs_plane_voids_t* pv = NULL;
        for (size_t qi = 0; qi < n_plane_voids; qi++) {
            if (plane_voids[qi].plane_id == P->id) { pv = &plane_voids[qi]; break; }
        }
        const bool has_owned_voids = (pv && pv->n_void_ids > 0 && n_voids > 0);
        if (pd->n_pts >= 3 || has_owned_voids) {
            // gc_pts holds real group points (and is realloc-extended below
            // with synth void points). malloc(0) is implementation-defined,
            // so request at least one slot for the ghost-mostly case.
            size_t alloc_n = pd->n_pts > 0 ? pd->n_pts : 1;
            v7_gc_point_t* gc_pts = (v7_gc_point_t*)malloc(alloc_n * sizeof(*gc_pts));
            if (gc_pts) {
                double minU = INFINITY, maxU = -INFINITY;
                double minV = INFINITY, maxV = -INFINITY;
                // Stand-in weight for synthetic void-fill points (idx >=
                // n_pts_local, no measured ts-weight): the MEAN Voronoi area
                // 1/density, not a bare 1.0 (which is ~21× too big for an
                // area-weighted raster). Guarded — density is validated below
                // for the contour path, but the loop runs first.
                const double void_fill_w =
                    (isfinite(params->lidar_density) && params->lidar_density > 0.0)
                        ? 1.0 / params->lidar_density : 1.0;
                for (uint32_t k = 0; k < pd->n_pts; k++) {
                    double u = pd->group_u[k], v = pd->group_v[k];
                    // Real point → its measured Voronoi area; synthetic
                    // void-fill point → the mean-area stand-in.
                    uint32_t idx = pd->group_indices[k];
                    double w = (idx < n_pts_local) ? (double)point_weights[idx] : void_fill_w;
                    gc_pts[k].u = u; gc_pts[k].v = v; gc_pts[k].weight = w;
                    if (u < minU) minU = u;
                    if (u > maxU) maxU = u;
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                }
                // Bbox check moved after void-fill so ghost-mostly planes
                // (n_pts==0 → bbox = ±INFINITY here) can still produce a
                // contour from synth void-fill points alone.
                if (params->compute_contour) {
                    v7_gc_bounds_t gb = { .min_u = minU, .max_u = maxU, .min_v = minV, .max_v = maxV };
                    v7_gc_opts_t go; v7_gc_opts_default(&go);
                    go.shrink = false;  // bp2 wants rough cell-edge topology
                    // No silent fallback — abort if caller asked for contour
                    // but didn't supply a valid density. NaN-sentinel default
                    // in params_default catches benches / step wrappers that
                    // forget to set it.
                    if (!isfinite(params->lidar_density) || params->lidar_density <= 0.0) {
                        fprintf(stderr, "[v7-boundary-signals] FATAL: compute_contour=true but lidar_density=%.6f — caller must supply a valid pts/m² (see v7-derive-thresholds)\n",
                                params->lidar_density);
                        free(gc_pts);
                        v7_bs_outputs_free(out);
                        return -1;
                    }
                    double spacing = 1.0 / sqrt(params->lidar_density);
                    // Nearby points (other-plane projections) — competing
                    // density grid sharpens the boundary at shared-edge
                    // zones. Calibrate nearby weight to the mean own-point
                    // weight so the per-cell contest is point-count-based,
                    // not weight-magnitude-based. Own carries Voronoi
                    // areas (~ 1/density per point, ≈ 0.05 at dahouet);
                    // a naïve nearby weight=1 makes nearby ~20× stronger
                    // per point and the boundary collapses inward
                    // wherever a neighbour exists.
                    v7_gc_point_t* nb_pts = NULL;
                    size_t n_nb = 0;
                    if (npp->n_items > 0) {
                        // Calibration baseline: mean own-point weight when
                        // real points exist; else 1/density (= what a synth
                        // void-fill point will carry — same magnitude as a
                        // real point at uniform `lidar_density`).
                        double mean_own_w;
                        if (pd->n_pts > 0) {
                            double sum_w = 0.0;
                            for (uint32_t k = 0; k < pd->n_pts; k++) sum_w += gc_pts[k].weight;
                            mean_own_w = sum_w / (double)pd->n_pts;
                        } else {
                            mean_own_w = 1.0 / params->lidar_density;
                        }
                        nb_pts = (v7_gc_point_t*)malloc(npp->n_items * sizeof(*nb_pts));
                        if (nb_pts) {
                            const double e1x_d = P->basis_e1[0], e1y_d = P->basis_e1[1], e1z_d = P->basis_e1[2];
                            const double e2x_d = P->basis_e2[0], e2y_d = P->basis_e2[1], e2z_d = P->basis_e2[2];
                            for (uint32_t k = 0; k < npp->n_items; k++) {
                                const double* p = npp->items[k].position;
                                nb_pts[n_nb].u = p[0]*e1x_d + p[1]*e1y_d + p[2]*e1z_d;
                                nb_pts[n_nb].v = p[0]*e2x_d + p[1]*e2y_d + p[2]*e2z_d;
                                nb_pts[n_nb].weight = mean_own_w;
                                n_nb++;
                            }
                        }
                    }
                    // ── Void-fill injection ─────────────────────────────
                    // For each dv2 void hull this plane owns, sample
                    // synthetic "own" points on a regular grid inside the
                    // hull at the expected LiDAR spacing. Each synth point
                    // carries weight = 1/density (= mean Voronoi area),
                    // so per-cell density boost equals what real own
                    // points at uniform `density` would contribute.
                    // Effect: cells inside the hull see own_density ≈
                    // perfect-coverage baseline → they win the
                    // `own > thr AND own > nearby` contest naturally,
                    // even where no LiDAR returns existed (optical
                    // dropouts). Hull edges get smoothed by Gaussian +
                    // ICM — no jaggy hull outline in the binary mask.
                    size_t n_void_synth = 0;
                    v7_gc_point_t* void_pts = NULL;
                    // `pv` + `has_owned_voids` were hoisted above the
                    // eligibility gate (so ghost-mostly planes can enter
                    // here on void-fill alone). Re-checking lidar_density
                    // is redundant — the FATAL above already aborted — but
                    // kept as a defensive guard.
                    if (has_owned_voids
                        && isfinite(params->lidar_density) && params->lidar_density > 0.0) {
                        const double synth_w = 1.0 / params->lidar_density;
                        // Two-pass: count, alloc, fill. Synth grid uses
                        // the natural sampling spacing (= 1/sqrt(density))
                        // — one synth point per "expected" point position.
                        const double e1x_d = P->basis_e1[0], e1y_d = P->basis_e1[1], e1z_d = P->basis_e1[2];
                        const double e2x_d = P->basis_e2[0], e2y_d = P->basis_e2[1], e2z_d = P->basis_e2[2];
                        // Project all hull verts of owned voids to plane UV
                        // (own + nearby live in this same UV — building-local
                        // projections through plane basis).
                        // Count + alloc.
                        size_t count_synth = 0;
                        for (uint32_t vi = 0; vi < pv->n_void_ids; vi++) {
                            const v7_bs_void_hull_t* H = NULL;
                            for (size_t hi = 0; hi < n_voids; hi++) {
                                if (voids[hi].void_id == pv->void_ids[vi]) { H = &voids[hi]; break; }
                            }
                            if (!H || H->n_hull_verts < 3) continue;
                            double hull_min_u = INFINITY, hull_max_u = -INFINITY;
                            double hull_min_v = INFINITY, hull_max_v = -INFINITY;
                            for (uint32_t k = 0; k < H->n_hull_verts; k++) {
                                const double* p = H->hull_xyz + (size_t)k * 3;
                                double u = p[0]*e1x_d + p[1]*e1y_d + p[2]*e1z_d;
                                double v = p[0]*e2x_d + p[1]*e2y_d + p[2]*e2z_d;
                                if (u < hull_min_u) hull_min_u = u;
                                if (u > hull_max_u) hull_max_u = u;
                                if (v < hull_min_v) hull_min_v = v;
                                if (v > hull_max_v) hull_max_v = v;
                            }
                            // Upper bound on synth points: bbox cells at spacing.
                            int nu = (int)ceil((hull_max_u - hull_min_u) / spacing) + 1;
                            int nv = (int)ceil((hull_max_v - hull_min_v) / spacing) + 1;
                            if (nu < 0 || nv < 0) continue;
                            count_synth += (size_t)nu * (size_t)nv;
                        }
                        if (count_synth > 0) {
                            void_pts = (v7_gc_point_t*)malloc(count_synth * sizeof(*void_pts));
                        }
                        if (void_pts) {
                            // Fill: project hull, walk synth grid, point-in-polygon test.
                            for (uint32_t vi = 0; vi < pv->n_void_ids; vi++) {
                                const v7_bs_void_hull_t* H = NULL;
                                for (size_t hi = 0; hi < n_voids; hi++) {
                                    if (voids[hi].void_id == pv->void_ids[vi]) { H = &voids[hi]; break; }
                                }
                                if (!H || H->n_hull_verts < 3) continue;
                                // Project hull to plane UV (heap because hull can grow).
                                double* hull_uv = (double*)malloc((size_t)H->n_hull_verts * 2 * sizeof(double));
                                if (!hull_uv) continue;
                                double hull_min_u = INFINITY, hull_max_u = -INFINITY;
                                double hull_min_v = INFINITY, hull_max_v = -INFINITY;
                                for (uint32_t k = 0; k < H->n_hull_verts; k++) {
                                    const double* p = H->hull_xyz + (size_t)k * 3;
                                    double u = p[0]*e1x_d + p[1]*e1y_d + p[2]*e1z_d;
                                    double v = p[0]*e2x_d + p[1]*e2y_d + p[2]*e2z_d;
                                    hull_uv[(size_t)k*2 + 0] = u;
                                    hull_uv[(size_t)k*2 + 1] = v;
                                    if (u < hull_min_u) hull_min_u = u;
                                    if (u > hull_max_u) hull_max_u = u;
                                    if (v < hull_min_v) hull_min_v = v;
                                    if (v > hull_max_v) hull_max_v = v;
                                }
                                // Synth grid walk + point-in-polygon (ray cast).
                                int nu = (int)ceil((hull_max_u - hull_min_u) / spacing) + 1;
                                int nv = (int)ceil((hull_max_v - hull_min_v) / spacing) + 1;
                                for (int ic = 0; ic < nu; ic++) for (int ir = 0; ir < nv; ir++) {
                                    double tu = hull_min_u + (ic + 0.5) * spacing;
                                    double tv = hull_min_v + (ir + 0.5) * spacing;
                                    // Standard ray-cast: count crossings of horizontal ray
                                    // from (tu,tv) to +∞ against hull edges.
                                    int crossings = 0;
                                    for (uint32_t e = 0; e < H->n_hull_verts; e++) {
                                        uint32_t en = (e + 1) % H->n_hull_verts;
                                        double au = hull_uv[(size_t)e *2 + 0], av = hull_uv[(size_t)e *2 + 1];
                                        double bu = hull_uv[(size_t)en*2 + 0], bv = hull_uv[(size_t)en*2 + 1];
                                        if ((av > tv) != (bv > tv)) {
                                            double x_isect = au + (tv - av) * (bu - au) / (bv - av);
                                            if (x_isect > tu) crossings++;
                                        }
                                    }
                                    if (crossings & 1) {
                                        void_pts[n_void_synth].u = tu;
                                        void_pts[n_void_synth].v = tv;
                                        void_pts[n_void_synth].weight = synth_w;
                                        n_void_synth++;
                                        // Extend contour bbox so synth points are inside.
                                        if (tu < minU) minU = tu;
                                        if (tu > maxU) maxU = tu;
                                        if (tv < minV) minV = tv;
                                        if (tv > maxV) maxV = tv;
                                    }
                                }
                                free(hull_uv);
                            }
                        }
                    }
                    // Append synth points to gc_pts (own array).
                    v7_gc_point_t* gc_pts_ext = gc_pts;
                    size_t n_pts_ext = pd->n_pts;
                    if (n_void_synth > 0) {
                        gc_pts_ext = (v7_gc_point_t*)realloc(gc_pts, (pd->n_pts + n_void_synth) * sizeof(*gc_pts));
                        if (gc_pts_ext) {
                            gc_pts = gc_pts_ext;
                            memcpy(gc_pts + pd->n_pts, void_pts, n_void_synth * sizeof(*void_pts));
                            n_pts_ext = pd->n_pts + n_void_synth;
                        }
                    }
                    free(void_pts);
                    // Update bbox in gb (bounds may have grown to include
                    // hulls). If neither real nor synth points contributed
                    // a usable bbox, skip the contour run — but still do
                    // the corner-validation work further down for this
                    // plane. Use a degenerate result so the contour-emit
                    // block below sees `n_pts < 3` and writes nothing.
                    v7_gc_result_t r; v7_gc_result_init(&r);
                    int gc_rc = -1;
                    if ((minU < maxU) && (minV < maxV) && n_pts_ext > 0) {
                        gb.min_u = minU; gb.max_u = maxU;
                        gb.min_v = minV; gb.max_v = maxV;
                        gc_rc = v7_gc_compute_adaptive(gc_pts, n_pts_ext, nb_pts, n_nb,
                                                       spacing, &gb, &go, &r, NULL, NULL);
                    }
                    free(nb_pts);
                    if (gc_rc == 0 && r.outer.n_pts >= 3) {
                        size_t ncell = (size_t)r.n_cols * r.n_rows;
                        PG->contour.has_grids   = true;
                        PG->contour.has_contour = true;
                        PG->contour.n_cols      = r.n_cols;
                        PG->contour.n_rows      = r.n_rows;
                        PG->contour.cell_size   = (float)r.cell_size;
                        PG->contour.g_min_u     = (float)r.g_min_u;
                        PG->contour.g_min_v     = (float)r.g_min_v;
                        PG->contour.max_density = (float)r.max_density;
                        PG->contour.threshold   = (float)r.threshold;
                        // Cast f64 grids to f32 for wire/RAM.
                        PG->contour.density  = (float*)malloc(ncell * sizeof(float));
                        PG->contour.smoothed = (float*)malloc(ncell * sizeof(float));
                        PG->contour.binary   = (uint8_t*)malloc(ncell);
                        if (PG->contour.density && PG->contour.smoothed && PG->contour.binary) {
                            for (size_t k = 0; k < ncell; k++) {
                                PG->contour.density[k]  = (float)r.density[k];
                                PG->contour.smoothed[k] = (float)r.smoothed[k];
                                PG->contour.binary[k]   = r.binary[k];
                            }
                        }
                        // Nearby grids — only when nearby_points were supplied
                        // (gc_compute leaves r.density_nearby NULL otherwise).
                        if (r.density_nearby && r.smoothed_nearby) {
                            PG->contour.has_nearby_grids = true;
                            PG->contour.density_nearby  = (float*)malloc(ncell * sizeof(float));
                            PG->contour.smoothed_nearby = (float*)malloc(ncell * sizeof(float));
                            if (PG->contour.density_nearby && PG->contour.smoothed_nearby) {
                                for (size_t k = 0; k < ncell; k++) {
                                    PG->contour.density_nearby[k]  = (float)r.density_nearby[k];
                                    PG->contour.smoothed_nearby[k] = (float)r.smoothed_nearby[k];
                                }
                            }
                        }
                        // Outer loop (f64 → f32).
                        PG->contour.outer.n_pts = r.outer.n_pts;
                        PG->contour.outer.pts = (float*)malloc(r.outer.n_pts * 2 * sizeof(float));
                        if (PG->contour.outer.pts) {
                            for (uint32_t k = 0; k < r.outer.n_pts; k++) {
                                PG->contour.outer.pts[k*2 + 0] = (float)r.outer.pts[k*2 + 0];
                                PG->contour.outer.pts[k*2 + 1] = (float)r.outer.pts[k*2 + 1];
                            }
                        }
                        // Holes (f64 → f32).
                        if (r.n_holes > 0) {
                            PG->contour.holes = (v7_bs_plane_contour_loop_t*)malloc(r.n_holes * sizeof(v7_bs_plane_contour_loop_t));
                            if (PG->contour.holes) {
                                PG->contour.n_holes = r.n_holes;
                                for (uint32_t h = 0; h < r.n_holes; h++) {
                                    PG->contour.holes[h].n_pts = r.holes[h].n_pts;
                                    PG->contour.holes[h].pts = (float*)malloc(r.holes[h].n_pts * 2 * sizeof(float));
                                    if (PG->contour.holes[h].pts) {
                                        for (uint32_t k = 0; k < r.holes[h].n_pts; k++) {
                                            PG->contour.holes[h].pts[k*2+0] = (float)r.holes[h].pts[k*2+0];
                                            PG->contour.holes[h].pts[k*2+1] = (float)r.holes[h].pts[k*2+1];
                                        }
                                    }
                                }
                            }
                        }
                    }
                    v7_gc_result_free(&r);
                }
                free(gc_pts);
            }
        }

        // Nearby bucket index for anchor validation.
        nearby_bucket_t nb_idx; memset(&nb_idx, 0, sizeof(nb_idx));
        if (npp->n_items > 0) {
            if (build_nearby_bucket(&nb_idx, npp->items, npp->n_items,
                                     P->basis_e1, P->basis_e2,
                                     (float)params->nearby_radius) != 0) {
                free(gsg.candidates); free(nsg.candidates); goto out_fail;
            }
        }

        // For each candidate (concave from group, convex from nearby):
        // validate and push to corner pool. dirsByPlane carries the two
        // outgoing edge directions for downstream regularization.
        for (int side = 0; side < 2; side++) {
            const sg_corner_cand_t* cands = (side == 0) ? gsg.candidates : nsg.candidates;
            uint32_t n_cands = (side == 0) ? gsg.n_candidates : nsg.n_candidates;
            uint8_t source = (side == 0) ? 4u /*concave*/ : 5u /*convex*/;
            for (uint32_t ci = 0; ci < n_cands; ci++) {
                const sg_corner_cand_t* c = &cands[ci];
                if (!anchor_valid(c, pd, &nb_idx,
                                   &PG->group_field, bal_field,
                                   P->basis_e1, P->basis_e2,
                                   params->anchor_valid_radius,
                                   params->anchor_balance_min)) {
                    continue;
                }
                // Reserve in corner pool.
                if (out->n_corners >= corners_cap) {
                    corners_cap = corners_cap ? corners_cap * 2 : 16;
                    v7_bs_corner_t* p = (v7_bs_corner_t*)realloc(out->corners,
                                                                  corners_cap * sizeof(*out->corners));
                    if (!p) { nearby_bucket_free(&nb_idx); free(gsg.candidates); free(nsg.candidates); goto out_fail; }
                    out->corners = p;
                }
                v7_bs_corner_t* C = &out->corners[out->n_corners];
                memset(C, 0, sizeof(*C));
                C->id = (uint32_t)out->n_corners;
                C->source = source;
                C->point[0] = c->center[0];
                C->point[1] = c->center[1];
                C->point[2] = c->center[2];
                C->n_merged = 1;
                C->plane_ids = (uint32_t*)malloc(sizeof(uint32_t));
                if (!C->plane_ids) { nearby_bucket_free(&nb_idx); free(gsg.candidates); free(nsg.candidates); goto out_fail; }
                C->plane_ids[0] = P->id;
                C->n_plane_ids = 1;
                // dirsByPlane: one entry with this plane's two outgoing edges.
                C->dirs = (v7_bs_corner_dir_t*)malloc(sizeof(*C->dirs));
                if (!C->dirs) { nearby_bucket_free(&nb_idx); free(gsg.candidates); free(nsg.candidates); goto out_fail; }
                C->dirs[0].plane_id = P->id;
                C->dirs[0].dir_a[0] = c->dir_a[0]; C->dirs[0].dir_a[1] = c->dir_a[1]; C->dirs[0].dir_a[2] = c->dir_a[2];
                C->dirs[0].dir_b[0] = c->dir_b[0]; C->dirs[0].dir_b[1] = c->dir_b[1]; C->dirs[0].dir_b[2] = c->dir_b[2];
                C->n_dirs = 1;
                // comBisectorByPlane: precompute the CoM-direction blended
                // bisector at the anchor's (u, v). Mirrors v2 boundary-
                // polygons.js D.4. Lookup both group + nearby outU/outV at
                // the anchor's cell in each grid; blend via (g - n) when
                // both present, falling back to single-side.
                double au = c->center[0]*P->basis_e1[0] + c->center[1]*P->basis_e1[1] + c->center[2]*P->basis_e1[2];
                double av = c->center[0]*P->basis_e2[0] + c->center[1]*P->basis_e2[1] + c->center[2]*P->basis_e2[2];
                float gU = NAN, gV = NAN, nU = NAN, nV = NAN;
                if (PG->has_group_field) {
                    const v7_bs_grid_field_t* F = &PG->group_field;
                    int col = (int)floor((au - (double)F->g_min_u) / (double)F->cell_size);
                    int row = (int)floor((av - (double)F->g_min_v) / (double)F->cell_size);
                    if (col >= 0 && (uint32_t)col < F->n_cols && row >= 0 && (uint32_t)row < F->n_rows) {
                        size_t idx = (size_t)row * F->n_cols + (size_t)col;
                        if (!isnan(F->oa_field[idx])) { gU = F->out_u[idx]; gV = F->out_v[idx]; }
                    }
                }
                if (PG->has_nearby_field) {
                    const v7_bs_grid_field_t* F = &PG->nearby_field;
                    int col = (int)floor((au - (double)F->g_min_u) / (double)F->cell_size);
                    int row = (int)floor((av - (double)F->g_min_v) / (double)F->cell_size);
                    if (col >= 0 && (uint32_t)col < F->n_cols && row >= 0 && (uint32_t)row < F->n_rows) {
                        size_t idx = (size_t)row * F->n_cols + (size_t)col;
                        if (!isnan(F->oa_field[idx])) { nU = F->out_u[idx]; nV = F->out_v[idx]; }
                    }
                }
                double bU = 0.0, bV = 0.0;
                bool has_bis = true;
                if (!isnan(gU) && !isnan(nU)) { bU = (double)gU - (double)nU; bV = (double)gV - (double)nV; }
                else if (!isnan(gU))          { bU = (double)gU;              bV = (double)gV; }
                else if (!isnan(nU))          { bU = -(double)nU;             bV = -(double)nV; }
                else has_bis = false;
                if (has_bis) {
                    double blen = sqrt(bU*bU + bV*bV);
                    if (blen < 1e-6) has_bis = false;
                    else { bU /= blen; bV /= blen; }
                }
                if (has_bis) {
                    C->com_bisectors = (v7_bs_corner_com_bisector_t*)malloc(sizeof(*C->com_bisectors));
                    if (!C->com_bisectors) { nearby_bucket_free(&nb_idx); free(gsg.candidates); free(nsg.candidates); goto out_fail; }
                    C->com_bisectors[0].plane_id = P->id;
                    C->com_bisectors[0].com_bisector[0] = (float)(bU * P->basis_e1[0] + bV * P->basis_e2[0]);
                    C->com_bisectors[0].com_bisector[1] = (float)(bU * P->basis_e1[1] + bV * P->basis_e2[1]);
                    C->com_bisectors[0].com_bisector[2] = (float)(bU * P->basis_e1[2] + bV * P->basis_e2[2]);
                    C->n_com_bisectors = 1;
                }
                // Name: P<planeId>C<n>  or  P<planeId>X<n>
                if (side == 0) {
                    C->name = corner_name_alloc("P%uC%u", (unsigned)P->id, ctrs.concave[pi]++);
                } else {
                    C->name = corner_name_alloc("P%uX%u", (unsigned)P->id, ctrs.convex[pi]++);
                }
                out->n_corners++;
            }
        }

        nearby_bucket_free(&nb_idx);
        free(gsg.candidates);
        free(nsg.candidates);
        alive_i++;
    }

    for (size_t j = 0; j < n_planes; j++) plane_data_free(&pds[j]);
    free(pds);
    free(ctrs.concave); free(ctrs.convex);
    return 0;

out_fail:
    for (size_t j = 0; j < n_planes; j++) free(accum[j].items);
    free(accum);
    for (size_t j = 0; j < n_planes; j++) plane_data_free(&pds[j]);
    free(pds);
    free(ctrs.concave); free(ctrs.convex);
    v7_bs_outputs_free(out);
    return -1;
}
