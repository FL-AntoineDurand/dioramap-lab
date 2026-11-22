// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-density-voids-2 — see header.

#include "v7-density-voids-2-impl.h"
#include "v3/safe_alloc.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "v7-mulberry32.h"
#include "v7-plane-fit.h"
#include "v7-ransac-plane.h"

void v7_dv2_params_default(v7_dv2_params_t* p) {
    p->cell_size           = 0.0;          // 0 → derive from density
    p->density             = 25.0;
    p->dist_threshold      = 0.09;         // = 4.5 × σ_z(0.02). Bake.
    p->occupied_min_count  = 2;
    p->core_min_empty      = 6;
    p->min_void_cells      = 8;
    p->rim_dilation        = 2;
    p->min_fit_pts         = 10;
    p->ransac_iterations   = 200;
    p->min_inliers         = 6;
    p->seed                = 42;
    p->min_normal_y        = 0.15;
    p->roof_tilt_min       = 0.20;
    p->roof_tilt_max       = 0.98;
    p->up_axis             = 1;            // bench frame for byte-cmp
    p->emit_viz_artifacts  = true;
}

void v7_dv2_outputs_init(v7_dv2_outputs_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

static void free_2d(v7_dv2_2d_t* v) {
    if (!v) return;
    free(v->counts);
    free(v->void_ids);
    free(v->rim_mask);
    free(v->voids);
    free(v->rim_xy);
    free(v->rim_void_ids);
    free(v->rim_inlier);
    free(v->rim_survivor);
    free(v->rim_dist);
    free(v->rim_filtered);
    free(v);
}

void v7_dv2_outputs_free(v7_dv2_outputs_t* out) {
    if (!out) return;
    if (out->ghost_planes) {
        for (size_t i = 0; i < out->n_ghost_planes; i++) {
            free(out->ghost_planes[i].inlier_indices);
            free(out->ghost_planes[i].hull_xyz);
        }
        free(out->ghost_planes);
    }
    free_2d(out->viz_2d);
    memset(out, 0, sizeof(*out));
}

// ── Frame helpers: pick the two horizontal axes for up_axis = 1 or 2. ──
//   up=1 (y-up): horiz = (x, z) = (0, 2)  — matches v2 XZ
//   up=2 (z-up): horiz = (x, y) = (0, 1)

static inline void horiz_axes(int up_axis, int* h0, int* h1) {
    if (up_axis == 1)      { *h0 = 0; *h1 = 2; }
    else if (up_axis == 2) { *h0 = 0; *h1 = 1; }
    else                   { *h0 = 1; *h1 = 2; }
}

static inline double abs_n_up(const float* normals, uint32_t gi, int up_axis) {
    return fabs((double)normals[gi * 3 + up_axis]);
}

// Lift a horizontal (h0, h1) point onto the plane along the up axis:
// solve n·p + d = 0 → p[up] = -(n[h0]·h0 + n[h1]·h1 + d) / n[up].
// Caller must guard n[up] ≠ 0.
static inline void lift_point_to_plane(double h0v, double h1v,
                                       const double n[3], double d,
                                       int h0, int h1, int up_axis,
                                       double out_xyz[3]) {
    out_xyz[h0]      = h0v;
    out_xyz[h1]      = h1v;
    out_xyz[up_axis] = -(n[h0] * h0v + n[h1] * h1v + d) / n[up_axis];
}

// ── Convex hull (monotone chain) on (h0, h1) pairs. ──
//   Output is a CCW (lower → upper) ring; returns the number of hull
//   verts written to out_h0/out_h1 (which must be sized >= n).

typedef struct hp { double a, b; uint32_t orig; } hp_t;

static int hp_cmp(const void* x, const void* y) {
    const hp_t* p = (const hp_t*)x;
    const hp_t* q = (const hp_t*)y;
    if (p->a < q->a) return -1;
    if (p->a > q->a) return  1;
    if (p->b < q->b) return -1;
    if (p->b > q->b) return  1;
    return 0;
}

static inline double cross_z(const hp_t* O, const hp_t* A, const hp_t* B) {
    return (A->a - O->a) * (B->b - O->b) - (A->b - O->b) * (B->a - O->a);
}

static size_t convex_hull_2d(const double* a_in, const double* b_in, size_t n,
                              double* out_a, double* out_b) {
    if (n <= 2) {
        for (size_t i = 0; i < n; i++) { out_a[i] = a_in[i]; out_b[i] = b_in[i]; }
        return n;
    }
    hp_t* sorted = (hp_t*)safe_malloc_array(n, sizeof(hp_t));
    if (!sorted) return 0;
    for (size_t i = 0; i < n; i++) {
        sorted[i].a = a_in[i]; sorted[i].b = b_in[i]; sorted[i].orig = (uint32_t)i;
    }
    qsort(sorted, n, sizeof(hp_t), hp_cmp);

    hp_t* lower = (hp_t*)safe_malloc_array(n, sizeof(hp_t));
    hp_t* upper = (hp_t*)safe_malloc_array(n, sizeof(hp_t));
    if (!lower || !upper) { free(sorted); free(lower); free(upper); return 0; }
    size_t L = 0, U = 0;
    for (size_t i = 0; i < n; i++) {
        while (L >= 2 && cross_z(&lower[L-2], &lower[L-1], &sorted[i]) <= 0) L--;
        lower[L++] = sorted[i];
    }
    for (size_t i = n; i-- > 0; ) {
        while (U >= 2 && cross_z(&upper[U-2], &upper[U-1], &sorted[i]) <= 0) U--;
        upper[U++] = sorted[i];
    }
    // v2: lower.pop(); upper.pop(); concat.
    if (L > 0) L--;
    if (U > 0) U--;

    size_t k = 0;
    for (size_t i = 0; i < L; i++) { out_a[k] = lower[i].a; out_b[k] = lower[i].b; k++; }
    for (size_t i = 0; i < U; i++) { out_a[k] = upper[i].a; out_b[k] = upper[i].b; k++; }

    free(sorted); free(lower); free(upper);
    return k;
}

// ── Compute ──

int v7_dv2_compute(const double* positions,
                   const float* normals,
                   const float* point_weights,
                   size_t n_pts,
                   const uint32_t* pool_indices,
                   size_t n_pool,
                   const v7_dv2_params_t* params,
                   v7_dv2_outputs_t* out) {
    if (!out || !positions || !pool_indices || !params) return -1;
    // point_weights is REQUIRED — see v7-plane-fit.h. No silent fallback.
    if (!point_weights) return -1;
    v7_dv2_outputs_init(out);

    // Param resolution.
    double cell_size = params->cell_size;
    if (cell_size <= 0.0) {
        const double spacing = 1.0 / sqrt(params->density);
        cell_size = spacing * 2.0;
        if (cell_size < 0.3) cell_size = 0.3;
    }
    const double dist_threshold = params->dist_threshold;
    const int    occ_min        = params->occupied_min_count;
    const int    core_min_empty = params->core_min_empty;
    const int    min_void_cells = params->min_void_cells;
    const int    rim_dilation   = params->rim_dilation < 1 ? 1 : params->rim_dilation;
    const int    min_fit_pts    = params->min_fit_pts;
    const int    ransac_iters   = params->ransac_iterations;
    const int    min_inliers    = params->min_inliers;
    const int    seed           = params->seed;
    const double min_normal_y   = params->min_normal_y;
    const double roof_tilt_min  = params->roof_tilt_min;
    const double roof_tilt_max  = params->roof_tilt_max;
    const int    up_axis        = params->up_axis;
    int h0, h1;
    horiz_axes(up_axis, &h0, &h1);

    if (n_pool == 0) return 0;

    // ── 1. Bounds ──
    double h0_min = INFINITY, h0_max = -INFINITY;
    double h1_min = INFINITY, h1_max = -INFINITY;
    for (size_t i = 0; i < n_pool; i++) {
        const double* p = positions + (size_t)pool_indices[i] * 3;
        if (p[h0] < h0_min) h0_min = p[h0];
        if (p[h0] > h0_max) h0_max = p[h0];
        if (p[h1] < h1_min) h1_min = p[h1];
        if (p[h1] > h1_max) h1_max = p[h1];
    }
    h0_min -= cell_size; h0_max += cell_size;
    h1_min -= cell_size; h1_max += cell_size;

    int cols = (int)ceil((h0_max - h0_min) / cell_size);
    int rows = (int)ceil((h1_max - h1_min) / cell_size);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    const size_t n_cells = (size_t)cols * (size_t)rows;

    // ── 2. Rasterize: counts + per-cell point lists (CSR). ──
    int32_t* counts = (int32_t*)safe_calloc_array(n_cells, sizeof(int32_t));
    if (!counts) return -1;
    // First pass: count.
    int* cell_of_pt = (int*)safe_malloc_array(n_pool, sizeof(int));
    if (!cell_of_pt) { free(counts); return -1; }
    for (size_t i = 0; i < n_pool; i++) {
        const double* p = positions + (size_t)pool_indices[i] * 3;
        int c = (int)floor((p[h0] - h0_min) / cell_size);
        int r = (int)floor((p[h1] - h1_min) / cell_size);
        if (c < 0) c = 0; else if (c >= cols) c = cols - 1;
        if (r < 0) r = 0; else if (r >= rows) r = rows - 1;
        const size_t k = (size_t)r * (size_t)cols + (size_t)c;
        cell_of_pt[i] = (int)k;
        counts[k]++;
    }
    // Build CSR offsets.
    size_t* cell_off = (size_t*)malloc((n_cells + 1) * sizeof(size_t));
    if (!cell_off) { free(counts); free(cell_of_pt); return -1; }
    cell_off[0] = 0;
    for (size_t k = 0; k < n_cells; k++) cell_off[k + 1] = cell_off[k] + (size_t)counts[k];
    uint32_t* cell_pts = (uint32_t*)safe_malloc_array(n_pool, sizeof(uint32_t));
    size_t*   cell_fill = (size_t*)safe_calloc_array(n_cells, sizeof(size_t));
    if (!cell_pts || !cell_fill) {
        free(counts); free(cell_of_pt); free(cell_off); free(cell_pts); free(cell_fill);
        return -1;
    }
    for (size_t i = 0; i < n_pool; i++) {
        const size_t k = (size_t)cell_of_pt[i];
        cell_pts[cell_off[k] + cell_fill[k]++] = pool_indices[i];
    }
    free(cell_of_pt); free(cell_fill);

    // ── 3. Occupancy + core cells. ──
    uint8_t* occupied = (uint8_t*)calloc(n_cells, 1);
    uint8_t* is_core  = (uint8_t*)calloc(n_cells, 1);
    if (!occupied || !is_core) {
        free(counts); free(cell_off); free(cell_pts); free(occupied); free(is_core);
        return -1;
    }
    for (size_t k = 0; k < n_cells; k++) if (counts[k] >= occ_min) occupied[k] = 1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const size_t k = (size_t)r * (size_t)cols + (size_t)c;
            if (occupied[k]) continue;
            int empty_nb = 0;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                if (dc == 0 && dr == 0) continue;
                const int nc = c + dc, nr = r + dr;
                if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) continue;
                if (!occupied[(size_t)nr * cols + (size_t)nc]) empty_nb++;
            }
            if (empty_nb >= core_min_empty) is_core[k] = 1;
        }
    }

    // ── 4. Core-anchored 4-connected flood fill. ──
    int32_t* raw_ids = (int32_t*)safe_malloc_array(n_cells, sizeof(int32_t));
    int32_t* comp_size = (int32_t*)safe_malloc_array(n_cells, sizeof(int32_t));  // upper bound
    int32_t* queue = (int32_t*)safe_malloc_array(n_cells, sizeof(int32_t));
    if (!raw_ids || !comp_size || !queue) {
        free(counts); free(cell_off); free(cell_pts);
        free(occupied); free(is_core); free(raw_ids); free(comp_size); free(queue);
        return -1;
    }
    for (size_t k = 0; k < n_cells; k++) raw_ids[k] = -1;
    int next_id = 0;
    for (int r0 = 0; r0 < rows; r0++) {
        for (int c0 = 0; c0 < cols; c0++) {
            const size_t k0 = (size_t)r0 * cols + (size_t)c0;
            if (!is_core[k0] || raw_ids[k0] >= 0) continue;
            // BFS via FIFO using head/tail indices.
            size_t head = 0, tail = 0;
            queue[tail++] = (int32_t)k0;
            raw_ids[k0] = next_id;
            int size = 1;
            const int CARDS[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
            while (head < tail) {
                const int kk = queue[head++];
                if (!is_core[kk]) continue;
                const int cr = kk / cols, cc = kk - cr * cols;
                for (int e = 0; e < 4; e++) {
                    const int nc = cc + CARDS[e][0], nr = cr + CARDS[e][1];
                    if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) continue;
                    const int nk = nr * cols + nc;
                    if (occupied[nk] || raw_ids[nk] >= 0) continue;
                    raw_ids[nk] = next_id;
                    size++;
                    queue[tail++] = nk;
                }
            }
            comp_size[next_id++] = size;
        }
    }
    free(queue);

    // ── 5. Prune small + reindex. ──
    int16_t* id_remap = (int16_t*)malloc((size_t)next_id * sizeof(int16_t));
    if (!id_remap) {
        free(counts); free(cell_off); free(cell_pts);
        free(occupied); free(is_core); free(raw_ids); free(comp_size);
        return -1;
    }
    int kept = 0;
    for (int i = 0; i < next_id; i++) {
        // Frama-C audit finding DV-1: cast `(int16_t)kept++` wraps for kept > 32767.
        // Past that the remap value goes negative and the downstream filter
        // treats it as "not a kept void" — silent peak loss. Cap at INT16_MAX.
        if (comp_size[i] >= min_void_cells && kept <= INT16_MAX) {
            id_remap[i] = (int16_t)kept++;
        } else {
            id_remap[i] = (int16_t)-1;
        }
    }
    int16_t* void_ids = (int16_t*)safe_malloc_array(n_cells, sizeof(int16_t));
    if (!void_ids) {
        free(counts); free(cell_off); free(cell_pts);
        free(occupied); free(is_core); free(raw_ids); free(comp_size); free(id_remap);
        return -1;
    }
    for (size_t k = 0; k < n_cells; k++) {
        const int32_t ri = raw_ids[k];
        void_ids[k] = (ri >= 0 && id_remap[ri] >= 0) ? id_remap[ri] : (int16_t)-1;
    }
    const int n_voids = kept;
    free(raw_ids); free(comp_size); free(id_remap);

    // ── 6. Rim extraction (Chebyshev dilation, min-dist tracking). ──
    uint8_t* rim_mask = (uint8_t*)calloc(n_cells, 1);
    uint8_t* rim_dist_cell = (uint8_t*)calloc(n_cells, 1);
    int16_t* rim_void_id_cell = (int16_t*)safe_malloc_array(n_cells, sizeof(int16_t));
    int32_t* void_cell_count = (int32_t*)calloc((size_t)n_voids, sizeof(int32_t));
    double*  void_sum_h0     = (double*)calloc((size_t)n_voids, sizeof(double));
    double*  void_sum_h1     = (double*)calloc((size_t)n_voids, sizeof(double));
    // Cell centres (for hull): up to n_cells / void.
    double** void_cell_centers_h0 = (double**)calloc((size_t)n_voids, sizeof(double*));
    double** void_cell_centers_h1 = (double**)calloc((size_t)n_voids, sizeof(double*));
    size_t*  void_cell_caps       = (size_t*)calloc((size_t)n_voids, sizeof(size_t));
    if (!rim_mask || !rim_dist_cell || !rim_void_id_cell
        || !void_cell_count || !void_sum_h0 || !void_sum_h1
        || !void_cell_centers_h0 || !void_cell_centers_h1 || !void_cell_caps) {
        goto oom;
    }
    for (size_t k = 0; k < n_cells; k++) rim_void_id_cell[k] = -1;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const size_t k = (size_t)r * cols + (size_t)c;
            const int16_t vid = void_ids[k];
            if (vid < 0) continue;
            const double cell_cx = h0_min + ((double)c + 0.5) * cell_size;
            const double cell_cz = h1_min + ((double)r + 0.5) * cell_size;
            void_cell_count[vid]++;
            void_sum_h0[vid] += cell_cx;
            void_sum_h1[vid] += cell_cz;

            // Append to per-void cell-center list (grow geometrically).
            if ((size_t)void_cell_count[vid] > void_cell_caps[vid]) {
                size_t nc = void_cell_caps[vid] ? void_cell_caps[vid] * 2 : 16;
                while (nc < (size_t)void_cell_count[vid]) nc *= 2;
                double* p0 = (double*)safe_realloc_array(void_cell_centers_h0[vid], nc, sizeof(double));
                double* p1 = (double*)safe_realloc_array(void_cell_centers_h1[vid], nc, sizeof(double));
                if (!p0 || !p1) { free(p0); free(p1); goto oom; }
                void_cell_centers_h0[vid] = p0;
                void_cell_centers_h1[vid] = p1;
                void_cell_caps[vid] = nc;
            }
            void_cell_centers_h0[vid][void_cell_count[vid] - 1] = cell_cx;
            void_cell_centers_h1[vid][void_cell_count[vid] - 1] = cell_cz;

            for (int dr = -rim_dilation; dr <= rim_dilation; dr++) {
                for (int dc = -rim_dilation; dc <= rim_dilation; dc++) {
                    if (dc == 0 && dr == 0) continue;
                    const int nc = c + dc, nr = r + dr;
                    if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) continue;
                    const size_t nk = (size_t)nr * cols + (size_t)nc;
                    if (!occupied[nk]) continue;
                    int cheb = dc < 0 ? -dc : dc;
                    if ((dr < 0 ? -dr : dr) > cheb) cheb = dr < 0 ? -dr : dr;
                    if (!rim_mask[nk] || (uint8_t)cheb < rim_dist_cell[nk]) {
                        rim_mask[nk] = 1;
                        rim_dist_cell[nk] = (uint8_t)cheb;
                        rim_void_id_cell[nk] = vid;
                    }
                }
            }
        }
    }

    // ── 7. Candidate pools (rim + interior survivors, normal pre-filter). ──
    // Pre-allocate worst-case sized scratch arrays.
    double*  rim_xy        = (double*)malloc(n_pool * 2 * sizeof(double));
    int16_t* rim_void_ids  = (int16_t*)safe_malloc_array(n_pool, sizeof(int16_t));
    uint8_t* rim_inlier    = (uint8_t*)calloc(n_pool, 1);
    uint8_t* rim_survivor  = (uint8_t*)malloc(n_pool);
    uint8_t* rim_dist_per  = (uint8_t*)malloc(n_pool);
    uint8_t* rim_filtered  = (uint8_t*)malloc(n_pool);
    uint32_t* rim_gis      = (uint32_t*)safe_malloc_array(n_pool, sizeof(uint32_t));
    int32_t* rim_pt_count  = (int32_t*)calloc((size_t)n_voids, sizeof(int32_t));
    int32_t* int_surv_count = (int32_t*)calloc((size_t)n_voids, sizeof(int32_t));
    // unowned-rim count = # rim points that passed the normal pre-filter,
    // regardless of dist (matches v2's outerCandidates - interiorSurvivors).
    // v3's inner_cands intentionally narrows to dist==1 for RANSAC seeding,
    // so we need a separate counter for the unownedRimPointCount stat.
    int32_t* rim_ok_count  = (int32_t*)calloc((size_t)n_voids, sizeof(int32_t));
    // Per-void inner/outer candidate lists (gi values).
    uint32_t** inner_cands = (uint32_t**)calloc((size_t)n_voids, sizeof(uint32_t*));
    size_t*    inner_caps  = (size_t*)calloc((size_t)n_voids, sizeof(size_t));
    size_t*    inner_lens  = (size_t*)calloc((size_t)n_voids, sizeof(size_t));
    if (!rim_xy || !rim_void_ids || !rim_inlier || !rim_survivor || !rim_dist_per
        || !rim_filtered || !rim_gis || !rim_pt_count || !int_surv_count
        || !inner_cands || !inner_caps || !inner_lens) {
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count);
        free(inner_cands); free(inner_caps); free(inner_lens);
        goto oom;
    }

    // Rim point gi → rim slot map (each gi appears in ≤ 1 rim slot since cells
    // are disjoint between rim/interior-survivor).
    int32_t* gi_to_rim_slot = (int32_t*)safe_malloc_array(n_pts, sizeof(int32_t));
    if (!gi_to_rim_slot) {
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count);
        free(inner_cands); free(inner_caps); free(inner_lens);
        goto oom;
    }
    for (size_t i = 0; i < n_pts; i++) gi_to_rim_slot[i] = -1;

    size_t n_rim = 0;
    int oom_flag = 0;

    // Helper to append a gi to inner_cands[v].
    #define INNER_PUSH(v, gi) do {                                              \
        const int _v = (v);                                                      \
        if (inner_lens[_v] == inner_caps[_v]) {                                  \
            size_t _nc = inner_caps[_v] ? inner_caps[_v] * 2 : 16;               \
            uint32_t* _p = (uint32_t*)safe_realloc_array(inner_cands[_v], _nc, sizeof(uint32_t)); \
            if (!_p) { oom_flag = 1; goto oom_post7; }                            \
            inner_cands[_v] = _p; inner_caps[_v] = _nc;                          \
        }                                                                        \
        inner_cands[_v][inner_lens[_v]++] = (gi);                                \
    } while (0)

    // Rim pass (rim_mask cells).
    for (size_t k = 0; k < n_cells; k++) {
        if (!rim_mask[k]) continue;
        const int16_t vid = rim_void_id_cell[k];
        const uint8_t dist = rim_dist_cell[k];
        const size_t pcount = (size_t)counts[k];
        const uint32_t* cellpts = cell_pts + cell_off[k];
        for (size_t j = 0; j < pcount; j++) {
            const uint32_t gi = cellpts[j];
            const double* p = positions + (size_t)gi * 3;
            rim_xy[2 * n_rim + 0] = p[h0];
            rim_xy[2 * n_rim + 1] = p[h1];
            rim_void_ids[n_rim] = vid;
            rim_survivor[n_rim] = 0;
            rim_dist_per[n_rim] = dist;
            const int ok = (normals && (roof_tilt_min > 0.0 || roof_tilt_max < 1.0))
                ? (abs_n_up(normals, gi, up_axis) >= roof_tilt_min
                   && abs_n_up(normals, gi, up_axis) <= roof_tilt_max)
                : 1;
            rim_filtered[n_rim] = ok ? 0 : 1;
            rim_gis[n_rim] = gi;
            gi_to_rim_slot[gi] = (int32_t)n_rim;
            if (vid >= 0) {
                rim_pt_count[vid]++;
                if (ok) rim_ok_count[vid]++;
                if (ok && dist == 1) INNER_PUSH(vid, gi);
            }
            n_rim++;
        }
    }
    // Interior survivors (cells inside voids with < occupiedMinCount points).
    for (size_t k = 0; k < n_cells; k++) {
        const int16_t vid = void_ids[k];
        if (vid < 0) continue;
        const size_t pcount = (size_t)counts[k];
        if (pcount == 0) continue;
        const uint32_t* cellpts = cell_pts + cell_off[k];
        for (size_t j = 0; j < pcount; j++) {
            const uint32_t gi = cellpts[j];
            const double* p = positions + (size_t)gi * 3;
            rim_xy[2 * n_rim + 0] = p[h0];
            rim_xy[2 * n_rim + 1] = p[h1];
            rim_void_ids[n_rim] = vid;
            rim_survivor[n_rim] = 1;
            rim_dist_per[n_rim] = 0;
            const int ok = (normals && (roof_tilt_min > 0.0 || roof_tilt_max < 1.0))
                ? (abs_n_up(normals, gi, up_axis) >= roof_tilt_min
                   && abs_n_up(normals, gi, up_axis) <= roof_tilt_max)
                : 1;
            rim_filtered[n_rim] = ok ? 0 : 1;
            rim_gis[n_rim] = gi;
            gi_to_rim_slot[gi] = (int32_t)n_rim;
            if (ok) {
                int_surv_count[vid]++;
                INNER_PUSH(vid, gi);
            }
            n_rim++;
        }
    }
    #undef INNER_PUSH
oom_post7:
    if (oom_flag) {
        for (int v = 0; v < n_voids; v++) free(inner_cands[v]);
        free(inner_cands); free(inner_caps); free(inner_lens);
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
        goto oom;
    }

    // ── 8. RANSAC per void on inner pool. ──
    v7_rng_t rng; v7_rng_init(&rng, seed);

    // Per-void fit (NULL = no fit).
    typedef struct fit_rec {
        bool   has;
        double normal[3];
        double d;
        double rms;
        double tilt;
        double azimuth;
        uint32_t n_candidates;
        uint32_t n_inliers;
        uint32_t* inlier_gi;       // owned
    } fit_rec_t;
    fit_rec_t* fits = (fit_rec_t*)calloc((size_t)n_voids, sizeof(fit_rec_t));
    if (!fits) {
        for (int v = 0; v < n_voids; v++) free(inner_cands[v]);
        free(inner_cands); free(inner_caps); free(inner_lens);
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
        goto oom;
    }

    for (int v = 0; v < n_voids; v++) {
        const size_t n_inner = inner_lens[v];
        if ((int)n_inner < min_fit_pts) continue;
        v7_ransac_result_t rs;
        if (v7_ransac_plane(positions, inner_cands[v], n_inner,
                             ransac_iters, dist_threshold, &rng, &rs) != 0
            || (int)rs.n_inliers < min_inliers) {
            v7_ransac_result_free(&rs);
            continue;
        }
        // Map RANSAC's local indices → global gi.
        uint32_t* inlier_gi = (uint32_t*)safe_malloc_array(rs.n_inliers, sizeof(uint32_t));
        if (!inlier_gi) {
            v7_ransac_result_free(&rs);
            for (int u = 0; u <= v; u++) free(fits[u].inlier_gi);
            free(fits);
            for (int u = 0; u < n_voids; u++) free(inner_cands[u]);
            free(inner_cands); free(inner_caps); free(inner_lens);
            free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
            free(rim_dist_per); free(rim_filtered); free(rim_gis);
            free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
            goto oom;
        }
        for (size_t i = 0; i < rs.n_inliers; i++) {
            inlier_gi[i] = inner_cands[v][rs.inlier_indices[i]];
        }
        // LSQ refit on inliers (keep RANSAC plane on degenerate refit).
        v7_plane_fit_t pf;
        double n_use[3]; double d_use; double rms_use = 0.0;
        if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), inlier_gi, rs.n_inliers, up_axis, &pf) == 0) {
            n_use[0] = pf.normal[0]; n_use[1] = pf.normal[1]; n_use[2] = pf.normal[2];
            d_use = pf.d; rms_use = pf.rms;
        } else {
            n_use[0] = rs.normal[0]; n_use[1] = rs.normal[1]; n_use[2] = rs.normal[2];
            d_use = rs.d;
        }
        fits[v].has = true;
        fits[v].normal[0] = n_use[0]; fits[v].normal[1] = n_use[1]; fits[v].normal[2] = n_use[2];
        fits[v].d = d_use; fits[v].rms = rms_use;
        fits[v].tilt = v7_tilt_from_normal(n_use, up_axis);
        fits[v].azimuth = v7_azimuth_from_normal(n_use, up_axis);
        fits[v].n_candidates = (uint32_t)n_inner;
        fits[v].n_inliers = (uint32_t)rs.n_inliers;
        fits[v].inlier_gi = inlier_gi;
        // Mark rim_inlier slots.
        for (size_t i = 0; i < rs.n_inliers; i++) {
            const int32_t slot = gi_to_rim_slot[inlier_gi[i]];
            if (slot >= 0) rim_inlier[slot] = 1;
        }
        v7_ransac_result_free(&rs);
    }

    // ── 9. Build ghost-planes-2 ──
    const double cell_area = cell_size * cell_size;
    size_t ng = 0;
    for (int v = 0; v < n_voids; v++) {
        if (!fits[v].has) continue;
        if (fabs(fits[v].normal[up_axis]) < min_normal_y) continue;
        ng++;
    }
    v7_dv2_ghost_plane_t* ghosts = ng ? (v7_dv2_ghost_plane_t*)safe_calloc_array(ng, sizeof(v7_dv2_ghost_plane_t)) : NULL;
    if (ng && !ghosts) {
        for (int v = 0; v < n_voids; v++) free(fits[v].inlier_gi);
        free(fits);
        for (int v = 0; v < n_voids; v++) free(inner_cands[v]);
        free(inner_cands); free(inner_caps); free(inner_lens);
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
        goto oom;
    }
    size_t gi_out = 0;
    for (int v = 0; v < n_voids; v++) {
        if (!fits[v].has) continue;
        if (fabs(fits[v].normal[up_axis]) < min_normal_y) continue;

        const size_t n_cell = (size_t)void_cell_count[v];
        double* hull_a = (double*)safe_malloc_array(n_cell, sizeof(double));
        double* hull_b = (double*)safe_malloc_array(n_cell, sizeof(double));
        if (!hull_a || !hull_b) { free(hull_a); free(hull_b); goto post9_oom; }
        size_t n_hull = convex_hull_2d(void_cell_centers_h0[v],
                                        void_cell_centers_h1[v], n_cell,
                                        hull_a, hull_b);
        // Lift each hull vert to 3D on the plane.
        double* hull3d = (double*)malloc(n_hull * 3 * sizeof(double));
        if (!hull3d) { free(hull_a); free(hull_b); goto post9_oom; }
        for (size_t i = 0; i < n_hull; i++) {
            double xyz[3];
            lift_point_to_plane(hull_a[i], hull_b[i],
                                 fits[v].normal, fits[v].d,
                                 h0, h1, up_axis, xyz);
            hull3d[3 * i + 0] = xyz[0];
            hull3d[3 * i + 1] = xyz[1];
            hull3d[3 * i + 2] = xyz[2];
        }
        free(hull_a); free(hull_b);

        const double cx = void_sum_h0[v] / (double)n_cell;
        const double cz = void_sum_h1[v] / (double)n_cell;
        double centroid[3];
        lift_point_to_plane(cx, cz, fits[v].normal, fits[v].d, h0, h1, up_axis, centroid);

        v7_dv2_ghost_plane_t* g = &ghosts[gi_out++];
        g->normal[0] = fits[v].normal[0];
        g->normal[1] = fits[v].normal[1];
        g->normal[2] = fits[v].normal[2];
        g->d         = fits[v].d;
        g->tilt      = fits[v].tilt;
        g->azimuth   = fits[v].azimuth;
        g->rms       = fits[v].rms;
        g->centroid[0] = centroid[0];
        g->centroid[1] = centroid[1];
        g->centroid[2] = centroid[2];
        g->area      = (double)n_cell * cell_area;
        g->void_id   = (uint32_t)v;
        g->inlier_count   = fits[v].n_inliers;
        g->hull_vert_count = (uint32_t)n_hull;
        g->inlier_indices = fits[v].inlier_gi;   // transfer ownership
        fits[v].inlier_gi = NULL;
        g->hull_xyz       = hull3d;
        continue;

post9_oom:
        for (size_t i = 0; i < gi_out; i++) {
            free(ghosts[i].inlier_indices);
            free(ghosts[i].hull_xyz);
        }
        free(ghosts);
        for (int u = 0; u < n_voids; u++) free(fits[u].inlier_gi);
        free(fits);
        for (int u = 0; u < n_voids; u++) free(inner_cands[u]);
        free(inner_cands); free(inner_caps); free(inner_lens);
        free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
        free(rim_dist_per); free(rim_filtered); free(rim_gis);
        free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
        goto oom;
    }
    out->ghost_planes = ghosts;
    out->n_ghost_planes = gi_out;

    // ── Build viz blob (gated). ──
    if (params->emit_viz_artifacts) {
        v7_dv2_2d_t* viz = (v7_dv2_2d_t*)calloc(1, sizeof(*viz));
        if (!viz) goto skip_viz;
        viz->bounds[0] = h0_min; viz->bounds[1] = h0_max;
        viz->bounds[2] = h1_min; viz->bounds[3] = h1_max;
        viz->cell_size = cell_size;
        viz->cols = cols; viz->rows = rows;
        viz->rim_dilation = rim_dilation;
        viz->n_points = (uint32_t)n_pool;
        viz->n_voids = (uint32_t)n_voids;
        viz->counts        = (int32_t*)safe_malloc_array(n_cells, sizeof(int32_t));
        viz->void_ids      = (int16_t*)safe_malloc_array(n_cells, sizeof(int16_t));
        viz->rim_mask      = (uint8_t*)malloc(n_cells);
        viz->voids         = n_voids ? (v7_dv2_void_summary_t*)calloc((size_t)n_voids, sizeof(v7_dv2_void_summary_t)) : NULL;
        if (n_rim > 0) {
            viz->rim_xy        = (double*)malloc(n_rim * 2 * sizeof(double));
            viz->rim_void_ids  = (int16_t*)safe_malloc_array(n_rim, sizeof(int16_t));
            viz->rim_inlier    = (uint8_t*)malloc(n_rim);
            viz->rim_survivor  = (uint8_t*)malloc(n_rim);
            viz->rim_dist      = (uint8_t*)malloc(n_rim);
            viz->rim_filtered  = (uint8_t*)malloc(n_rim);
        }
        viz->n_rim = (uint32_t)n_rim;
        if (!viz->counts || !viz->void_ids || !viz->rim_mask
            || (n_voids && !viz->voids)
            || (n_rim && (!viz->rim_xy || !viz->rim_void_ids || !viz->rim_inlier
                          || !viz->rim_survivor || !viz->rim_dist || !viz->rim_filtered))) {
            free_2d(viz); viz = NULL; goto skip_viz;
        }
        memcpy(viz->counts,   counts,    n_cells * sizeof(int32_t));
        memcpy(viz->void_ids, void_ids,  n_cells * sizeof(int16_t));
        memcpy(viz->rim_mask, rim_mask,  n_cells);
        if (n_rim) {
            memcpy(viz->rim_xy,       rim_xy,       n_rim * 2 * sizeof(double));
            memcpy(viz->rim_void_ids, rim_void_ids, n_rim * sizeof(int16_t));
            memcpy(viz->rim_inlier,   rim_inlier,   n_rim);
            memcpy(viz->rim_survivor, rim_survivor, n_rim);
            memcpy(viz->rim_dist,     rim_dist_per, n_rim);
            memcpy(viz->rim_filtered, rim_filtered, n_rim);
        }
        for (int v = 0; v < n_voids; v++) {
            v7_dv2_void_summary_t* s = &viz->voids[v];
            s->cell_count = (uint32_t)void_cell_count[v];
            s->rim_point_count = (uint32_t)rim_pt_count[v];
            // unownedRim = rim points that passed the normal pre-filter.
            // Matches v2's outerCandidates[v].length - interiorSurvivorCount[v].
            s->unowned_rim_point_count = (uint32_t)rim_ok_count[v];
            s->interior_survivor_count = (uint32_t)int_surv_count[v];
            s->area = (double)void_cell_count[v] * cell_area;
            s->cx = void_sum_h0[v] / (double)void_cell_count[v];
            s->cz = void_sum_h1[v] / (double)void_cell_count[v];
            if (fits[v].has) {
                s->has_fit = true;
                s->fit_normal[0] = fits[v].normal[0];
                s->fit_normal[1] = fits[v].normal[1];
                s->fit_normal[2] = fits[v].normal[2];
                s->fit_d   = fits[v].d;
                s->fit_rms = fits[v].rms;
                s->fit_tilt = fits[v].tilt;
                s->fit_azimuth = fits[v].azimuth;
                s->fit_n_candidates = fits[v].n_candidates;
                s->fit_n_inliers    = fits[v].n_inliers;
            }
        }
        out->viz_2d = viz;
    }
skip_viz:

    // ── Cleanup scratch. ──
    for (int v = 0; v < n_voids; v++) {
        free(fits[v].inlier_gi);
        free(inner_cands[v]);
        free(void_cell_centers_h0[v]);
        free(void_cell_centers_h1[v]);
    }
    free(fits);
    free(inner_cands); free(inner_caps); free(inner_lens);
    free(void_cell_centers_h0); free(void_cell_centers_h1); free(void_cell_caps);
    free(rim_xy); free(rim_void_ids); free(rim_inlier); free(rim_survivor);
    free(rim_dist_per); free(rim_filtered); free(rim_gis);
    free(rim_pt_count); free(int_surv_count); free(rim_ok_count); free(gi_to_rim_slot);
    free(rim_mask); free(rim_dist_cell); free(rim_void_id_cell);
    free(void_cell_count); free(void_sum_h0); free(void_sum_h1);
    free(void_ids); free(occupied); free(is_core);
    free(counts); free(cell_off); free(cell_pts);
    return 0;

oom:
    free(rim_mask); free(rim_dist_cell); free(rim_void_id_cell);
    free(void_cell_count); free(void_sum_h0); free(void_sum_h1);
    if (void_cell_centers_h0) for (int v = 0; v < n_voids; v++) free(void_cell_centers_h0[v]);
    if (void_cell_centers_h1) for (int v = 0; v < n_voids; v++) free(void_cell_centers_h1[v]);
    free(void_cell_centers_h0); free(void_cell_centers_h1); free(void_cell_caps);
    free(void_ids); free(occupied); free(is_core);
    free(counts); free(cell_off); free(cell_pts);
    v7_dv2_outputs_free(out);
    return -1;
}
