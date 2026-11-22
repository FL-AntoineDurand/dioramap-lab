// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-hemisphere-segment — pure compute. Direct port of bench-v2
// hemisphere-normals-density-aware-segment.js (meta.version=5).

#include "v7-hemisphere-segment-impl.h"
#include "v3/safe_alloc.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v3/blob.h"
#include "v7-histogram.h"
#include "v7-lambert.h"

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

void v7_hs_params_default(v7_hs_params_t* p) {
    p->lambert_resolution = 128;
    p->lambert_smooth_sigma = 2.0;
    p->lambert_min_basin_area = 0.5;
    p->stage2_iqr_bandwidth = 0.0;       // 0 = Silverman, 1 = IQR
    p->kde_n_eval = 200;
    p->connect_radius_scale = 3.0;
    p->connect_radius_max = 2.0;
    p->min_segment_area = 1.0;
    p->up_axis = 1;                      // y-up (bench-v2 default)
    p->emit_viz_artifacts = true;
}

void v7_hs_outputs_init(v7_hs_outputs_t* out) {
    memset(out, 0, sizeof(*out));
}

void v7_hs_outputs_free(v7_hs_outputs_t* out) {
    if (!out) return;
    free(out->cascade_segments);
    free(out->ridge_candidates);
    free(out->owner);
    if (out->segments) {
        for (size_t i = 0; i < out->n_segments; i++) free(out->segments[i].pt_indices);
        free(out->segments);
    }
    free(out->lambert_grid);
    free(out->lambert_peaks);
    free(out->offset_kde_blob);
    memset(out, 0, sizeof(*out));
}

// ── Dynamic-array helper for accumulating per-group point indices ──

typedef struct uint_vec {
    uint32_t* data;
    size_t len;
    size_t cap;
} uint_vec_t;

static int uvec_push(uint_vec_t* v, uint32_t x) {
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 16;
        uint32_t* p = (uint32_t*)safe_realloc_array(v->data, nc, sizeof(uint32_t));
        if (!p) return -1;
        v->data = p;
        v->cap = nc;
    }
    v->data[v->len++] = x;
    return 0;
}

static void uvec_free(uint_vec_t* v) {
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}


// ── S3: radius-graph union-find connected components ──

typedef struct uf {
    int32_t* parent;
    int32_t* rank;
    int32_t n;
} uf_t;

static int uf_init(uf_t* u, int32_t n) {
    u->parent = (int32_t*)malloc((size_t)n * sizeof(int32_t));
    u->rank = (int32_t*)calloc((size_t)n, sizeof(int32_t));
    if (!u->parent || !u->rank) {
        free(u->parent); free(u->rank);
        u->parent = NULL; u->rank = NULL;
        return -1;
    }
    for (int32_t i = 0; i < n; i++) u->parent[i] = i;
    u->n = n;
    return 0;
}

static void uf_free(uf_t* u) {
    free(u->parent); free(u->rank);
    u->parent = NULL; u->rank = NULL;
    u->n = 0;
}

static int32_t uf_find(uf_t* u, int32_t x) {
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]];
        x = u->parent[x];
    }
    return x;
}

static void uf_union(uf_t* u, int32_t a, int32_t b) {
    a = uf_find(u, a); b = uf_find(u, b);
    if (a == b) return;
    if (u->rank[a] < u->rank[b]) { int32_t t = a; a = b; b = t; }
    u->parent[b] = a;
    if (u->rank[a] == u->rank[b]) u->rank[a]++;
}

// ── Radius search via kd-tree, recursive walk ──
//
// Match bench-v2's radiusSearch: visit near subtree first, prune far if
// diff² > radius². Returns a list of (idx, dist_sq) — caller-owned.

typedef struct rs_state {
    const kd_tree_t* tree;
    double qx, qy, qz;
    double radius_sq;
    int32_t* out_indices;
    size_t out_count;
    size_t out_cap;
} rs_state_t;

static int rs_push(rs_state_t* s, int32_t idx) {
    if (s->out_count == s->out_cap) {
        size_t nc = s->out_cap ? s->out_cap * 2 : 32;
        int32_t* p = (int32_t*)safe_realloc_array(s->out_indices, nc, sizeof(int32_t));
        if (!p) return -1;
        s->out_indices = p;
        s->out_cap = nc;
    }
    s->out_indices[s->out_count++] = idx;
    return 0;
}

static void rs_search(rs_state_t* s, int32_t node_id) {
    if (node_id < 0) return;
    const kd_node_t* node = &s->tree->nodes[node_id];
    const double* p = &s->tree->positions[node->point_idx * 3];
    double dx = p[0] - s->qx, dy = p[1] - s->qy, dz = p[2] - s->qz;
    double dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq <= s->radius_sq) rs_push(s, node->point_idx);

    double q_axis;
    switch (node->axis) {
        case 0: q_axis = s->qx; break;
        case 1: q_axis = s->qy; break;
        default: q_axis = s->qz; break;
    }
    double diff = q_axis - node->split_val;
    int32_t near = (diff < 0) ? node->left : node->right;
    int32_t far = (diff < 0) ? node->right : node->left;
    rs_search(s, near);
    if (diff * diff <= s->radius_sq) rs_search(s, far);
}

// ── Main compute ──

int v7_hs_compute(const double* positions,
                  const float* normals,
                  const float* point_weights,
                  size_t n_pts,
                  const kd_tree_t* tree,
                  const int16_t* owner_in,
                  const v7_hs_params_t* params,
                  v7_hs_outputs_t* out) {
    v7_hs_outputs_free(out);
    // Empty input is a legitimate trivial-success case (no work, no
    // allocations, no weights needed) — returns 0 with all-zero output.
    // The unit test test_empty_input_handled relies on this fast path,
    // and an empty caller (e.g. building with 0 unowned points) is
    // semantically valid. Must come BEFORE the strict weights check.
    if (n_pts == 0) { out->n_pts = 0; return 0; }
    // point_weights is REQUIRED for the main path — see v7-plane-fit.h
    // policy. No silent fallback to uniform-1.0 (would zero out density-
    // bias correction throughout cluster-area + group-area + comp-area
    // accumulators).
    if (!point_weights) return -1;
    out->n_pts = n_pts;
    out->cascade_segments = (int16_t*)safe_malloc_array(n_pts, sizeof(int16_t));
    out->ridge_candidates = (uint8_t*)safe_calloc_array(n_pts, sizeof(uint8_t));
    out->owner = (int16_t*)safe_malloc_array(n_pts, sizeof(int16_t));
    if (!out->cascade_segments || !out->ridge_candidates || !out->owner) {
        v7_hs_outputs_free(out);
        return -1;
    }
    for (size_t i = 0; i < n_pts; i++) out->cascade_segments[i] = -1;
    if (owner_in) memcpy(out->owner, owner_in, n_pts * sizeof(int16_t));
    else for (size_t i = 0; i < n_pts; i++) out->owner[i] = -1;

    if (n_pts < 3) return 0;
    if (!normals) return 0;

    // Eligible mask (unowned points).
    uint8_t* eligible = (uint8_t*)safe_calloc_array(n_pts, sizeof(uint8_t));
    if (!eligible) { v7_hs_outputs_free(out); return -1; }
    size_t n_eligible = 0;
    for (size_t i = 0; i < n_pts; i++) {
        if (out->owner[i] == -1) { eligible[i] = 1; n_eligible++; }
    }
    if (n_eligible == 0) { free(eligible); return 0; }

    uint32_t* eligible_indices = (uint32_t*)safe_malloc_array(n_eligible, sizeof(uint32_t));
    float* eligible_weights = (float*)safe_malloc_array(n_eligible, sizeof(float));
    if (!eligible_indices || !eligible_weights) {
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    {
        size_t k = 0;
        for (size_t i = 0; i < n_pts; i++) {
            if (eligible[i]) {
                eligible_indices[k] = (uint32_t)i;
                eligible_weights[k] = point_weights[i];
                k++;
            }
        }
    }

    // ════════════════ STAGE 1: Lambert hemisphere ════════════════

    lambert_grid_t grid;
    if (lambert_grid_init(&grid, params->lambert_resolution, params->up_axis) != 0) {
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    lambert_grid_accumulate(&grid, normals, eligible_weights,
                             eligible_indices, n_eligible);
    lambert_grid_gaussian_smooth(&grid, params->lambert_smooth_sigma);

    size_t n_all_peaks = 0;
    lambert_peak_t* all_peaks = lambert_grid_find_peaks(&grid, 0.0, &n_all_peaks);

    int16_t* raw_assign = (int16_t*)safe_malloc_array(n_eligible, sizeof(int16_t));
    if (!raw_assign) {
        free(all_peaks); lambert_grid_free(&grid);
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    if (n_all_peaks > 0) {
        lambert_grid_build_watershed_labels(&grid, all_peaks, n_all_peaks);
        lambert_grid_assign_watershed(&grid, normals, eligible_indices,
                                       n_eligible, raw_assign);
    } else {
        for (size_t k = 0; k < n_eligible; k++) raw_assign[k] = -1;
    }

    // Basin weights (sum of point weights draining to each peak).
    double* basin_weights = (double*)calloc(n_all_peaks ? n_all_peaks : 1, sizeof(double));
    if (!basin_weights) {
        free(raw_assign); free(all_peaks); lambert_grid_free(&grid);
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    for (size_t k = 0; k < n_eligible; k++) {
        int16_t gi = raw_assign[k];
        if (gi >= 0 && (size_t)gi < n_all_peaks) {
            basin_weights[gi] += (double)eligible_weights[k];
        }
    }

    // Filter by basin area, build remap.
    // safe_malloc_array (with ACSL ensures \valid(((char*)\result) + (0 ..
    // n*size - 1))) instead of bare malloc — the ternary `n_all_peaks ?
    // n_all_peaks : 1` lost the size relation in Eva's bare-malloc
    // tracking and produced a sure alarm at the `remap[pi]` read below.
    // Going through the wrapper preserves the contract; we keep the
    // `? 1` to avoid malloc(0)'s implementation-defined return.
    size_t remap_cap = n_all_peaks > 0 ? n_all_peaks : 1;
    int16_t* remap = (int16_t*)safe_malloc_array(remap_cap, sizeof(int16_t));
    if (!remap) {
        free(basin_weights); free(raw_assign); free(all_peaks); lambert_grid_free(&grid);
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    size_t n_peaks = 0;
    for (size_t pi = 0; pi < n_all_peaks; pi++) {
        if (basin_weights[pi] >= params->lambert_min_basin_area) {
            // Frama-C audit finding HS-1: cast `(int16_t)n_peaks++` silently
            // wraps once n_peaks > INT16_MAX (32767). Past that wrap, remap[pi]
            // is negative, and the subsequent filter loop at line ~321 skips
            // these entries — meaning we silently lose peaks beyond 32 K
            // (a typical lambert grid can have far fewer, but pathological
            // hemispheres or future param tuning could exceed it).
            if (n_peaks > INT16_MAX) {
                // remap can't represent the new index; treat as a non-peak
                // rather than wrap silently.
                remap[pi] = -1;
            } else {
                remap[pi] = (int16_t)n_peaks++;
            }
        } else {
            remap[pi] = -1;
        }
    }

    lambert_peak_t* peaks = NULL;
    if (n_peaks > 0) {
        peaks = (lambert_peak_t*)safe_malloc_array(n_peaks, sizeof(lambert_peak_t));
        if (!peaks) {
            free(remap); free(basin_weights); free(raw_assign); free(all_peaks);
            lambert_grid_free(&grid);
            free(eligible); free(eligible_indices); free(eligible_weights);
            v7_hs_outputs_free(out);
            return -1;
        }
        size_t k = 0;
        for (size_t pi = 0; pi < n_all_peaks; pi++) {
            if (remap[pi] >= 0) {
                peaks[k] = all_peaks[pi];
                peaks[k].weight_sum = basin_weights[pi]; // overwrite with basin
                k++;
            }
        }
    }

    // Emit Lambert viz extras (gated).
    if (params->emit_viz_artifacts) {
        out->lambert_grid_resolution = grid.resolution;
        size_t grid_sz = (size_t)grid.resolution * grid.resolution;
        out->lambert_grid = (double*)safe_malloc_array(grid_sz, sizeof(double));
        if (out->lambert_grid) memcpy(out->lambert_grid, grid.grid, grid_sz * sizeof(double));
    }

    // Build per-group point index lists (using local pt indices, not global).
    uint_vec_t* group_pts = (uint_vec_t*)calloc(n_peaks ? n_peaks : 1, sizeof(uint_vec_t));
    if (!group_pts && n_peaks > 0) {
        free(peaks); free(remap); free(basin_weights); free(raw_assign); free(all_peaks);
        lambert_grid_free(&grid);
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    int8_t* normal_group_assign = (int8_t*)safe_malloc_array(n_pts, sizeof(int8_t));
    if (!normal_group_assign) {
        if (group_pts) {
            for (size_t i = 0; i < n_peaks; i++) uvec_free(&group_pts[i]);
            free(group_pts);
        }
        free(peaks); free(remap); free(basin_weights); free(raw_assign); free(all_peaks);
        lambert_grid_free(&grid);
        free(eligible); free(eligible_indices); free(eligible_weights);
        v7_hs_outputs_free(out);
        return -1;
    }
    for (size_t i = 0; i < n_pts; i++) normal_group_assign[i] = -1;

    // Declare seg_acc + kde_blob HERE — before any `goto oom` site below
    // — so -Werror=maybe-uninitialized can prove they're zeroed when the
    // oom: label runs cleanup.
    typedef struct seg_acc {
        v7_hs_segment_t* segs;
        size_t len, cap;
    } seg_acc_t;
    seg_acc_t seg_acc = {0};
    int32_t global_seg_id = 0;
    blob_t kde_blob = {0};

    {
        // Reverse map global→local position-in-eligible-list.
        // For each local point i, look up its assignment in raw_assign.
        // raw_assign is indexed by k (position in eligible_indices) — we need
        // i (local point idx) → k. Since eligible_indices[k] = i, build the
        // inverse here.
        size_t k = 0;
        for (size_t i = 0; i < n_pts; i++) {
            if (!eligible[i]) continue;
            int16_t raw_gi = raw_assign[k];
            int16_t gi = (raw_gi >= 0 && (size_t)raw_gi < n_all_peaks)
                          ? remap[raw_gi] : -1;
            normal_group_assign[i] = (int8_t)gi;
            if (gi >= 0) {
                if (uvec_push(&group_pts[gi], (uint32_t)i) != 0) {
                    // OOM — bail
                    goto oom;
                }
            } else {
                out->ridge_candidates[i] |= 0x80; // orphan bit
            }
            k++;
        }
    }

    // ════════════ STAGE 1b: antipodal merge ════════════
    {
        const double merge_anti_threshold = -0.85;
        uint8_t* merged_flag = (uint8_t*)calloc(n_peaks ? n_peaks : 1, sizeof(uint8_t));
        if (n_peaks > 0 && !merged_flag) goto oom;
        for (size_t i = 0; i < n_peaks; i++) {
            if (merged_flag[i] || group_pts[i].len == 0) continue;
            double nix = peaks[i].normal_x, niy = peaks[i].normal_y, niz = peaks[i].normal_z;
            for (size_t j = i + 1; j < n_peaks; j++) {
                if (merged_flag[j] || group_pts[j].len == 0) continue;
                double njx = peaks[j].normal_x, njy = peaks[j].normal_y, njz = peaks[j].normal_z;
                double d = nix * njx + niy * njy + niz * njz;
                if (d >= merge_anti_threshold) continue;
                // Merge j into i.
                for (size_t kk = 0; kk < group_pts[j].len; kk++) {
                    uint32_t idx = group_pts[j].data[kk];
                    normal_group_assign[idx] = (int8_t)i;
                    if (uvec_push(&group_pts[i], idx) != 0) {
                        free(merged_flag);
                        goto oom;
                    }
                }
                uvec_free(&group_pts[j]);
                merged_flag[j] = 1;
            }
        }
        free(merged_flag);
    }

    // ════════════ STAGE 2 + 3: per-group offset KDE + spatial CC ════════════

    for (size_t gi = 0; gi < n_peaks; gi++) {
        size_t n_g = group_pts[gi].len;
        if (n_g == 0) continue;

        // Group area.
        double group_area = 0.0;
        for (size_t k = 0; k < n_g; k++) {
            uint32_t idx = group_pts[gi].data[k];
            group_area += (double)point_weights[idx];
        }
        if (group_area < params->min_segment_area) {
            for (size_t k = 0; k < n_g; k++) {
                out->ridge_candidates[group_pts[gi].data[k]] |= 0x80;
            }
            continue;
        }

        // Weighted mean normal — flip each point normal toward the peak's
        // canonical direction to avoid sign cancellation.
        double peak_nx = peaks[gi].normal_x;
        double peak_ny = peaks[gi].normal_y;
        double peak_nz = peaks[gi].normal_z;
        double mx = 0, my = 0, mz = 0;
        for (size_t k = 0; k < n_g; k++) {
            uint32_t idx = group_pts[gi].data[k];
            double w = (double)point_weights[idx];
            double nx = (double)normals[idx * 3 + 0];
            double ny = (double)normals[idx * 3 + 1];
            double nz = (double)normals[idx * 3 + 2];
            if (nx * peak_nx + ny * peak_ny + nz * peak_nz < 0.0) {
                nx = -nx; ny = -ny; nz = -nz;
            }
            mx += nx * w; my += ny * w; mz += nz * w;
        }
        double mlen = sqrt(mx * mx + my * my + mz * mz);
        double mean_nx, mean_ny, mean_nz;
        if (mlen > 0.0) {
            mean_nx = mx / mlen; mean_ny = my / mlen; mean_nz = mz / mlen;
        } else {
            mean_nx = 0.0; mean_ny = 0.0; mean_nz = 0.0;
            mean_ny = (params->up_axis == 1) ? 1.0 : 0.0;
            if (params->up_axis == 0) mean_nx = 1.0;
            if (params->up_axis == 2) mean_nz = 1.0;
        }

        // Per-point offset = mean_normal · position.
        double* offsets = (double*)safe_malloc_array(n_g, sizeof(double));
        double* group_w = (double*)safe_malloc_array(n_g, sizeof(double));
        if (!offsets || !group_w) {
            free(offsets); free(group_w);
            goto oom;
        }
        for (size_t k = 0; k < n_g; k++) {
            uint32_t idx = group_pts[gi].data[k];
            const double* p = &positions[(size_t)idx * 3];
            offsets[k] = mean_nx * p[0] + mean_ny * p[1] + mean_nz * p[2];
            group_w[k] = (double)point_weights[idx];
        }

        // Bandwidth selection.
        double bandwidth = params->stage2_iqr_bandwidth > 0.5
            ? v7_iqr_bandwidth(offsets, n_g)
            : v7_silverman_bandwidth(offsets, n_g);
        if (bandwidth < 0.05) bandwidth = 0.05;
        if (bandwidth > 0.30) bandwidth = 0.30;

        // KDE.
        size_t n_eval = (size_t)params->kde_n_eval;
        kde_result_t kde = {
            .eval_points = (double*)safe_malloc_array(n_eval, sizeof(double)),
            .densities = (double*)safe_malloc_array(n_eval, sizeof(double)),
            .n_eval = n_eval,
        };
        if (!kde.eval_points || !kde.densities) {
            free(kde.eval_points); free(kde.densities);
            free(offsets); free(group_w);
            goto oom;
        }
        v7_weighted_gaussian_kde(offsets, group_w, n_g, bandwidth, &kde);

        double total_gw = 0.0;
        for (size_t k = 0; k < n_g; k++) total_gw += group_w[k];
        double kde_min_density = params->min_segment_area / (total_gw * bandwidth);
        if (total_gw <= 0.0) kde_min_density = 0.0;

        // Peaks + valleys (worst-case n_eval / 2 each).
        kde_peak_t* kde_peaks = (kde_peak_t*)safe_malloc_array(n_eval, sizeof(kde_peak_t));
        kde_peak_t* kde_valleys = (kde_peak_t*)safe_malloc_array(n_eval, sizeof(kde_peak_t));
        if (!kde_peaks || !kde_valleys) {
            free(kde.eval_points); free(kde.densities);
            free(kde_peaks); free(kde_valleys);
            free(offsets); free(group_w);
            goto oom;
        }
        size_t n_kpeaks = 0, n_kvalleys = 0;
        v7_find_kde_peaks(kde.eval_points, kde.densities, n_eval, kde_min_density,
                           kde_peaks, &n_kpeaks);
        v7_find_kde_valleys(kde.eval_points, kde.densities, n_eval,
                             kde_valleys, &n_kvalleys);

        // Emit KDE viz record (gated).
        if (params->emit_viz_artifacts) {
            v7_hs_offset_kde_record_t hdr = {
                .normal_group_id = (int32_t)gi,
                .n_eval = (int32_t)n_eval,
                .n_peaks = (int32_t)n_kpeaks,
                .n_valleys = (int32_t)n_kvalleys,
                .bandwidth = bandwidth,
            };
            size_t rec_len = sizeof(hdr)
                             + n_eval * sizeof(double) * 2
                             + n_kpeaks * sizeof(double) * 2
                             + n_kvalleys * sizeof(double) * 2;
            uint8_t* rec = (uint8_t*)malloc(rec_len);
            if (rec) {
                size_t cur = 0;
                memcpy(rec + cur, &hdr, sizeof(hdr)); cur += sizeof(hdr);
                memcpy(rec + cur, kde.eval_points, n_eval * sizeof(double)); cur += n_eval * sizeof(double);
                memcpy(rec + cur, kde.densities,   n_eval * sizeof(double)); cur += n_eval * sizeof(double);
                for (size_t pp = 0; pp < n_kpeaks; pp++) {
                    memcpy(rec + cur, &kde_peaks[pp].center,  8); cur += 8;
                    memcpy(rec + cur, &kde_peaks[pp].density, 8); cur += 8;
                }
                for (size_t vv = 0; vv < n_kvalleys; vv++) {
                    memcpy(rec + cur, &kde_valleys[vv].center,  8); cur += 8;
                    memcpy(rec + cur, &kde_valleys[vv].density, 8); cur += 8;
                }
                blob_push(&kde_blob, rec, rec_len);
                free(rec);
            }
        }

        // Build assignments (n_kpeaks==0 → all in one cluster).
        int16_t* offset_cluster_assign = NULL;
        if (n_kpeaks > 0) {
            offset_cluster_assign = (int16_t*)safe_malloc_array(n_g, sizeof(int16_t));
            if (!offset_cluster_assign) {
                free(kde_peaks); free(kde_valleys);
                free(kde.eval_points); free(kde.densities);
                free(offsets); free(group_w);
                goto oom;
            }
            v7_assign_points_by_valley(offsets, n_g,
                                         kde_peaks, n_kpeaks,
                                         kde_valleys, n_kvalleys,
                                         offset_cluster_assign);
        }

        // For each offset cluster, run Stage 3 (radius-graph CC).
        size_t n_clusters = n_kpeaks > 0 ? n_kpeaks : 1;
        for (size_t opi = 0; opi < n_clusters; opi++) {
            // Collect this cluster's local-in-group indices.
            uint_vec_t cluster_idx = {0};
            for (size_t k = 0; k < n_g; k++) {
                int16_t a = (n_kpeaks > 0) ? offset_cluster_assign[k] : 0;
                if (a == (int16_t)opi) {
                    if (uvec_push(&cluster_idx, group_pts[gi].data[k]) != 0) {
                        uvec_free(&cluster_idx);
                        free(offset_cluster_assign);
                        free(kde_peaks); free(kde_valleys);
                        free(kde.eval_points); free(kde.densities);
                        free(offsets); free(group_w);
                        goto oom;
                    }
                }
            }
            if (cluster_idx.len == 0) { uvec_free(&cluster_idx); continue; }

            // Cluster area filter.
            double cluster_area = 0.0;
            for (size_t kk = 0; kk < cluster_idx.len; kk++) {
                uint32_t idx = cluster_idx.data[kk];
                cluster_area += (double)point_weights[idx];
            }
            if (cluster_area < params->min_segment_area) {
                for (size_t kk = 0; kk < cluster_idx.len; kk++) {
                    out->ridge_candidates[cluster_idx.data[kk]] |= 0x80;
                }
                uvec_free(&cluster_idx);
                continue;
            }

            // Stage 3: radius-graph union-find. If no kd-tree or too few
            // points, treat as single CC.
            uf_t uf;
            int32_t cn = (int32_t)cluster_idx.len;
            if (!tree || cn < 3 || uf_init(&uf, cn) != 0) {
                // Emit as single segment.
                v7_hs_segment_t seg = {
                    .id = global_seg_id++,
                    .normal_group_id = (int32_t)gi,
                    .offset_cluster_id = (int32_t)opi,
                    .spatial_cluster_id = 0,
                    .peak_offset = (n_kpeaks > 0) ? kde_peaks[opi].center : 0.0,
                    .mean_normal = {mean_nx, mean_ny, mean_nz},
                    .n_pts = (uint32_t)cluster_idx.len,
                    .pt_indices = cluster_idx.data,
                };
                cluster_idx.data = NULL; // ownership transfer
                cluster_idx.len = cluster_idx.cap = 0;
                for (size_t kk = 0; kk < seg.n_pts; kk++) {
                    out->cascade_segments[seg.pt_indices[kk]] = (int16_t)seg.id;
                }
                if (seg_acc.len == seg_acc.cap) {
                    size_t nc = seg_acc.cap ? seg_acc.cap * 2 : 8;
                    v7_hs_segment_t* p = (v7_hs_segment_t*)safe_realloc_array(seg_acc.segs, nc, sizeof(v7_hs_segment_t));
                    if (!p) {
                        free(seg.pt_indices);
                        free(offset_cluster_assign);
                        free(kde_peaks); free(kde_valleys);
                        free(kde.eval_points); free(kde.densities);
                        free(offsets); free(group_w);
                        goto oom;
                    }
                    seg_acc.segs = p; seg_acc.cap = nc;
                }
                seg_acc.segs[seg_acc.len++] = seg;
                uvec_free(&cluster_idx);
                continue;
            }

            // Build local-position lookup: global-point-idx → cluster-local-idx.
            // Use a hash via linear probe over the cluster.
            // Cheaper for our sizes (≤ a few thousand): just linear search.
            // (Optimisable later if profile shows it hot.)

            double connect_scale = params->connect_radius_scale;
            double connect_max = params->connect_radius_max;
            for (int32_t i = 0; i < cn; i++) {
                uint32_t idx = cluster_idx.data[i];
                const double* pt = &positions[(size_t)idx * 3];
                double w = (double)point_weights[idx];
                double local_radius = sqrt(w) * connect_scale;
                if (local_radius > connect_max) local_radius = connect_max;

                rs_state_t s = {
                    .tree = tree, .qx = pt[0], .qy = pt[1], .qz = pt[2],
                    .radius_sq = local_radius * local_radius,
                };
                rs_search(&s, tree->root);

                for (size_t k = 0; k < s.out_count; k++) {
                    int32_t hit_global = s.out_indices[k];
                    if (hit_global == (int32_t)idx) continue;
                    // Linear lookup: find hit_global in cluster_idx.
                    for (int32_t j = 0; j < cn; j++) {
                        if (cluster_idx.data[j] == (uint32_t)hit_global && j != i) {
                            uf_union(&uf, i, j);
                            break;
                        }
                    }
                }
                free(s.out_indices);
            }

            // Collect components.
            // root_to_comp maps representative → array index in components.
            int32_t* comp_for = (int32_t*)malloc((size_t)cn * sizeof(int32_t));
            int32_t* comp_root_table = (int32_t*)malloc((size_t)cn * sizeof(int32_t));
            int32_t* comp_size = (int32_t*)calloc((size_t)cn, sizeof(int32_t));
            if (!comp_for || !comp_root_table || !comp_size) {
                free(comp_for); free(comp_root_table); free(comp_size);
                uf_free(&uf);
                uvec_free(&cluster_idx);
                free(offset_cluster_assign);
                free(kde_peaks); free(kde_valleys);
                free(kde.eval_points); free(kde.densities);
                free(offsets); free(group_w);
                goto oom;
            }
            int32_t n_comps = 0;
            for (int32_t i = 0; i < cn; i++) {
                int32_t r = uf_find(&uf, i);
                int32_t found = -1;
                for (int32_t cc = 0; cc < n_comps; cc++) {
                    if (comp_root_table[cc] == r) { found = cc; break; }
                }
                if (found < 0) {
                    comp_root_table[n_comps] = r;
                    found = n_comps++;
                }
                comp_for[i] = found;
                comp_size[found]++;
            }

            // Pre-alloc per-comp index lists.
            uint_vec_t* comp_pts = (uint_vec_t*)calloc((size_t)n_comps, sizeof(uint_vec_t));
            if (!comp_pts) {
                free(comp_for); free(comp_root_table); free(comp_size);
                uf_free(&uf);
                uvec_free(&cluster_idx);
                free(offset_cluster_assign);
                free(kde_peaks); free(kde_valleys);
                free(kde.eval_points); free(kde.densities);
                free(offsets); free(group_w);
                goto oom;
            }
            for (int32_t cc = 0; cc < n_comps; cc++) {
                comp_pts[cc].data = (uint32_t*)malloc((size_t)comp_size[cc] * sizeof(uint32_t));
                comp_pts[cc].cap = (size_t)comp_size[cc];
            }
            for (int32_t i = 0; i < cn; i++) {
                int32_t cc = comp_for[i];
                comp_pts[cc].data[comp_pts[cc].len++] = cluster_idx.data[i];
            }

            int32_t spatial_idx = 0;
            for (int32_t cc = 0; cc < n_comps; cc++) {
                double comp_area = 0.0;
                for (size_t kk = 0; kk < comp_pts[cc].len; kk++) {
                    uint32_t idx = comp_pts[cc].data[kk];
                    comp_area += (double)point_weights[idx];
                }
                if (comp_area < params->min_segment_area) {
                    for (size_t kk = 0; kk < comp_pts[cc].len; kk++) {
                        out->ridge_candidates[comp_pts[cc].data[kk]] |= 0x80;
                    }
                    uvec_free(&comp_pts[cc]);
                    continue;
                }
                v7_hs_segment_t seg = {
                    .id = global_seg_id++,
                    .normal_group_id = (int32_t)gi,
                    .offset_cluster_id = (int32_t)opi,
                    .spatial_cluster_id = spatial_idx++,
                    .peak_offset = (n_kpeaks > 0) ? kde_peaks[opi].center : 0.0,
                    .mean_normal = {mean_nx, mean_ny, mean_nz},
                    .n_pts = (uint32_t)comp_pts[cc].len,
                    .pt_indices = comp_pts[cc].data,
                };
                comp_pts[cc].data = NULL;
                comp_pts[cc].len = comp_pts[cc].cap = 0;
                for (size_t kk = 0; kk < seg.n_pts; kk++) {
                    out->cascade_segments[seg.pt_indices[kk]] = (int16_t)seg.id;
                }
                if (seg_acc.len == seg_acc.cap) {
                    size_t nc = seg_acc.cap ? seg_acc.cap * 2 : 8;
                    v7_hs_segment_t* p = (v7_hs_segment_t*)safe_realloc_array(seg_acc.segs, nc, sizeof(v7_hs_segment_t));
                    if (!p) {
                        free(seg.pt_indices);
                        for (int32_t cc2 = cc + 1; cc2 < n_comps; cc2++) uvec_free(&comp_pts[cc2]);
                        free(comp_pts);
                        free(comp_for); free(comp_root_table); free(comp_size);
                        uf_free(&uf);
                        uvec_free(&cluster_idx);
                        free(offset_cluster_assign);
                        free(kde_peaks); free(kde_valleys);
                        free(kde.eval_points); free(kde.densities);
                        free(offsets); free(group_w);
                        goto oom;
                    }
                    seg_acc.segs = p; seg_acc.cap = nc;
                }
                seg_acc.segs[seg_acc.len++] = seg;
            }

            free(comp_pts); free(comp_for); free(comp_root_table); free(comp_size);
            uf_free(&uf);
            uvec_free(&cluster_idx);
        }

        free(offset_cluster_assign);
        free(kde_peaks); free(kde_valleys);
        free(kde.eval_points); free(kde.densities);
        free(offsets); free(group_w);
    }

    // Emit lambert peak records (viz, gated).
    if (params->emit_viz_artifacts && n_peaks > 0) {
        out->lambert_peaks = (v7_hs_lambert_peak_record_t*)
                              safe_calloc_array(n_peaks, sizeof(v7_hs_lambert_peak_record_t));
        if (out->lambert_peaks) {
            out->n_lambert_peaks = n_peaks;
            for (size_t pi = 0; pi < n_peaks; pi++) {
                out->lambert_peaks[pi].tilt = peaks[pi].tilt;
                out->lambert_peaks[pi].azimuth = peaks[pi].azimuth;
                out->lambert_peaks[pi].basin_weight = peaks[pi].weight_sum;
                out->lambert_peaks[pi].peak_normal[0] = peaks[pi].normal_x;
                out->lambert_peaks[pi].peak_normal[1] = peaks[pi].normal_y;
                out->lambert_peaks[pi].peak_normal[2] = peaks[pi].normal_z;
                out->lambert_peaks[pi].n_assigned_pts = (uint32_t)group_pts[pi].len;
            }
        }
    }

    out->segments = seg_acc.segs;
    out->n_segments = seg_acc.len;
    if (params->emit_viz_artifacts) {
        out->offset_kde_blob = kde_blob.bytes;
        out->offset_kde_blob_len = kde_blob.len;
        out->offset_kde_n_records = kde_blob.n_records;
    } else {
        free(kde_blob.bytes);
    }

    // Cleanup.
    for (size_t i = 0; i < n_peaks; i++) uvec_free(&group_pts[i]);
    free(group_pts);
    free(normal_group_assign);
    free(peaks);
    free(remap);
    free(basin_weights);
    free(raw_assign);
    free(all_peaks);
    lambert_grid_free(&grid);
    free(eligible);
    free(eligible_indices);
    free(eligible_weights);
    return 0;

oom:
    if (group_pts) {
        for (size_t i = 0; i < n_peaks; i++) uvec_free(&group_pts[i]);
        free(group_pts);
    }
    free(normal_group_assign);
    free(peaks);
    free(remap);
    free(basin_weights);
    free(raw_assign);
    free(all_peaks);
    lambert_grid_free(&grid);
    free(eligible);
    free(eligible_indices);
    free(eligible_weights);
    if (seg_acc.segs) {
        for (size_t i = 0; i < seg_acc.len; i++) free(seg_acc.segs[i].pt_indices);
        free(seg_acc.segs);
    }
    free(kde_blob.bytes);
    v7_hs_outputs_free(out);
    return -1;
}
