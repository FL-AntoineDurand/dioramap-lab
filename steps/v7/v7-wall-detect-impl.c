// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-wall-detect — algorithm impl. Port of bench-v2 wall-detect.js v4.

#define _POSIX_C_SOURCE 200809L

#include "v7-wall-detect-impl.h"
#include "v3/safe_alloc.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Defaults ──────────────────────────────────────────────────────

void v7_wd_params_default(v7_wd_params_t* p) {
    if (!p) return;
    p->search_width_m                  = 2.0;
    p->search_length_buffer_m          = 1.0;
    p->overhang_cap_m                  = 1.5;
    p->merge_angle_deg                 = 10.0;
    p->merge_offset_m                  = 0.30;       // parity-only (unused by v2 algo)
    p->point_match_dist_m              = 0.0;        // 0 → consumer passes ppThresh
    p->min_beam_downness               = 0.05;
    p->y_top_buffer_m                  = 0.10;
    p->wall_tilt_min_deg               = 72.0;
    p->search_outward_tolerance_m      = 0.10;
    p->overhang_cap_search_buffer_m    = 0.30;
    p->point_match_y_pad_m             = 0.10;
    p->point_match_xz_pad_m            = 0.05;
    p->point_in_footprint_eps          = 0.05;
    p->min_edge_len_3d                 = 0.10;
    p->min_edge_len_xz                 = 0.01;
    p->min_existing_wall_normal_xz     = 0.5;
    p->up_axis                         = 1;          // y-up (v2 default)
}

// ── Outputs lifecycle ─────────────────────────────────────────────

void v7_wd_outputs_init(v7_wd_outputs_t* o) {
    if (o) memset(o, 0, sizeof(*o));
}

void v7_wd_outputs_free(v7_wd_outputs_t* o) {
    if (!o) return;
    free(o->wall_planes);
    free(o->wall_corners);
    free(o->owner_changes);
    free(o->debug);
    free(o->viz_edges);
    memset(o, 0, sizeof(*o));
}

// ── Internal: id maps ─────────────────────────────────────────────

typedef struct id_map {
    uint32_t* idx;
    uint32_t  cap;
} id_map_t;

static int id_map_build(id_map_t* m, uint32_t max_id_plus_one) {
    m->cap = max_id_plus_one;
    if (m->cap == 0) { m->idx = NULL; return 0; }
    m->idx = (uint32_t*)safe_malloc_array(m->cap, sizeof(uint32_t));
    if (!m->idx) return -1;
    for (uint32_t i = 0; i < m->cap; i++) m->idx[i] = UINT32_MAX;
    return 0;
}
static void id_map_free(id_map_t* m) { free(m->idx); m->idx = NULL; m->cap = 0; }
static inline uint32_t id_map_lookup(const id_map_t* m, uint32_t id) {
    if (!m->idx || id >= m->cap) return UINT32_MAX;
    return m->idx[id];
}
static inline void id_map_set(id_map_t* m, uint32_t id, uint32_t idx) {
    if (id < m->cap) m->idx[id] = idx;
}

// ── Growable vector helpers ───────────────────────────────────────

#define VEC_INIT(field, cap0, T)                                        \
    do {                                                                \
        out->field = (T*)safe_malloc_array((cap0), sizeof(T));          \
        if (!out->field) goto cleanup;                                  \
        out->n_##field = 0;                                              \
    } while (0)

#define VEC_PUSH(field, cap_var, val, T)                                   \
    do {                                                                   \
        if (out->n_##field == (cap_var)) {                                 \
            size_t _nc = (cap_var) * 2;                                    \
            T* _nb = (T*)safe_realloc_array(out->field, _nc, sizeof(T));   \
            if (!_nb) goto cleanup;                                        \
            out->field = _nb;                                              \
            (cap_var) = _nc;                                               \
        }                                                                  \
        out->field[out->n_##field++] = (val);                              \
    } while (0)

// ── Main compute ──────────────────────────────────────────────────

int v7_wd_compute(
    const v7_wd_input_plane_t* planes, size_t n_planes,
    const v7_wd_input_corner_t* corners, size_t n_corners,
    const v7_wd_input_edge_t* edges, size_t n_edges,
    const v7_wd_input_perim_tag_t* perim_tags, size_t n_perim_tags,
    const float* positions, size_t n_points,
    const float* beam_directions,
    int32_t* owner_inout,
    double point_plane_dist_threshold,
    const v7_wd_params_t* params,
    v7_wd_outputs_t* out
) {
    if (!planes || !corners || !edges || !perim_tags || !positions ||
        !owner_inout || !params || !out) return -1;
    v7_wd_outputs_init(out);

    if (n_points == 0 || n_edges == 0 || n_planes == 0) {
        // Nothing to do — pipeline upstream should have produced these,
        // but exits cleanly if not. Caller distinguishes via n_wall_planes.
        return 0;
    }

    const double SEARCH_WIDTH     = params->search_width_m;
    const double SEARCH_LEN_BUFF  = params->search_length_buffer_m;
    const double OVERHANG_CAP     = params->overhang_cap_m;
    const double MERGE_ANGLE_COS  = cos(params->merge_angle_deg * M_PI / 180.0);
    const double MIN_BEAM_DOWN    = params->min_beam_downness;
    const double Y_TOP_BUFFER     = params->y_top_buffer_m;
    const double WALL_TILT_MIN    = params->wall_tilt_min_deg;
    const double OUTWARD_TOL      = params->search_outward_tolerance_m;
    const double CAP_SEARCH_BUF   = params->overhang_cap_search_buffer_m;
    const double Y_PAD            = params->point_match_y_pad_m;
    const double XZ_PAD           = params->point_match_xz_pad_m;
    const double FOOTPRINT_EPS    = params->point_in_footprint_eps;
    const double MIN_LEN_3D       = params->min_edge_len_3d;
    const double MIN_LEN_XZ       = params->min_edge_len_xz;
    const double MIN_WALL_NORMAL_XZ = params->min_existing_wall_normal_xz;
    const double POINT_MATCH_DIST = (params->point_match_dist_m > 0.0)
        ? params->point_match_dist_m : point_plane_dist_threshold;

    int rc = -1;
    id_map_t plane_map = {0}, corner_map = {0}, edge_map = {0};
    // Declared here (NULL) so the `goto cleanup` paths below — which precede
    // the malloc at its first use — free a well-defined pointer rather than
    // an indeterminate one (-Wmaybe-uninitialized / latent free-of-garbage).
    uint32_t* existing_walls = NULL;
    size_t    n_existing_walls = 0;

    // ── Build id maps ─────────────────────────────────────────────
    uint32_t max_plane = 0, max_corner = 0, max_edge = 0;
    for (size_t i = 0; i < n_planes;  i++) if (planes[i].id  > max_plane)  max_plane  = planes[i].id;
    for (size_t i = 0; i < n_corners; i++) if (corners[i].id > max_corner) max_corner = corners[i].id;
    for (size_t i = 0; i < n_edges;   i++) if (edges[i].id   > max_edge)   max_edge   = edges[i].id;
    if (id_map_build(&plane_map,  max_plane  + 1) != 0) goto cleanup;
    if (id_map_build(&corner_map, max_corner + 1) != 0) goto cleanup;
    if (id_map_build(&edge_map,   max_edge   + 1) != 0) goto cleanup;
    for (size_t i = 0; i < n_planes;  i++) id_map_set(&plane_map,  planes[i].id,  (uint32_t)i);
    for (size_t i = 0; i < n_corners; i++) id_map_set(&corner_map, corners[i].id, (uint32_t)i);
    for (size_t i = 0; i < n_edges;   i++) id_map_set(&edge_map,   edges[i].id,   (uint32_t)i);

    // ── Building-wide ground: min y across positions ──────────────
    // v2 falls back silently to 0 if positions is empty; v3 asserts —
    // empty positions can't reach here (we already returned above).
    double building_ground_y = INFINITY;
    for (size_t i = 0; i < n_points; i++) {
        double y = (double)positions[i * 3 + 1];
        if (y < building_ground_y) building_ground_y = y;
    }
    if (!isfinite(building_ground_y)) {
        // Per audit #275: fail-loud instead of silent fallback.
        // n_points > 0 is checked above, so a non-finite min means all
        // y-coords are NaN — pipeline-stage bug, surface it.
        rc = -2;
        goto cleanup;
    }

    // ── Next-id allocation ────────────────────────────────────────
    uint32_t next_corner_id = max_corner + 1;
    uint32_t next_plane_id  = max_plane  + 1;

    // ── Filter existing walls (tilt ≥ WALL_TILT_MIN, not rejected, has
    //    horizontal-enough normal). Indices into planes[]. Iterated in
    //    plane-id order via the dense filter loop (planes[] is already
    //    id-indexed from upstream).
    existing_walls = (uint32_t*)safe_malloc_array(n_planes ? n_planes : 1, sizeof(uint32_t));
    if (!existing_walls) goto cleanup;
    for (size_t i = 0; i < n_planes; i++) {
        if (planes[i].rejected) continue;
        if (planes[i].tilt_deg < WALL_TILT_MIN) continue;
        double nxe = planes[i].normal[0], nze = planes[i].normal[2];
        double nlen = sqrt(nxe * nxe + nze * nze);
        if (nlen < MIN_WALL_NORMAL_XZ) continue;
        existing_walls[n_existing_walls++] = (uint32_t)i;
    }

    // ── Allocate outputs (initial capacity hints) ─────────────────
    size_t cap_planes = 16, cap_corners = 64, cap_changes = 1024, cap_debug = 16, cap_viz = 64;
    VEC_INIT(wall_planes,   cap_planes,  v7_wd_wall_plane_t);
    VEC_INIT(wall_corners,  cap_corners, v7_wd_wall_corner_t);
    VEC_INIT(owner_changes, cap_changes, v7_wd_owner_change_t);
    VEC_INIT(debug,         cap_debug,   v7_wd_debug_entry_t);
    VEC_INIT(viz_edges,     cap_viz,     v7_wd_wall_viz_edge_t);

    // ── Main per-edge loop ────────────────────────────────────────
    //
    // Iterate by perim_tags[] order (matches v2's `edges.filter(...)`
    // iteration over the edges array, since perim_tags is parallel to
    // the input edges[] array by edge index — boundary-classify emits
    // one record per edge with tag = UNSET for skipped edges).

    for (size_t pi = 0; pi < n_perim_tags; pi++) {
        const v7_wd_input_perim_tag_t* pt = &perim_tags[pi];
        if (pt->tag != V7_BCLASS_TAG_PERIMETER &&
            pt->tag != V7_BCLASS_TAG_LEVEL_TRANSITION) continue;
        uint32_t eidx = id_map_lookup(&edge_map, pt->edge_id);
        if (eidx == UINT32_MAX) continue;
        const v7_wd_input_edge_t* e = &edges[eidx];

        v7_wd_debug_entry_t dbg = {0};
        dbg.edge_id = e->id;
        dbg.tag     = pt->tag;
        dbg.merged_with_plane_id = UINT32_MAX;
        dbg.new_plane_id         = UINT32_MAX;

        if (!pt->has_outward_xz) {
            dbg.skipped = true;
            VEC_PUSH(debug, cap_debug, dbg, v7_wd_debug_entry_t);
            out->n_edges_skipped++;
            continue;
        }
        uint32_t cai = id_map_lookup(&corner_map, e->corner_a);
        uint32_t cbi = id_map_lookup(&corner_map, e->corner_b);
        if (cai == UINT32_MAX || cbi == UINT32_MAX) {
            dbg.skipped = true;
            VEC_PUSH(debug, cap_debug, dbg, v7_wd_debug_entry_t);
            out->n_edges_skipped++;
            continue;
        }
        const double ax = corners[cai].point[0], ay = corners[cai].point[1], az = corners[cai].point[2];
        const double bx = corners[cbi].point[0], by = corners[cbi].point[1], bz = corners[cbi].point[2];
        const double ex = bx - ax, ey = by - ay, ez = bz - az;
        const double e_len   = sqrt(ex * ex + ey * ey + ez * ez);
        const double e_lenxz = sqrt(ex * ex + ez * ez);
        if (e_len < MIN_LEN_3D || e_lenxz < MIN_LEN_XZ) {
            dbg.skipped = true;
            VEC_PUSH(debug, cap_debug, dbg, v7_wd_debug_entry_t);
            out->n_edges_skipped++;
            continue;
        }
        const double tx = ex / e_lenxz;
        const double tz = ez / e_lenxz;
        const double nx = (double)pt->outward_xz[0];
        const double nz = (double)pt->outward_xz[1];
        const double mx = (ax + bx) * 0.5;
        const double mz = (az + bz) * 0.5;
        const double y_top = ay > by ? ay : by;

        // ── Phase 1: beam-geometry overhang bound ─────────────────
        double delta_min = 0.0;
        uint32_t witness_count = 0;
        const double half_len = e_len * 0.5 + SEARCH_LEN_BUFF;
        const double search_width_sq_dummy = SEARCH_WIDTH;  // (avoid unused warning when no points)
        (void)search_width_sq_dummy;

        for (size_t gi = 0; gi < n_points; gi++) {
            const size_t o = gi * 3;
            const double py = (double)positions[o + 1];
            if (py >= y_top - Y_TOP_BUFFER) continue;
            const double px = (double)positions[o + 0];
            const double pz = (double)positions[o + 2];
            const double dx = px - mx;
            const double dz = pz - mz;
            const double t_dist = dx * tx + dz * tz;
            if (fabs(t_dist) > half_len) continue;
            const double n_dist = dx * nx + dz * nz;
            if (fabs(n_dist) > SEARCH_WIDTH) continue;

            const double eave_x = mx + t_dist * tx;
            const double eave_z = mz + t_dist * tz;
            const double p_p = (eave_x - px) * nx + (eave_z - pz) * nz;
            if (p_p <= FOOTPRINT_EPS) continue;

            // Beam direction — default to nadir (0,-1,0).
            double beam_x = 0.0, beam_y = -1.0, beam_z = 0.0;
            if (beam_directions) {
                const double bxv = (double)beam_directions[o + 0];
                const double byv = (double)beam_directions[o + 1];
                const double bzv = (double)beam_directions[o + 2];
                if (byv < -MIN_BEAM_DOWN) { beam_x = bxv; beam_y = byv; beam_z = bzv; }
            }
            if (beam_y > -MIN_BEAM_DOWN) continue;

            const double t_beam = (y_top - py) / (-beam_y);
            const double pat_top_x = px - t_beam * beam_x;
            const double pat_top_z = pz - t_beam * beam_z;
            const double p_t = (eave_x - pat_top_x) * nx + (eave_z - pat_top_z) * nz;
            const double delta_this = p_p > p_t ? p_p : p_t;
            if (delta_this > delta_min) delta_min = delta_this;
            witness_count++;
        }
        int delta_source = (delta_min > 1e-6) ? V7_WD_DELTA_BEAM_BOUND : V7_WD_DELTA_NAIVE;
        if (delta_min > OVERHANG_CAP) delta_min = OVERHANG_CAP;

        // ── Phase 2: existing-wall merge ──────────────────────────
        uint32_t merged_idx = UINT32_MAX;
        double merged_inward = 0.0;
        double best_score = INFINITY;
        for (size_t k = 0; k < n_existing_walls; k++) {
            const v7_wd_input_plane_t* ep = &planes[existing_walls[k]];
            const double enx_raw = ep->normal[0], enz_raw = ep->normal[2];
            const double en_xz   = sqrt(enx_raw * enx_raw + enz_raw * enz_raw);
            if (en_xz < MIN_WALL_NORMAL_XZ) continue;  // already filtered, but defensive
            const double en_x_u = enx_raw / en_xz;
            const double en_z_u = enz_raw / en_xz;
            const double cos_ang = nx * en_x_u + nz * en_z_u;
            if (fabs(cos_ang) < MERGE_ANGLE_COS) continue;
            const double sign = cos_ang >= 0.0 ? 1.0 : -1.0;
            const double adjusted_d = sign * ep->d / en_xz;
            const double inward_dist = (sign * en_x_u) * mx + (sign * en_z_u) * mz + adjusted_d;
            if (inward_dist < -OUTWARD_TOL) continue;
            if (inward_dist > OVERHANG_CAP + CAP_SEARCH_BUF) continue;
            // v2 also does a hull-proximity check; v3 doesn't track hull
            // (derivable from polygon-paths). Skip the check in v3 — it
            // only ever returns "near" for walls without hulls in v2;
            // walls with hulls (rare in v7 output) need a separate hull
            // emission path which we don't carry. Default-near matches
            // v2's `else { near = true; }` fallback when no hull exists.
            const double score = fabs(inward_dist);
            if (score < best_score) {
                best_score    = score;
                merged_idx    = (uint32_t)existing_walls[k];
                merged_inward = inward_dist > 0.0 ? inward_dist : 0.0;
            }
        }

        if (merged_idx != UINT32_MAX) {
            delta_min = merged_inward;
            delta_source = V7_WD_DELTA_EXISTING_WALL;
        }
        dbg.delta_min_m = delta_min;
        dbg.delta_source = delta_source;
        dbg.witness_count = witness_count;

        // ── Phase 3: allocate/reuse plane, build 4 corners ────────
        const double wall_x = mx - delta_min * nx;
        const double wall_z = mz - delta_min * nz;
        const double wall_d = -(nx * wall_x + nz * wall_z);

        v7_wd_wall_plane_t wp = {0};
        wp.existing_plane_id = merged_idx == UINT32_MAX ? UINT32_MAX : planes[merged_idx].id;
        wp.id = merged_idx == UINT32_MAX ? next_plane_id++ : planes[merged_idx].id;
        wp.normal[0] = nx; wp.normal[1] = 0.0; wp.normal[2] = nz;
        wp.d = wall_d;
        wp.tilt_deg = 90.0;
        wp.is_merged = merged_idx != UINT32_MAX;
        wp.source_edge_id = e->id;
        wp.source_perim_tag = pt->tag;

        // Project endpoints onto wall plane to get the top corners.
        const double sa = (ax - wall_x) * nx + (az - wall_z) * nz;
        const double top_a_x = ax - sa * nx, top_a_y = ay, top_a_z = az - sa * nz;
        const double sb = (bx - wall_x) * nx + (bz - wall_z) * nz;
        const double top_b_x = bx - sb * nx, top_b_y = by, top_b_z = bz - sb * nz;
        const double bot_a_x = top_a_x, bot_a_y = building_ground_y, bot_a_z = top_a_z;
        const double bot_b_x = top_b_x, bot_b_y = building_ground_y, bot_b_z = top_b_z;

        // 4 corners — winding order: top_a, top_b, bot_b, bot_a.
        v7_wd_wall_corner_t c_top_a = {.id = next_corner_id++, .plane_id = wp.id,
            .point = {top_a_x, top_a_y, top_a_z}};
        v7_wd_wall_corner_t c_top_b = {.id = next_corner_id++, .plane_id = wp.id,
            .point = {top_b_x, top_b_y, top_b_z}};
        v7_wd_wall_corner_t c_bot_b = {.id = next_corner_id++, .plane_id = wp.id,
            .point = {bot_b_x, bot_b_y, bot_b_z}};
        v7_wd_wall_corner_t c_bot_a = {.id = next_corner_id++, .plane_id = wp.id,
            .point = {bot_a_x, bot_a_y, bot_a_z}};
        snprintf(c_top_a.name, sizeof(c_top_a.name), "W%u_topA", wp.id);
        snprintf(c_top_b.name, sizeof(c_top_b.name), "W%u_topB", wp.id);
        snprintf(c_bot_b.name, sizeof(c_bot_b.name), "W%u_botB", wp.id);
        snprintf(c_bot_a.name, sizeof(c_bot_a.name), "W%u_botA", wp.id);

        wp.polygon_corner_ids[0] = c_top_a.id;
        wp.polygon_corner_ids[1] = c_top_b.id;
        wp.polygon_corner_ids[2] = c_bot_b.id;
        wp.polygon_corner_ids[3] = c_bot_a.id;

        // Centroid: average of 4 corners.
        wp.centroid[0] = (top_a_x + top_b_x + bot_a_x + bot_b_x) * 0.25;
        wp.centroid[1] = (top_a_y + top_b_y + bot_a_y + bot_b_y) * 0.25;
        wp.centroid[2] = (top_a_z + top_b_z + bot_a_z + bot_b_z) * 0.25;

        // Area: rectangle width × height.
        const double wall_width  = sqrt(
            (top_b_x - top_a_x) * (top_b_x - top_a_x) +
            (top_b_z - top_a_z) * (top_b_z - top_a_z));
        const double wall_height = y_top - building_ground_y;
        wp.area_m2 = wall_width * wall_height;

        VEC_PUSH(wall_planes,  cap_planes,  wp,       v7_wd_wall_plane_t);
        VEC_PUSH(wall_corners, cap_corners, c_top_a,  v7_wd_wall_corner_t);
        VEC_PUSH(wall_corners, cap_corners, c_top_b,  v7_wd_wall_corner_t);
        VEC_PUSH(wall_corners, cap_corners, c_bot_b,  v7_wd_wall_corner_t);
        VEC_PUSH(wall_corners, cap_corners, c_bot_a,  v7_wd_wall_corner_t);

        if (wp.is_merged) {
            out->n_walls_merged++;
            dbg.merged_with_plane_id = wp.id;
        } else {
            out->n_walls_created++;
            dbg.new_plane_id = wp.id;
        }

        // ── Phase 5: point grab ───────────────────────────────────
        double xz_min_x = top_a_x < top_b_x ? top_a_x : top_b_x;
        double xz_max_x = top_a_x > top_b_x ? top_a_x : top_b_x;
        double xz_min_z = top_a_z < top_b_z ? top_a_z : top_b_z;
        double xz_max_z = top_a_z > top_b_z ? top_a_z : top_b_z;
        xz_min_x -= XZ_PAD; xz_max_x += XZ_PAD;
        xz_min_z -= XZ_PAD; xz_max_z += XZ_PAD;

        uint32_t grabbed = 0;
        for (size_t gi = 0; gi < n_points; gi++) {
            if (owner_inout[gi] != -1) continue;
            const size_t o = gi * 3;
            const double py = (double)positions[o + 1];
            if (py < building_ground_y - Y_PAD || py > y_top + Y_PAD) continue;
            const double px = (double)positions[o + 0];
            const double pz = (double)positions[o + 2];
            if (px < xz_min_x || px > xz_max_x || pz < xz_min_z || pz > xz_max_z) continue;
            const double perp = fabs((px - wall_x) * nx + (pz - wall_z) * nz);
            if (perp > POINT_MATCH_DIST) continue;
            v7_wd_owner_change_t chg = {.point_idx = (uint32_t)gi, .old_owner = owner_inout[gi], .new_owner = (int32_t)wp.id};
            VEC_PUSH(owner_changes, cap_changes, chg, v7_wd_owner_change_t);
            owner_inout[gi] = (int32_t)wp.id;
            grabbed++;
        }
        out->n_points_grabbed += grabbed;
        dbg.points_grabbed = grabbed;
        dbg.y_top    = y_top;
        dbg.y_bottom = building_ground_y;
        VEC_PUSH(debug, cap_debug, dbg, v7_wd_debug_entry_t);

        // Viz: 4-edge outline of the wall polygon.
        v7_wd_wall_viz_edge_t ve;
        ve.plane_id = wp.id; ve.source_perim_tag = pt->tag;
        ve.from[0] = top_a_x; ve.from[1] = top_a_y; ve.from[2] = top_a_z;
        ve.to[0]   = top_b_x; ve.to[1]   = top_b_y; ve.to[2]   = top_b_z;
        VEC_PUSH(viz_edges, cap_viz, ve, v7_wd_wall_viz_edge_t);
        ve.from[0] = top_b_x; ve.from[1] = top_b_y; ve.from[2] = top_b_z;
        ve.to[0]   = bot_b_x; ve.to[1]   = bot_b_y; ve.to[2]   = bot_b_z;
        VEC_PUSH(viz_edges, cap_viz, ve, v7_wd_wall_viz_edge_t);
        ve.from[0] = bot_b_x; ve.from[1] = bot_b_y; ve.from[2] = bot_b_z;
        ve.to[0]   = bot_a_x; ve.to[1]   = bot_a_y; ve.to[2]   = bot_a_z;
        VEC_PUSH(viz_edges, cap_viz, ve, v7_wd_wall_viz_edge_t);
        ve.from[0] = bot_a_x; ve.from[1] = bot_a_y; ve.from[2] = bot_a_z;
        ve.to[0]   = top_a_x; ve.to[1]   = top_a_y; ve.to[2]   = top_a_z;
        VEC_PUSH(viz_edges, cap_viz, ve, v7_wd_wall_viz_edge_t);
    }

    free(existing_walls);
    rc = 0;
cleanup:
    id_map_free(&plane_map);
    id_map_free(&corner_map);
    id_map_free(&edge_map);
    if (rc != 0) {
        free(existing_walls);
        v7_wd_outputs_free(out);
    }
    return rc;
}
