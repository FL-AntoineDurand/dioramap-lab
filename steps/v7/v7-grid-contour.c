// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-grid-contour — faithful C port of the v2 marching-squares utility.
// See v7-grid-contour.h for the algorithm overview.

#define _POSIX_C_SOURCE 200809L

#include "v7-grid-contour.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Defaults ────────────────────────────────────────────────────────

void v7_gc_opts_default(v7_gc_opts_t* o) {
    if (!o) return;
    o->threshold_ratio = 0.1;
    o->icm_iterations  = 10;
    o->margin_cells    = 4;
    o->shrink          = false;
}

void v7_gc_result_init(v7_gc_result_t* r) { memset(r, 0, sizeof(*r)); }

void v7_gc_result_free(v7_gc_result_t* r) {
    if (!r) return;
    free(r->density); free(r->smoothed); free(r->binary);
    free(r->density_nearby); free(r->smoothed_nearby);
    free(r->outer.pts);
    for (uint32_t i = 0; i < r->n_holes; i++) free(r->holes[i].pts);
    free(r->holes);
    memset(r, 0, sizeof(*r));
}

// ── Segment chaining via int64-keyed open-addressing hashmap ────────
//
// Mirrors the v2 JS chainAllSegments(): round each segment endpoint to a
// 1e-4 grid; vertices that round to the same cell are the same node.
// Walk first-unvisited-neighbour until loop closure (or dead end). Loops
// of < 3 vertices are dropped.

#define GC_EPS_INV 10000.0     // matches JS eps = 0.0001
#define GC_NB_MAX  6           // upper bound on neighbours per vertex

typedef struct {
    int64_t  packed_key;       // 0 = empty slot sentinel; bias by +1 to use
    uint32_t order_id;         // index into order[] of this vertex
} gc_hentry_t;

typedef struct {
    int64_t  packed_key;
    double   u, v;
    uint32_t nb[GC_NB_MAX];    // neighbour order_ids (in insertion order)
    uint8_t  n_nb;
} gc_vertex_t;

static int64_t gc_pack(double u, double v) {
    // Round half-to-even is what JS Math.round does for typical inputs;
    // round() in C is half-away-from-zero. The 1e-4 rounding granularity
    // is much coarser than the precision difference, so both schemes
    // map the same float to the same cell in practice.
    // Pack via uint64 to avoid signed shift UB when rv has the sign bit set.
    uint64_t ru = (uint32_t)(int32_t)round(u * GC_EPS_INV);
    uint64_t rv = (uint32_t)(int32_t)round(v * GC_EPS_INV);
    return (int64_t)(ru | (rv << 32));
}

// Unpackers — unused for now; verts cache u,v explicitly.

// linear-probe hashmap; size = 2× expected entries, rounded to power of 2.
static uint32_t gc_intern(gc_hentry_t* h, uint32_t hsize,
                           gc_vertex_t* verts, uint32_t* p_n_verts,
                           int64_t pkey, double u, double v) {
    uint64_t hsh = (uint64_t)pkey;
    hsh ^= hsh >> 33; hsh *= 0xff51afd7ed558ccdULL;
    hsh ^= hsh >> 33; hsh *= 0xc4ceb9fe1a85ec53ULL;
    hsh ^= hsh >> 33;
    uint32_t mask = hsize - 1;
    uint32_t slot = (uint32_t)hsh & mask;
    while (h[slot].packed_key != 0) {
        // Stored key is biased by +1 so 0 means empty.
        if (h[slot].packed_key - 1 == pkey) return h[slot].order_id;
        slot = (slot + 1) & mask;
    }
    uint32_t id = (*p_n_verts)++;
    h[slot].packed_key = pkey + 1;
    h[slot].order_id   = id;
    verts[id].packed_key = pkey;
    verts[id].u = u; verts[id].v = v;
    verts[id].n_nb = 0;
    return id;
}

static void gc_add_neighbour(gc_vertex_t* v, uint32_t nb) {
    if (v->n_nb >= GC_NB_MAX) return;
    // Match JS: push regardless of duplicate; the walker only checks visited.
    v->nb[v->n_nb++] = nb;
}

// Returns 0 on alloc fail. Loops are written into *out_loops (caller frees).
static int gc_chain_segments(const double* seg_xy /*[4 doubles per seg]*/, size_t n_segs,
                              v7_gc_loop_t** out_loops, uint32_t* out_n_loops) {
    *out_loops = NULL; *out_n_loops = 0;
    if (n_segs == 0) return 0;
    // Each segment has 2 endpoints → upper bound 2·n_segs distinct verts.
    uint32_t hsize = 16;
    while (hsize < n_segs * 4) hsize *= 2;
    gc_hentry_t* h = (gc_hentry_t*)calloc(hsize, sizeof(*h));
    gc_vertex_t* verts = (gc_vertex_t*)calloc(n_segs * 2, sizeof(*verts));
    if (!h || !verts) { free(h); free(verts); return -1; }
    uint32_t n_verts = 0;
    for (size_t i = 0; i < n_segs; i++) {
        double au = seg_xy[i*4 + 0], av = seg_xy[i*4 + 1];
        double bu = seg_xy[i*4 + 2], bv = seg_xy[i*4 + 3];
        int64_t ka = gc_pack(au, av), kb = gc_pack(bu, bv);
        uint32_t ia = gc_intern(h, hsize, verts, &n_verts, ka, au, av);
        uint32_t ib = gc_intern(h, hsize, verts, &n_verts, kb, bu, bv);
        gc_add_neighbour(&verts[ia], ib);
        gc_add_neighbour(&verts[ib], ia);
    }
    free(h);

    uint8_t* visited = (uint8_t*)calloc(n_verts, 1);
    if (!visited) { free(verts); return -1; }
    v7_gc_loop_t* loops = NULL;
    uint32_t n_loops = 0, cap_loops = 0;

    // Walk in insertion order — matches JS Map iteration.
    for (uint32_t start = 0; start < n_verts; start++) {
        if (visited[start]) continue;
        uint32_t* loc_seen = (uint32_t*)malloc(n_verts * sizeof(uint32_t));
        uint8_t*  in_loc   = (uint8_t*)calloc(n_verts, 1);
        if (!loc_seen || !in_loc) { free(loc_seen); free(in_loc); free(visited); free(verts); free(loops); return -1; }
        uint32_t n_loc = 0;
        uint32_t cur = start;
        while (cur != UINT32_MAX && !in_loc[cur]) {
            in_loc[cur] = 1; visited[cur] = 1;
            loc_seen[n_loc++] = cur;
            uint32_t next = UINT32_MAX;
            for (uint8_t k = 0; k < verts[cur].n_nb; k++) {
                uint32_t cand = verts[cur].nb[k];
                if (!in_loc[cand]) { next = cand; break; }
            }
            cur = next;
        }
        free(in_loc);
        if (n_loc >= 3) {
            if (n_loops == cap_loops) {
                cap_loops = cap_loops ? cap_loops * 2 : 8;
                v7_gc_loop_t* nl = (v7_gc_loop_t*)realloc(loops, cap_loops * sizeof(*loops));
                if (!nl) { free(loc_seen); free(visited); free(verts); free(loops); return -1; }
                loops = nl;
            }
            v7_gc_loop_t* L = &loops[n_loops++];
            L->n_pts = n_loc;
            L->pts = (double*)malloc(n_loc * 2 * sizeof(double));
            if (!L->pts) { free(loc_seen); free(visited); free(verts); for (uint32_t i = 0; i < n_loops; i++) free(loops[i].pts); free(loops); return -1; }
            for (uint32_t i = 0; i < n_loc; i++) {
                L->pts[i*2 + 0] = verts[loc_seen[i]].u;
                L->pts[i*2 + 1] = verts[loc_seen[i]].v;
            }
        }
        free(loc_seen);
    }
    free(visited); free(verts);

    // Sort loops by length desc (JS does this with .sort).
    for (uint32_t i = 1; i < n_loops; i++) {
        v7_gc_loop_t cur = loops[i];
        uint32_t j = i;
        while (j > 0 && loops[j-1].n_pts < cur.n_pts) { loops[j] = loops[j-1]; j--; }
        loops[j] = cur;
    }
    *out_loops = loops; *out_n_loops = n_loops;
    return 0;
}

// ── compute ─────────────────────────────────────────────────────────

// Rasterise + 2-pass 3×3 Gaussian smooth. Allocates dst grids; returns -1 on OOM.
// Used twice: once for own points (always), once for nearby (when supplied).
static int gc_rasterize_and_smooth(const v7_gc_point_t* pts, size_t n_pts,
                                    int64_t rows, int64_t cols,
                                    double g_min_u, double g_min_v, double cell_size,
                                    double** out_density, double** out_smoothed) {
    size_t n_cells = (size_t)cols * (size_t)rows;
    double* density  = (double*)calloc(n_cells, sizeof(double));
    double* tmp      = (double*)calloc(n_cells, sizeof(double));
    double* smoothed = (double*)calloc(n_cells, sizeof(double));
    if (!density || !tmp || !smoothed) {
        free(density); free(tmp); free(smoothed); return -1;
    }
    for (size_t i = 0; i < n_pts; i++) {
        double u = pts[i].u, v = pts[i].v;
        int64_t c = (int64_t)floor((u - g_min_u) / cell_size);
        int64_t r = (int64_t)floor((v - g_min_v) / cell_size);
        if (r < 0 || r >= rows || c < 0 || c >= cols) continue;
        density[r * cols + c] += pts[i].weight;
    }
    // Pass 1: density → tmp. Pass 2: tmp → smoothed.
    const double* src = density;
    double* dst = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int64_t r = 0; r < rows; r++) for (int64_t c = 0; c < cols; c++) {
            double s = 0, w = 0;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                int64_t nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                double wt = (dr == 0 && dc == 0) ? 4.0 : (dr == 0 || dc == 0) ? 2.0 : 1.0;
                s += src[nr * cols + nc] * wt;
                w += wt;
            }
            dst[r * cols + c] = s / w;
        }
        if (pass == 0) { src = dst; dst = smoothed; }
    }
    // After loop: dst (= smoothed) holds the second-pass result.
    free(tmp);
    *out_density = density;
    *out_smoothed = smoothed;
    return 0;
}

int v7_gc_compute(const v7_gc_point_t* points, size_t n_points,
                   const v7_gc_point_t* nearby_points, size_t n_nearby,
                   double cell_size,
                   const v7_gc_bounds_t* bounds,
                   const v7_gc_opts_t* in_opts,
                   v7_gc_result_t* out) {
    v7_gc_result_init(out);
    v7_gc_opts_t opts; v7_gc_opts_default(&opts);
    if (in_opts) opts = *in_opts;

    double margin = cell_size * (double)opts.margin_cells;
    double g_min_u = bounds->min_u - margin;
    double g_min_v = bounds->min_v - margin;
    int64_t cols = (int64_t)ceil((bounds->max_u + margin - g_min_u) / cell_size) + 1;
    int64_t rows = (int64_t)ceil((bounds->max_v + margin - g_min_v) / cell_size) + 1;
    if (cols <= 0 || rows <= 0) return +1;
    if ((uint64_t)cols * (uint64_t)rows > 500000ULL) return +1;

    size_t n_cells = (size_t)cols * (size_t)rows;

    // Steps 1-2: rasterise + smooth own (always) and nearby (when supplied).
    double* density = NULL;
    double* smoothed = NULL;
    if (gc_rasterize_and_smooth(points, n_points, rows, cols,
                                 g_min_u, g_min_v, cell_size,
                                 &density, &smoothed) != 0) {
        return -1;
    }
    double* density_nearby = NULL;
    double* smoothed_nearby = NULL;
    if (nearby_points && n_nearby > 0) {
        if (gc_rasterize_and_smooth(nearby_points, n_nearby, rows, cols,
                                     g_min_u, g_min_v, cell_size,
                                     &density_nearby, &smoothed_nearby) != 0) {
            free(density); free(smoothed);
            return -1;
        }
    }
    uint8_t* binary = (uint8_t*)calloc(n_cells, 1);
    if (!binary) {
        free(density); free(smoothed); free(density_nearby); free(smoothed_nearby);
        return -1;
    }

    // Step 3: binarise. Keep absolute threshold (prevents expansion into
    // truly empty cells) and add `own > nearby` comparison when available
    // — sharpens the boundary at shared-edge zones.
    double max_density = 0;
    for (size_t i = 0; i < n_cells; i++) if (smoothed[i] > max_density) max_density = smoothed[i];
    double threshold = max_density * opts.threshold_ratio;
    if (smoothed_nearby) {
        for (size_t i = 0; i < n_cells; i++) {
            binary[i] = (smoothed[i] > threshold && smoothed[i] > smoothed_nearby[i]) ? 1u : 0u;
        }
    } else {
        for (size_t i = 0; i < n_cells; i++) binary[i] = smoothed[i] > threshold ? 1u : 0u;
    }

    // Step 4: ICM. Tiebreak (n1 = 3-4) reuses the binarisation rule so
    // the morphology stays consistent with the per-cell contest.
    for (int it = 0; it < opts.icm_iterations; it++) {
        int changes = 0;
        for (int64_t r = 1; r < rows - 1; r++) for (int64_t c = 1; c < cols - 1; c++) {
            int64_t idx = r * cols + c;
            int n1 = 0;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                if (binary[(r + dr) * cols + (c + dc)]) n1++;
            }
            uint8_t tie = smoothed_nearby
                ? ((smoothed[idx] > threshold && smoothed[idx] > smoothed_nearby[idx]) ? 1u : 0u)
                : ((smoothed[idx] > threshold) ? 1u : 0u);
            uint8_t new_label = (n1 >= 5) ? 1u : (n1 <= 2) ? 0u : tie;
            if (binary[idx] != new_label) { binary[idx] = new_label; changes++; }
        }
        if (!changes) break;
    }

    // Step 5: marching squares.
    // Each cell with mixed corners emits 1 or 2 segments at edge midpoints.
    double* seg_xy = NULL;
    size_t cap_segs = 0, n_segs = 0;
    for (int64_t r = 0; r < rows - 1; r++) for (int64_t c = 0; c < cols - 1; c++) {
        uint8_t v00 = binary[r * cols + c],       v10 = binary[r * cols + (c + 1)];
        uint8_t v01 = binary[(r + 1) * cols + c], v11 = binary[(r + 1) * cols + (c + 1)];
        int ci = 0;
        if (v00) ci |= 1;
        if (v10) ci |= 2;
        if (v11) ci |= 4;
        if (v01) ci |= 8;
        if (ci == 0 || ci == 15) continue;
        double x0 = g_min_u + c * cell_size, x1 = g_min_u + (c + 1) * cell_size;
        double y0 = g_min_v + r * cell_size, y1 = g_min_v + (r + 1) * cell_size;
        double top[2]    = { (x0 + x1) * 0.5, y0 };
        double right[2]  = { x1, (y0 + y1) * 0.5 };
        double bottom[2] = { (x0 + x1) * 0.5, y1 };
        double left[2]   = { x0, (y0 + y1) * 0.5 };
        // (a, b) pairs per case — matches JS table line-for-line.
        const double* segs[4] = {NULL, NULL, NULL, NULL};
        int n_local = 0;
        #define ADD(a, b) do { segs[n_local*2] = (a); segs[n_local*2+1] = (b); n_local++; } while (0)
        switch (ci) {
            case 1:  ADD(left, top); break;
            case 2:  ADD(top, right); break;
            case 3:  ADD(left, right); break;
            case 4:  ADD(right, bottom); break;
            case 5:  ADD(left, top); ADD(right, bottom); break;
            case 6:  ADD(top, bottom); break;
            case 7:  ADD(left, bottom); break;
            case 8:  ADD(bottom, left); break;
            case 9:  ADD(bottom, top); break;
            case 10: ADD(top, left); ADD(bottom, right); break;
            case 11: ADD(bottom, right); break;
            case 12: ADD(right, left); break;
            case 13: ADD(right, top); break;
            case 14: ADD(top, left); break;
            default: break;
        }
        #undef ADD
        for (int k = 0; k < n_local; k++) {
            if (n_segs == cap_segs) {
                cap_segs = cap_segs ? cap_segs * 2 : 64;
                double* nb = (double*)realloc(seg_xy, cap_segs * 4 * sizeof(double));
                if (!nb) { free(seg_xy); free(density); free(smoothed); free(binary); return -1; }
                seg_xy = nb;
            }
            const double* a = segs[k * 2];
            const double* b = segs[k * 2 + 1];
            seg_xy[n_segs*4 + 0] = a[0]; seg_xy[n_segs*4 + 1] = a[1];
            seg_xy[n_segs*4 + 2] = b[0]; seg_xy[n_segs*4 + 3] = b[1];
            n_segs++;
        }
    }

    // Step 6: chain into closed loops.
    v7_gc_loop_t* loops = NULL;
    uint32_t n_loops = 0;
    int rc = gc_chain_segments(seg_xy, n_segs, &loops, &n_loops);
    free(seg_xy);
    if (rc != 0) { free(density); free(smoothed); free(binary); return -1; }

    // Step 7: shrink — skipped when opts.shrink == false (default in C).
    if (opts.shrink) {
        for (uint32_t li = 0; li < n_loops; li++) {
            v7_gc_loop_t* L = &loops[li];
            if (L->n_pts < 3) continue;
            uint32_t n = L->n_pts;
            double* offsets = (double*)malloc(n * 2 * sizeof(double));
            if (!offsets) continue;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t prev = (i + n - 1) % n;
                uint32_t next = (i + 1) % n;
                double tdx = L->pts[next*2] - L->pts[prev*2];
                double tdy = L->pts[next*2+1] - L->pts[prev*2+1];
                double tlen = hypot(tdx, tdy);
                if (tlen < 1e-10) { offsets[i*2] = 0; offsets[i*2+1] = 0; continue; }
                double nu = -tdy / tlen, nv = tdx / tlen;
                offsets[i*2]   = nu * cell_size * 0.5;
                offsets[i*2+1] = nv * cell_size * 0.5;
            }
            double area = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t j = (i + 1) % n;
                area += L->pts[i*2] * L->pts[j*2+1] - L->pts[j*2] * L->pts[i*2+1];
            }
            double sign = area > 0 ? 1.0 : -1.0;
            for (uint32_t i = 0; i < n; i++) {
                L->pts[i*2]   += offsets[i*2]   * sign;
                L->pts[i*2+1] += offsets[i*2+1] * sign;
            }
            free(offsets);
        }
    }

    // Pack into result. outer = loops[0]; holes = remainder with n_pts >= 4.
    out->n_cols = (uint32_t)cols;
    out->n_rows = (uint32_t)rows;
    out->cell_size = cell_size;
    out->g_min_u = g_min_u; out->g_min_v = g_min_v;
    out->max_density = max_density; out->threshold = threshold;
    out->density = density; out->smoothed = smoothed; out->binary = binary;
    out->density_nearby = density_nearby;     // NULL when no nearby supplied
    out->smoothed_nearby = smoothed_nearby;
    if (n_loops > 0) {
        out->outer = loops[0];
        // Holes: remaining loops with ≥4 pts (matches JS slice(1).filter(h.length>=4)).
        uint32_t n_holes_kept = 0;
        for (uint32_t i = 1; i < n_loops; i++) if (loops[i].n_pts >= 4) n_holes_kept++;
        if (n_holes_kept > 0) {
            out->holes = (v7_gc_loop_t*)malloc(n_holes_kept * sizeof(v7_gc_loop_t));
            if (!out->holes) { v7_gc_result_free(out); free(loops); return -1; }
            uint32_t k = 0;
            for (uint32_t i = 1; i < n_loops; i++) {
                if (loops[i].n_pts >= 4) out->holes[k++] = loops[i];
                else free(loops[i].pts);
            }
            out->n_holes = n_holes_kept;
        } else {
            for (uint32_t i = 1; i < n_loops; i++) free(loops[i].pts);
        }
    } else {
        out->outer.pts = NULL; out->outer.n_pts = 0;
    }
    free(loops);
    return 0;
}

// ── adaptive ────────────────────────────────────────────────────────

static double gc_loop_length(const v7_gc_loop_t* L) {
    if (!L || L->n_pts < 2) return 0;
    double len = 0;
    for (uint32_t i = 0; i < L->n_pts; i++) {
        uint32_t j = (i + 1) % L->n_pts;
        double du = L->pts[j*2] - L->pts[i*2];
        double dv = L->pts[j*2+1] - L->pts[i*2+1];
        len += hypot(du, dv);
    }
    return len;
}

int v7_gc_compute_adaptive(const v7_gc_point_t* points, size_t n_points,
                            const v7_gc_point_t* nearby_points, size_t n_nearby,
                            double spacing,
                            const v7_gc_bounds_t* bounds,
                            const v7_gc_opts_t* opts,
                            v7_gc_result_t* out,
                            v7_gc_trial_t** out_trials,
                            uint32_t* out_n_trials) {
    v7_gc_result_init(out);
    if (out_trials) *out_trials = NULL;
    if (out_n_trials) *out_n_trials = 0;

    const double start_mult = 0.5, step_mult = 0.1, max_mult = 2.0;
    // Trial count ceil((2.0 - 0.5)/0.1) + 1 = 16.
    enum { MAX_TRIALS = 16 };
    v7_gc_result_t results[MAX_TRIALS];
    v7_gc_trial_t  trials [MAX_TRIALS];
    int n_trials = 0;
    for (int i = 0; i < MAX_TRIALS; i++) v7_gc_result_init(&results[i]);

    for (double mult = start_mult; mult <= max_mult + 0.001 && n_trials < MAX_TRIALS; mult += step_mult) {
        double cs = spacing * mult;
        int rc = v7_gc_compute(points, n_points, nearby_points, n_nearby,
                                cs, bounds, opts, &results[n_trials]);
        if (rc < 0) {
            for (int k = 0; k <= n_trials; k++) v7_gc_result_free(&results[k]);
            return -1;
        }
        double len_raw = (rc == 0 && results[n_trials].outer.n_pts >= 3)
                          ? gc_loop_length(&results[n_trials].outer) : 0;
        // Match v2's Math.round(len * 100) / 100 — kept identical so the
        // stability comparison agrees with the JS reference at the
        // centimetre boundary near the 5% threshold.
        double len = round(len_raw * 100.0) / 100.0;
        // Count connected regions in the binary mask (BFS flood-fill, 4-neigh).
        // Mirrors v2 signals.js#L706-727: regions = topologically distinct
        // "alive" blobs after ICM cleanup. Used by Phase 2 below to skip
        // trials whose grid is still fragmented (multiple regions).
        uint32_t n_regions = 0;
        if (rc == 0 && results[n_trials].binary && results[n_trials].n_cols > 0 && results[n_trials].n_rows > 0) {
            uint32_t cols = results[n_trials].n_cols;
            uint32_t rows = results[n_trials].n_rows;
            uint8_t* bin = results[n_trials].binary;
            size_t ncells = (size_t)cols * rows;
            uint8_t* visited = (uint8_t*)calloc(ncells, 1);
            uint32_t* q = (uint32_t*)malloc(ncells * sizeof(uint32_t));
            if (visited && q) {
                for (uint32_t ri = 0; ri < rows; ri++) {
                    for (uint32_t ci = 0; ci < cols; ci++) {
                        uint32_t idx = ri * cols + ci;
                        if (!bin[idx] || visited[idx]) continue;
                        n_regions++;
                        uint32_t head = 0, tail = 0;
                        q[tail++] = idx; visited[idx] = 1;
                        while (head < tail) {
                            uint32_t cur = q[head++];
                            uint32_t cr = cur / cols, cc = cur % cols;
                            static const int dr[4] = {-1, 1, 0, 0};
                            static const int dc[4] = {0, 0, -1, 1};
                            for (int k = 0; k < 4; k++) {
                                int nr = (int)cr + dr[k], nc = (int)cc + dc[k];
                                if (nr < 0 || nc < 0 || nr >= (int)rows || nc >= (int)cols) continue;
                                uint32_t ni = (uint32_t)nr * cols + (uint32_t)nc;
                                if (bin[ni] && !visited[ni]) { visited[ni] = 1; q[tail++] = ni; }
                            }
                        }
                    }
                }
            }
            free(visited); free(q);
        }
        trials[n_trials].mult = round(mult * 100.0) / 100.0;
        trials[n_trials].cell_size = cs;
        trials[n_trials].contour_len = len;
        trials[n_trials].n_outer = results[n_trials].outer.n_pts;
        trials[n_trials].n_regions = n_regions;
        n_trials++;
    }

    // Phase 2: find startIdx = first trial whose nRegions hits its minimum
    // across all trials. v2 calls this "fragments have merged as much as
    // they can" — finer trials may still split the mask into fragments,
    // and we want to start the length-stability scan from the coarsest
    // trial that has the same minimum region count as anything coarser.
    uint32_t min_regions = UINT32_MAX;
    for (int i = 0; i < n_trials; i++) {
        if (trials[i].n_regions > 0 && trials[i].n_regions < min_regions) {
            min_regions = trials[i].n_regions;
        }
    }
    int start_idx = -1;
    if (min_regions != UINT32_MAX) {
        for (int i = 0; i < n_trials; i++) {
            if (trials[i].n_regions == min_regions) { start_idx = i; break; }
        }
    }
    if (start_idx < 0) start_idx = n_trials - 1;

    // Phase 3: from start_idx, pick the finest length-stable trial.
    int pick = -1;
    for (int i = start_idx; i < n_trials; i++) {
        if (trials[i].contour_len <= 0) continue;
        bool stable = true;
        for (int j = i + 1; j < n_trials; j++) {
            if (trials[j].contour_len <= 0) continue;
            // v2: drop = (trials[i].len - trials[j].len) / trials[i].len;
            //      if (drop > 0.05) stable = false;
            double drop = (trials[i].contour_len - trials[j].contour_len) / trials[i].contour_len;
            if (drop > 0.05) { stable = false; break; }
        }
        if (stable) { pick = i; break; }
    }
    // v2 default if no stable trial is found: pick the coarsest trial
    // (results.length - 1). We use the same fallback for parity.
    if (pick < 0) pick = n_trials - 1;
    if (pick < 0) {
        for (int i = 0; i < n_trials; i++) v7_gc_result_free(&results[i]);
        return +1;
    }
    // Move-out the chosen result; free the rest.
    *out = results[pick];
    memset(&results[pick], 0, sizeof(results[pick]));  // owned by out now
    for (int i = 0; i < n_trials; i++) v7_gc_result_free(&results[i]);

    if (out_trials) {
        v7_gc_trial_t* tb = (v7_gc_trial_t*)malloc(n_trials * sizeof(v7_gc_trial_t));
        if (tb) { memcpy(tb, trials, n_trials * sizeof(v7_gc_trial_t)); *out_trials = tb; }
    }
    if (out_n_trials) *out_n_trials = (uint32_t)n_trials;
    return 0;
}
