// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-regularize — step wrapper. Consumes the per-bldg roof-graph
// TLV from v7-boundary-polygons-2 (Path-B reorder 2026-06-12: merge removed,
// regularize now runs before classify), runs R.0/R.0b/R.1/R.2/R.3 + joint moves
// + R.7 pairwise, and emits an updated roof-graph with regularized
// corner positions + (R.0b/R.2/R.3-)mutated polygon paths. All other
// upstream sections (PLANES, RIDGES, RIDGE_LINES, EDGES, PERIMETER_TAGS,
// NEW_ADJACENCIES, NEARBY_PROJECTIONS, PLANE_GRIDS, …) pass through
// unchanged.
//
// Reads:
//   lidar-positions                       (tile, F64×3·N)
//   per-building-records                  (per-bldg)
//   building-local-frames                 (per-bldg — centroid)
//   boundary-polygons-roof-graph          (per-bldg TLV input)
//   grm-owner                              (per-bldg I16×n_pts)
//   grm-residual                           (per-bldg F32×n_pts — point-plane residual)
//   bldg-thresholds                        (per-bldg — point-plane-distance-threshold)
//
// Writes:
//   boundary-regularize-roof-graph        (per-bldg ARRAY_INLINE of TLV)
//
// Sections re-emitted (with mutations):
//   CORNERS         (0x06)  — updated `point[3]`, dead corners filtered out
//   POLYGON_PATHS   (0x0B)  — paths re-emitted from final post-loop polys[]
//                              (R.0b + in-loop R.2/R.3/R.4); EDGES (0x0A) rebuilt
//
// Axis convention: TLV is z-up; impl works in y-up. Swap at wrapper boundary
// (mirrors v7-boundary-classify.c / v7-wall-detect.c).

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-boundary-regularize-impl.h"
#include "v7-perbldg-gather.h"  // v7_per_bldg_load_frame (#72c Family B)
#include "v7-boundary-merge-impl.h"
#include "v7-boundary-corners-impl.h"
#include "v7-boundary-classify-impl.h"
#include "v7-corners-parse.h"
#include "v7-roof-graph-format.h"
#include "v7-derive-thresholds.h"

#define BLF_RECORD_BYTES 72

// ── Axis swap helpers (mirror v7-boundary-classify.c) ────────────────

static inline void axis_zup_to_yup_d(double p[3]) {
    const double x = p[0], y = p[1], z = p[2];
    p[0] = x; p[1] = z; p[2] = -y;
}

static inline void axis_yup_to_zup_d(double p[3]) {
    const double x = p[0], y = p[1], z = p[2];
    p[0] = x; p[1] = -z; p[2] = y;
}

static inline void axis_zup_to_yup_f(float p[3]) {
    const float x = p[0], y = p[1], z = p[2];
    p[0] = x; p[1] = z; p[2] = -y;
}

// ── Params ────────────────────────────────────────────────────────

static int parse_params(const char* params_str, v7_br_params_t* p) {
    v7_br_params_default(p);
    p->up_axis = 2;  // wrapper hands y-up data to impl, but result is z-up
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    #define BR_NUM(key, lo, hi, field) \
        if (json_number_or_default(json_object_get(obj, key), (lo), (hi), key, &p->field) != 0) \
            { json_decref(obj); return -1; }
    #define BR_INT(key, lo, hi, field) \
        if (json_int32_or_default(json_object_get(obj, key), (lo), (hi), key, &p->field) != 0) \
            { json_decref(obj); return -1; }
    #define BR_BOOL(key, field) do { json_t* j_ = json_object_get(obj, key); \
        if (j_) { if (!json_is_boolean(j_)) { json_decref(obj); return -1; } p->field = json_is_true(j_); } } while(0)
    BR_INT ("maxIterations",                  0, 1000000, max_iterations);
    BR_NUM ("nudgeRange",                     0.0,  100.0, nudge_range_m);
    BR_NUM ("nudgeStep",                      0.0,  100.0, nudge_step_m);
    BR_NUM ("angleSnapThreshold",             0.0,  180.0, angle_snap_thresh_deg);
    BR_BOOL("useWsdCost",                     use_wsd_cost);
    BR_NUM ("lambdaWSD",                      0.0, 1.0e6, lambda_wsd);
    BR_NUM ("wsdSigmaM",                      0.0, 1.0e6, wsd_sigma_m);
    BR_BOOL("useAngleCost",                   use_angle_cost);
    BR_NUM ("lambdaAngle",                    0.0, 1.0e6, lambda_angle);
    BR_NUM ("angleSigmaDeg",                  0.0,  180.0, angle_sigma_deg);
    BR_BOOL("useNeighborAngleCost",           use_neighbor_angle_cost);
    BR_NUM ("lambdaNeighborAngle",            0.0, 1.0e6, lambda_neighbor_angle);
    BR_BOOL("useHorizontalEdgeCost",          use_horizontal_edge_cost);
    BR_NUM ("lambdaHorizontalEdge",           0.0, 1.0e6, lambda_horizontal_edge);
    BR_NUM ("horizontalEdgeSigmaDeg",         0.0,  180.0, horizontal_edge_sigma_deg);
    BR_BOOL("useTrustWeighting",              use_trust_weighting);
    BR_BOOL("useExtendedAngles",              use_extended_angles);
    BR_BOOL("useProximityMerge",              use_proximity_merge);
    BR_NUM ("proximityMergeDist",             0.0,  100.0, proximity_merge_dist_m);
    BR_BOOL("useCollinearRemoval",            use_collinear_removal);
    BR_NUM ("collinearRemovalThresholdDeg",   0.0,  180.0, collinear_removal_threshold_deg);
    BR_BOOL("useRidgeMerge",                  use_ridge_merge);
    BR_NUM ("ridgeMergeDistM",                0.0,  100.0, ridge_merge_dist_m);
    BR_NUM ("ridgeMergeCoplanarM",            0.0,  100.0, ridge_merge_coplanar_m);
    BR_BOOL("useJointMoves",                  use_joint_moves);
    BR_INT ("jointSamplesPerEdge",            0, 1000000, joint_samples_per_edge);
    {
        int32_t seed_i32 = (int32_t)p->joint_move_seed;
        if (json_int32_or_default(json_object_get(obj, "jointMoveSeed"), 0, 2147483647,
                                    "jointMoveSeed", &seed_i32) != 0)
            { json_decref(obj); return -1; }
        p->joint_move_seed = (uint32_t)seed_i32;
    }
    BR_BOOL("useHorizontalCoplanarCost",      use_horiz_coplanar_cost);
    BR_NUM ("lambdaHorizontalCoplanar",       0.0, 1.0e6, lambda_horiz_coplanar);
    BR_NUM ("horizontalCoplanarSigmaM",       0.0,  100.0, horiz_coplanar_sigma_m);
    BR_BOOL("useVerticalCoplanarCost",        use_vert_coplanar_cost);
    BR_NUM ("lambdaVerticalCoplanar",         0.0, 1.0e6, lambda_vert_coplanar);
    BR_NUM ("verticalCoplanarSigmaM",         0.0,  100.0, vert_coplanar_sigma_m);
    BR_BOOL("useCollinearCost",               use_collinear_cost);
    BR_NUM ("lambdaCollinear",                0.0, 1.0e6, lambda_collinear);
    BR_NUM ("collinearSigmaM",                0.0,  100.0, collinear_sigma_m);
    BR_BOOL("wsdNormalize",                   wsd_normalize);
    BR_BOOL("cornerInsert",                   corner_insert);
    BR_NUM ("lambdaCompact",                  0.0,  1.0e6, lambda_compact);
    BR_BOOL("compactGate",                    compact_gate);
    BR_NUM ("compactGateEps",                 0.0,  100.0, compact_gate_eps);
    BR_INT ("costShape",                      0,        3, cost_shape);
    BR_INT ("r1AcceptanceStrategy",           0,        1, r1_acceptance_strategy);
    #undef BR_BOOL
    #undef BR_NUM
    #undef BR_INT
    json_decref(obj);
    return 0;
}

// ── Upstream parse (PLANES, RIDGES, CORNERS, EDGES, POLYGON_PATHS, NEARBY) ──

typedef struct upstream {
    v7_bm_input_plane_t*           planes;        size_t n_planes;
    v7_br_input_ridge_t*           ridges;        size_t n_ridges;
    v7_bm_input_corner_t*          corners;       size_t n_corners;
    v7_bm_input_edge_t*            edges;         size_t n_edges;
    v7_bm_input_polygon_path_t*    polygon_paths; size_t n_polygon_paths;
    v7_br_input_nearby_per_plane_t* nearby;       size_t n_nearby;
} upstream_t;

static void upstream_free(upstream_t* u) {
    if (!u) return;
    free(u->planes);
    free(u->ridges);
    if (u->corners) {
        for (size_t i = 0; i < u->n_corners; i++) {
            free(u->corners[i].plane_ids);
            free(u->corners[i].source_per_plane);
            free(u->corners[i].ridge_ids);
            free(u->corners[i].dirs);
            free(u->corners[i].com_bisectors);
            free(u->corners[i].members);
            free(u->corners[i].name);
        }
        free(u->corners);
    }
    if (u->edges) {
        for (size_t i = 0; i < u->n_edges; i++) free(u->edges[i].plane_ids);
        free(u->edges);
    }
    if (u->polygon_paths) {
        for (size_t i = 0; i < u->n_polygon_paths; i++) free(u->polygon_paths[i].corner_ids);
        free(u->polygon_paths);
    }
    if (u->nearby) {
        for (size_t i = 0; i < u->n_nearby; i++) free(u->nearby[i].items);
        free(u->nearby);
    }
    memset(u, 0, sizeof(*u));
}

// CORNERS reader — #72c Family D, extracted to v7-corners-parse.c
// (shared with classify).
static int parse_corners(rg_reader_t* r, upstream_t* u,
                          double cx, double cy, double cz) {
    return v7_parse_corners_section(r, &u->corners, &u->n_corners, cx, cy, cz);
}

// EDGES reader — same as classify's.
static int parse_edges(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_EDGES, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 16;
    u->edges = (v7_bm_input_edge_t*)safe_calloc_array(cap, sizeof(*u->edges));
    if (!u->edges) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 40) continue;
        if (u->n_edges == cap) {
            cap *= 2;
            v7_bm_input_edge_t* nb = (v7_bm_input_edge_t*)safe_realloc_array(u->edges, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_edges, 0, (cap - u->n_edges) * sizeof(*nb));
            u->edges = nb;
        }
        v7_bm_input_edge_t* E = &u->edges[u->n_edges++];
        memcpy(&E->id, rec + 0, 4);
        memcpy(&E->corner_a, rec + 8, 4);
        memcpy(&E->corner_b, rec + 12, 4);
        memcpy(E->direction, rec + 16, 12);
        memcpy(&E->length, rec + 28, 4);
        size_t off = 32;
        uint32_t pc; memcpy(&pc, rec + off, 4); off += 8;
        if (pc > 0) {
            E->plane_ids = (uint32_t*)safe_malloc_array(pc, sizeof(uint32_t));
            if (!E->plane_ids) return -1;
            memcpy(E->plane_ids, rec + off, pc * 4);
            E->n_plane_ids = pc;
        }
    }
    return 0;
}

static int parse_polygon_paths(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_POLYGON_PATHS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 16;
    u->polygon_paths = (v7_bm_input_polygon_path_t*)safe_calloc_array(cap, sizeof(*u->polygon_paths));
    if (!u->polygon_paths) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 8) continue;
        if (u->n_polygon_paths == cap) {
            cap *= 2;
            v7_bm_input_polygon_path_t* nb = (v7_bm_input_polygon_path_t*)safe_realloc_array(u->polygon_paths, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_polygon_paths, 0, (cap - u->n_polygon_paths) * sizeof(*nb));
            u->polygon_paths = nb;
        }
        v7_bm_input_polygon_path_t* P = &u->polygon_paths[u->n_polygon_paths++];
        memcpy(&P->plane_id,  rec + 0, 4);
        memcpy(&P->n_corners, rec + 4, 4);
        if (P->n_corners > 0) {
            P->corner_ids = (uint32_t*)safe_malloc_array(P->n_corners, sizeof(uint32_t));
            if (!P->corner_ids) return -1;
            memcpy(P->corner_ids, rec + 8, P->n_corners * 4);
        }
    }
    return 0;
}

// NEARBY_PROJECTIONS reader — copy of boundary-polygons.c's.
static int parse_nearby(rg_reader_t* r, upstream_t* u, double cx, double cy, double cz) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_NEARBY_PROJECTIONS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 8;
    u->nearby = (v7_br_input_nearby_per_plane_t*)safe_calloc_array(cap, sizeof(*u->nearby));
    if (!u->nearby) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 8) continue;
        if (u->n_nearby == cap) {
            cap *= 2;
            v7_br_input_nearby_per_plane_t* nb = (v7_br_input_nearby_per_plane_t*)safe_realloc_array(u->nearby, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_nearby, 0, (cap - u->n_nearby) * sizeof(*nb));
            u->nearby = nb;
        }
        v7_br_input_nearby_per_plane_t* N = &u->nearby[u->n_nearby++];
        memcpy(&N->plane_id, rec + 0, 4);
        memcpy(&N->n_items,  rec + 4, 4);
        if (N->n_items == 0) continue;
        N->items = (v7_br_input_nearby_item_t*)safe_calloc_array(N->n_items, sizeof(*N->items));
        if (!N->items) return -1;
        size_t off = 8;
        for (uint32_t k = 0; k < N->n_items; k++) {
            double pw[3];
            memcpy(&pw[0], rec + off + 0,  8);
            memcpy(&pw[1], rec + off + 8,  8);
            memcpy(&pw[2], rec + off + 16, 8);
            N->items[k].position[0] = pw[0] - cx;
            N->items[k].position[1] = pw[1] - cy;
            N->items[k].position[2] = pw[2] - cz;
            axis_zup_to_yup_d(N->items[k].position);
            memcpy(&N->items[k].signed_dist, rec + off + 24, 4);
            memcpy(&N->items[k].src_idx,     rec + off + 28, 4);
            off += 40;
        }
    }
    return 0;
}

static int parse_upstream_full(const uint8_t* tlv_bytes, size_t tlv_len,
                                 double cx, double cy, double cz,
                                 upstream_t* u) {
    memset(u, 0, sizeof(*u));
    rg_reader_t r;
    if (rg_reader_open(&r, tlv_bytes, tlv_len) != 0) return -1;
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_plane_record_t) && n > 0) {
            u->planes = (v7_bm_input_plane_t*)safe_calloc_array(n, sizeof(*u->planes));
            if (!u->planes) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_plane_record_t* P = (const rg_plane_record_t*)(base + (size_t)i * stride);
                u->planes[i].id        = P->id;
                u->planes[i].rejected  = (P->flags & RG_PLANE_REJECTED) != 0;
                u->planes[i].tilt_deg  = (double)P->tilt_deg;
                u->planes[i].plane_type = P->plane_type;  // authoritative type from topology
                u->planes[i].normal[0] = P->normal[0];
                u->planes[i].normal[1] = P->normal[1];
                u->planes[i].normal[2] = P->normal[2];
                // d transforms world→local in z-up frame (rotation-invariant
                // → same d in y-up local).
                u->planes[i].d         = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                axis_zup_to_yup_d(u->planes[i].normal);
                memcpy(u->planes[i].basis_e1, P->basis_e1, 24);
                memcpy(u->planes[i].basis_e2, P->basis_e2, 24);
            }
            u->n_planes = n;
        }
    }
    if (rg_reader_section(&r, RG_SECT_RIDGES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_ridge_record_t) && n > 0) {
            u->ridges = (v7_br_input_ridge_t*)safe_calloc_array(n, sizeof(*u->ridges));
            if (!u->ridges) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_ridge_record_t* R = (const rg_ridge_record_t*)(base + (size_t)i * stride);
                u->ridges[i].id       = R->id;
                u->ridges[i].plane_a  = R->plane_a;
                u->ridges[i].plane_b  = R->plane_b;
                u->ridges[i].type     = R->type;   // make-shared allowlist (I1)
                // Clipped endpoints: world z-up → building-local y-up, same
                // transform corners get (subtract centroid, swap axes), so
                // R.4 can gate corner positions against the ridge line.
                u->ridges[i].from[0] = R->from[0] - cx;
                u->ridges[i].from[1] = R->from[1] - cy;
                u->ridges[i].from[2] = R->from[2] - cz;
                axis_zup_to_yup_d(u->ridges[i].from);
                u->ridges[i].to[0]   = R->to[0] - cx;
                u->ridges[i].to[1]   = R->to[1] - cy;
                u->ridges[i].to[2]   = R->to[2] - cz;
                axis_zup_to_yup_d(u->ridges[i].to);
            }
            u->n_ridges = n;
        }
    }
    if (parse_corners(&r, u, cx, cy, cz)        != 0) return -1;
    if (parse_edges(&r, u)                       != 0) return -1;
    if (parse_polygon_paths(&r, u)               != 0) return -1;
    if (parse_nearby(&r, u, cx, cy, cz)          != 0) return -1;
    return 0;
}

// ── Re-emit CORNERS (mutated position, dead corners filtered) ────────
//
// Strategy: copy each upstream record byte-for-byte (preserves source,
// flags, plane_ids, source_per_plane, ridge_ids, name, dirs,
// com_bisectors, members tail), then overwrite the position bytes
// (offsets 8/16/24) with the regularize impl's updated position. Skip
// corners with alive=false.

static int reemit_corners(rg_writer_t* w,
                           const uint8_t* up_bytes, size_t up_len,
                           const v7_br_outputs_t* out,
                           double cx, double cy, double cz) {
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) return -1;
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(&r, RG_SECT_CORNERS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;

    // Build id → output index map.
    // n_dof and n_corner_positions correspond 1:1 to input corners.
    if (rg_writer_section_begin(w, RG_SECT_CORNERS, 0) != 0) return -1;
    size_t up_idx = 0;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 32 || up_idx >= out->n_corner_positions) { up_idx++; continue; }
        bool alive = (up_idx < out->n_dof) ? out->dof[up_idx].alive : true;
        if (!alive) { up_idx++; continue; }
        const v7_br_corner_pos_t* pos = &out->corner_positions[up_idx];
        // Updated position is impl's y-up building-local. Swap to z-up,
        // un-shift to world.
        double p_local[3] = { pos->point[0], pos->point[1], pos->point[2] };
        axis_yup_to_zup_d(p_local);
        double pw[3] = { p_local[0] + cx, p_local[1] + cy, p_local[2] + cz };

        // Decode the upstream plane block (offset 32) so we can splice in
        // R.2/R.4's grown plane set. Layout (mirrors parse_corners):
        //   [32] u32 n_plane_ids, [36] u32 pad, [40] u32 plane_ids[pc],
        //   then u8 source_per_plane[pc], then align8 to the ridge block.
        uint32_t old_pc = 0;
        if (rec_len >= 40) memcpy(&old_pc, rec + 32, 4);
        size_t old_plane_block = 8 + (size_t)old_pc * 4 + (size_t)old_pc;
        old_plane_block = (old_plane_block + 7) & ~(size_t)7;
        size_t tail_off = 32 + old_plane_block;   // start of ridge block onward

        // New plane set from R.2/R.4 union (parallel to corner_positions).
        const v7_br_corner_planes_t* cpl =
            (up_idx < out->n_corner_planes) ? &out->corner_planes[up_idx] : NULL;
        uint32_t new_pc = (cpl && cpl->n_plane_ids > 0) ? cpl->n_plane_ids : old_pc;
        const uint32_t* new_pids = (cpl && cpl->n_plane_ids > 0) ? cpl->plane_ids : NULL;

        if (new_pc == old_pc || tail_off > rec_len) {
            // No plane-count change (or malformed) → copy verbatim, just
            // overwrite position. (Plane ids themselves are stable unless
            // grown; a union that doesn't grow keeps original ordering.)
            uint8_t* nb = (uint8_t*)safe_calloc_array(rec_len, 1);
            if (!nb) return -1;
            memcpy(nb, rec, rec_len);
            memcpy(nb + 8,  &pw[0], 8);
            memcpy(nb + 16, &pw[1], 8);
            memcpy(nb + 24, &pw[2], 8);
            int rc = rg_writer_record(w, nb, rec_len);
            free(nb);
            if (rc != 0) return -1;
            up_idx++;
            continue;
        }

        // Plane count grew (R.2/R.4 merge). Rebuild: header[0..31] +
        // new plane block + verbatim tail (ridge_ids/name/dirs/com/members).
        size_t new_plane_block = 8 + (size_t)new_pc * 4 + (size_t)new_pc;
        new_plane_block = (new_plane_block + 7) & ~(size_t)7;
        size_t tail_len = rec_len - tail_off;
        size_t new_len  = 32 + new_plane_block + tail_len;
        uint8_t* nb = (uint8_t*)safe_calloc_array(new_len, 1);
        if (!nb) return -1;
        memcpy(nb, rec, 32);                       // id/source/flags/n_merged/point
        memcpy(nb + 8,  &pw[0], 8);
        memcpy(nb + 16, &pw[1], 8);
        memcpy(nb + 24, &pw[2], 8);
        size_t off = 32;
        memcpy(nb + off, &new_pc, 4); off += 8;    // count + pad
        for (uint32_t k = 0; k < new_pc; k++) {
            uint32_t pid = new_pids ? new_pids[k] : 0;
            memcpy(nb + off, &pid, 4); off += 4;
        }
        // source_per_plane[]: preserve upstream bytes for the original
        // planes; new (merged-in) planes get source 0 (unspecified). The
        // upstream source bytes start at rec + 40 + old_pc*4.
        if (rec_len >= 40 + (size_t)old_pc * 4 + old_pc) {
            const uint8_t* old_src = rec + 40 + (size_t)old_pc * 4;
            for (uint32_t k = 0; k < new_pc && k < old_pc; k++) nb[off + k] = old_src[k];
        }
        off = 32 + new_plane_block;
        memcpy(nb + off, rec + tail_off, tail_len);
        int rc = rg_writer_record(w, nb, new_len);
        free(nb);
        if (rc != 0) return -1;
        up_idx++;
    }

    // #25: corners MINTED by the impl (output index >= the upstream corner count)
    // have no upstream record, so the 1:1 loop above skipped them — without this
    // their ids dangle in POLYGON_PATHS (br-check dead_id_refs/edge_orphans). Emit
    // a fresh minimal record: header + plane block + empty ridge block, flags=0
    // (no name/dirs/com/members). Position: impl y-up local → z-up → +world.
    for (; up_idx < out->n_corner_positions; up_idx++) {
        if (up_idx < out->n_dof && !out->dof[up_idx].alive) continue;   // dropped insert
        const v7_br_corner_planes_t* cpl =
            (up_idx < out->n_corner_planes) ? &out->corner_planes[up_idx] : NULL;
        uint32_t pc = cpl ? cpl->n_plane_ids : 0;
        if (pc == 0) continue;   // no plane membership → nothing meaningful to emit
        double p_local[3] = { out->corner_positions[up_idx].point[0],
                              out->corner_positions[up_idx].point[1],
                              out->corner_positions[up_idx].point[2] };
        axis_yup_to_zup_d(p_local);
        double pw[3] = { p_local[0] + cx, p_local[1] + cy, p_local[2] + cz };
        size_t plane_block = 8 + (size_t)pc * 4 + (size_t)pc;
        plane_block = (plane_block + 7) & ~(size_t)7;
        size_t new_len = 32 + plane_block + 8;   // + ridge count(0)+pad; flags=0 ⇒ no tail
        uint8_t* nb = (uint8_t*)safe_calloc_array(new_len, 1);
        if (!nb) return -1;
        memcpy(nb + 0, &out->corner_positions[up_idx].corner_id, 4);   // id
        nb[4] = 4;   // source: concave-ish (informational); nb[5] flags = 0
        memcpy(nb + 8,  &pw[0], 8);
        memcpy(nb + 16, &pw[1], 8);
        memcpy(nb + 24, &pw[2], 8);
        memcpy(nb + 32, &pc, 4);          // n_plane_ids (offset 36 pad stays 0)
        size_t off = 40;
        for (uint32_t k = 0; k < pc; k++) { memcpy(nb + off, &cpl->plane_ids[k], 4); off += 4; }
        // source_per_plane[pc] left 0 (calloc); ridge count+pad at 32+plane_block are 0.
        int rc = rg_writer_record(w, nb, new_len);
        free(nb);
        if (rc != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── Re-emit POLYGON_PATHS (R.0b/R.2/R.3 mutations applied) ───────────
//
// The impl emits out->polygon_paths only for paths that changed; for
// unchanged planes we forward the upstream path. We index by plane_id
// because the count + ordering may differ post-mutation.

static int reemit_polygon_paths(rg_writer_t* w, const upstream_t* up,
                                  const v7_br_outputs_t* out) {
    if (rg_writer_section_begin(w, RG_SECT_POLYGON_PATHS, 0) != 0) return -1;
    for (size_t i = 0; i < up->n_polygon_paths; i++) {
        const v7_bm_input_polygon_path_t* upP = &up->polygon_paths[i];
        const v7_br_polygon_path_t* outP = NULL;
        for (size_t j = 0; j < out->n_polygon_paths; j++) {
            if (out->polygon_paths[j].plane_id == upP->plane_id) {
                outP = &out->polygon_paths[j]; break;
            }
        }
        uint32_t plane_id, n_corners;
        const uint32_t* corner_ids;
        if (outP) {
            plane_id = outP->plane_id;
            n_corners = outP->n_corners;
            corner_ids = outP->corner_ids;
        } else {
            plane_id = upP->plane_id;
            n_corners = upP->n_corners;
            corner_ids = upP->corner_ids;
        }
        // Drop degenerate polygons (less than 3 corners — killed by R.2/R.3).
        if (n_corners < 3) continue;
        size_t rec_len = 8 + (size_t)n_corners * 4;
        rec_len = (rec_len + 7) & ~(size_t)7;
        uint8_t* nb = (uint8_t*)safe_calloc_array(rec_len, 1);
        if (!nb) return -1;
        memcpy(nb + 0, &plane_id,   4);
        memcpy(nb + 4, &n_corners,  4);
        if (n_corners > 0) memcpy(nb + 8, corner_ids, (size_t)n_corners * 4);
        int rc = rg_writer_record(w, nb, rec_len);
        free(nb);
        if (rc != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── Re-emit EDGES (rebuilt from post-merge polygon paths) ────────────
//
// R.2/R.3/R.4 mutate the polygon paths (merged corners → shared corner
// ids). The upstream EDGES section from boundary-polygons-2 still
// references the PRE-merge corner ids and so is stale (and misses the
// new shared edges R.4 creates). Rebuild EDGES the same way
// boundary-polygons-2's emit_edges does: walk every final polygon path,
// accumulate (corner_a<corner_b) edges, union the visiting plane_ids.
// An edge visited by >=2 planes ⇒ n_plane_ids>=2 ⇒ interior-shared
// downstream. Positions come from out->corner_positions (y-up local →
// z-up world) to match the upstream dir/length convention.

typedef struct edge_rb {
    uint32_t corner_a, corner_b;
    double   dir[3]; double length;
    uint32_t* plane_ids; uint32_t n_plane_ids; uint32_t cap_plane_ids;
} edge_rb_t;

// Final position (world z-up) for a corner id, from out->corner_positions.
static bool final_corner_pos_world(const v7_br_outputs_t* out, uint32_t id,
                                    double cx, double cy, double cz, double pw[3]) {
    for (size_t i = 0; i < out->n_corner_positions; i++) {
        if (out->corner_positions[i].corner_id != id) continue;
        double p[3] = { out->corner_positions[i].point[0],
                        out->corner_positions[i].point[1],
                        out->corner_positions[i].point[2] };
        axis_yup_to_zup_d(p);
        pw[0] = p[0] + cx; pw[1] = p[1] + cy; pw[2] = p[2] + cz;
        return true;
    }
    return false;
}

static int reemit_edges(rg_writer_t* w, const upstream_t* up,
                        const v7_br_outputs_t* out,
                        double cx, double cy, double cz) {
    edge_rb_t* edges = NULL; size_t n_edges = 0, cap = 0;
    int rc = 0;

    for (size_t i = 0; i < up->n_polygon_paths; i++) {
        const v7_bm_input_polygon_path_t* upP = &up->polygon_paths[i];
        const v7_br_polygon_path_t* outP = NULL;
        for (size_t j = 0; j < out->n_polygon_paths; j++) {
            if (out->polygon_paths[j].plane_id == upP->plane_id) { outP = &out->polygon_paths[j]; break; }
        }
        uint32_t plane_id   = outP ? outP->plane_id   : upP->plane_id;
        uint32_t n_corners  = outP ? outP->n_corners  : upP->n_corners;
        const uint32_t* ids = outP ? outP->corner_ids : upP->corner_ids;
        if (n_corners < 3) continue;
        for (uint32_t k = 0; k < n_corners; k++) {
            uint32_t a = ids[k], b = ids[(k + 1) % n_corners];
            if (a == b) continue;
            uint32_t aa = a < b ? a : b, bb = a < b ? b : a;
            edge_rb_t* E = NULL;
            for (size_t e = 0; e < n_edges; e++)
                if (edges[e].corner_a == aa && edges[e].corner_b == bb) { E = &edges[e]; break; }
            if (!E) {
                if (n_edges == cap) {
                    cap = cap ? cap * 2 : 64;
                    edge_rb_t* nb = (edge_rb_t*)realloc(edges, cap * sizeof(*edges));
                    if (!nb) { rc = -1; goto cleanup; }
                    edges = nb;
                }
                E = &edges[n_edges++];
                memset(E, 0, sizeof(*E));
                E->corner_a = aa; E->corner_b = bb;
                double pa[3], pb[3];
                if (final_corner_pos_world(out, aa, cx, cy, cz, pa) &&
                    final_corner_pos_world(out, bb, cx, cy, cz, pb)) {
                    double dx = pb[0]-pa[0], dy = pb[1]-pa[1], dz = pb[2]-pa[2];
                    double len = sqrt(dx*dx + dy*dy + dz*dz);
                    E->length = len;
                    if (len > 0) { E->dir[0]=dx/len; E->dir[1]=dy/len; E->dir[2]=dz/len; }
                }
            }
            bool present = false;
            for (uint32_t p = 0; p < E->n_plane_ids; p++)
                if (E->plane_ids[p] == plane_id) { present = true; break; }
            if (!present) {
                if (E->n_plane_ids == E->cap_plane_ids) {
                    E->cap_plane_ids = E->cap_plane_ids ? E->cap_plane_ids * 2 : 4;
                    uint32_t* nb = (uint32_t*)realloc(E->plane_ids, E->cap_plane_ids * sizeof(uint32_t));
                    if (!nb) { rc = -1; goto cleanup; }
                    E->plane_ids = nb;
                }
                E->plane_ids[E->n_plane_ids++] = plane_id;
            }
        }
    }

    if (rg_writer_section_begin(w, RG_SECT_EDGES, 0) != 0) { rc = -1; goto cleanup; }
    for (size_t i = 0; i < n_edges; i++) {
        const edge_rb_t* E = &edges[i];
        uint32_t id = (uint32_t)i;
        size_t pids_block = 8 + (size_t)E->n_plane_ids * 4;
        pids_block = (pids_block + 7) & ~(size_t)7;
        size_t rec_len = 32 + pids_block;
        uint8_t* rec = (uint8_t*)safe_calloc_array(rec_len, 1);
        if (!rec) { rc = -1; break; }
        memcpy(rec + 0,  &id, 4);
        memcpy(rec + 8,  &E->corner_a, 4);
        memcpy(rec + 12, &E->corner_b, 4);
        float dir_f[3] = { (float)E->dir[0], (float)E->dir[1], (float)E->dir[2] };
        memcpy(rec + 16, dir_f, 12);
        float len_f = (float)E->length;
        memcpy(rec + 28, &len_f, 4);
        uint32_t pcount = E->n_plane_ids;
        memcpy(rec + 32, &pcount, 4);
        for (uint32_t p = 0; p < E->n_plane_ids; p++)
            memcpy(rec + 40 + (size_t)p * 4, &E->plane_ids[p], 4);
        int wrc = rg_writer_record(w, rec, rec_len);
        free(rec);
        if (wrc != 0) { rc = -1; break; }
    }
    if (rc == 0) rc = rg_writer_section_end(w);

cleanup:
    for (size_t i = 0; i < n_edges; i++) free(edges[i].plane_ids);
    free(edges);
    return rc;
}

// ── br_check_consistency (T0, M3) — gated debug invariant ───────────
//
// Asserts, over the EMITTED roof-graph TLV (one building), the P2⇔P3
// consistency invariant + dead-id + EDGES⇔POLYGON_PATHS consistency:
//   (1) for every corner present in some POLYGON_PATH (alive && emitted),
//       its CORNERS plane set (P2, deduped) == {plane_id of each emitted
//       path that contains it} (P3-derived). Reports P2⊋P3 (and P3⊋P2).
//   (2) no POLYGON_PATH references a corner id absent from CORNERS (dead id).
//   (3) every EDGES corner id appears in some POLYGON_PATH, and vice-versa
//       (the EDGES⇔POLYGON_PATHS corner-id set match).
// Reads POLYGON_PATHS via RG_SECT_POLYGON_PATHS (0x0B). Env-gated by
// BR_CHECK — never changes release behaviour (read-only; only prints).
// Accumulates global counts via out-params so the caller can total them.
//
// Returns 0 always (diagnostic only); on a mismatch it prints [br-check].
typedef struct br_check_tally {
    uint64_t buildings;
    uint64_t dead_id_refs;     // path corner ids absent from CORNERS
    uint64_t p2_superset_p3;   // corners whose P2 ⊋ P3 (a plane in P2 but not P3)
    uint64_t p3_superset_p2;   // corners whose P3 ⊋ P2 (a path-plane not in P2)
    uint64_t edge_orphans;     // edge corner ids not in any path
} br_check_tally_t;

// Small dynamic u32 set (sorted-unique via linear ops; corner counts are tiny).
static bool u32set_has(const uint32_t* a, uint32_t n, uint32_t v) {
    for (uint32_t i = 0; i < n; i++) if (a[i] == v) return true;
    return false;
}

static int br_check_consistency(const uint8_t* tlv, size_t tlv_len,
                                size_t bi, br_check_tally_t* tally) {
    rg_reader_t r;
    if (rg_reader_open(&r, tlv, tlv_len) != 0) return 0;
    const uint8_t* sect; size_t slen;

    // ── Collect CORNERS: id → plane set (P2). Build id→idx for membership. ──
    uint32_t* cids = NULL; uint32_t** cp = NULL; uint32_t* cpn = NULL;
    size_t n_c = 0, cap_c = 0;
    if (rg_reader_section(&r, RG_SECT_CORNERS, &sect, &slen) == 0) {
        rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
        const uint8_t* rec; uint32_t rl;
        while (rg_record_iter_next(&it, &rec, &rl)) {
            if (rl < 32) continue;
            if (n_c == cap_c) {
                cap_c = cap_c ? cap_c * 2 : 64;
                cids = (uint32_t*)realloc(cids, cap_c * sizeof(uint32_t));
                cp   = (uint32_t**)realloc(cp, cap_c * sizeof(uint32_t*));
                cpn  = (uint32_t*)realloc(cpn, cap_c * sizeof(uint32_t));
                if (!cids || !cp || !cpn) goto done;
            }
            uint32_t id; memcpy(&id, rec + 0, 4);
            uint32_t pc = 0; if (rl >= 40) memcpy(&pc, rec + 32, 4);
            uint32_t* pids = NULL;
            if (pc > 0 && rl >= 40 + (size_t)pc * 4) {
                pids = (uint32_t*)malloc(pc * sizeof(uint32_t));
                if (!pids) goto done;
                memcpy(pids, rec + 40, pc * 4);
            } else pc = 0;
            cids[n_c] = id; cp[n_c] = pids; cpn[n_c] = pc; n_c++;
        }
    }
    #define CIDX(id, out_idx) do { (out_idx) = UINT32_MAX; \
        for (size_t _i = 0; _i < n_c; _i++) if (cids[_i] == (id)) { (out_idx) = (uint32_t)_i; break; } } while (0)

    // ── Derive P3: per-corner set of planes whose path contains it. ──
    uint32_t** p3 = (uint32_t**)calloc(n_c ? n_c : 1, sizeof(uint32_t*));
    uint32_t*  p3n = (uint32_t*)calloc(n_c ? n_c : 1, sizeof(uint32_t));
    uint32_t*  p3cap = (uint32_t*)calloc(n_c ? n_c : 1, sizeof(uint32_t));
    bool* in_path = (bool*)calloc(n_c ? n_c : 1, sizeof(bool));
    if (!p3 || !p3n || !p3cap || !in_path) goto done2;

    uint64_t dead_here = 0;
    if (rg_reader_section(&r, RG_SECT_POLYGON_PATHS, &sect, &slen) == 0) {
        rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
        const uint8_t* rec; uint32_t rl;
        while (rg_record_iter_next(&it, &rec, &rl)) {
            if (rl < 8) continue;
            uint32_t plane_id, nco;
            memcpy(&plane_id, rec + 0, 4); memcpy(&nco, rec + 4, 4);
            if ((size_t)8 + (size_t)nco * 4 > rl) continue;
            for (uint32_t k = 0; k < nco; k++) {
                uint32_t cid; memcpy(&cid, rec + 8 + (size_t)k * 4, 4);
                uint32_t idx; CIDX(cid, idx);
                if (idx == UINT32_MAX) { dead_here++; continue; }  // dead-id ref
                in_path[idx] = true;
                if (!u32set_has(p3[idx], p3n[idx], plane_id)) {
                    if (p3n[idx] == p3cap[idx]) {
                        p3cap[idx] = p3cap[idx] ? p3cap[idx] * 2 : 4;
                        p3[idx] = (uint32_t*)realloc(p3[idx], p3cap[idx] * sizeof(uint32_t));
                        if (!p3[idx]) goto done2;
                    }
                    p3[idx][p3n[idx]++] = plane_id;
                }
            }
        }
    }
    tally->dead_id_refs += dead_here;

    // ── (1) P2⇔P3 over corners that ARE in some path. ──
    uint64_t p2sup = 0, p3sup = 0;
    for (size_t i = 0; i < n_c; i++) {
        if (!in_path[i]) continue;  // out of domain (legitimately dead set)
        // P2 ⊋ P3: a plane in CORNERS but no path of that plane holds it.
        for (uint32_t k = 0; k < cpn[i]; k++)
            if (!u32set_has(p3[i], p3n[i], cp[i][k])) { p2sup++; break; }
        // P3 ⊋ P2: a path-plane not recorded in CORNERS plane set.
        for (uint32_t k = 0; k < p3n[i]; k++)
            if (!u32set_has(cp[i], cpn[i], p3[i][k])) { p3sup++; break; }
    }
    tally->p2_superset_p3 += p2sup;
    tally->p3_superset_p2 += p3sup;

    // ── (3) EDGES ⇔ POLYGON_PATHS corner-id consistency. ──
    uint64_t edge_orphans = 0;
    if (rg_reader_section(&r, RG_SECT_EDGES, &sect, &slen) == 0) {
        rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
        const uint8_t* rec; uint32_t rl;
        while (rg_record_iter_next(&it, &rec, &rl)) {
            if (rl < 32) continue;
            uint32_t ca, cb; memcpy(&ca, rec + 8, 4); memcpy(&cb, rec + 12, 4);
            uint32_t ia, ib; CIDX(ca, ia); CIDX(cb, ib);
            if (ia == UINT32_MAX || !in_path[ia]) edge_orphans++;
            else if (ib == UINT32_MAX || !in_path[ib]) edge_orphans++;
        }
    }
    tally->edge_orphans += edge_orphans;
    tally->buildings++;

    if (dead_here || p2sup || p3sup || edge_orphans) {
        fprintf(stderr,
            "[br-check] bldg %zu: dead_id_refs=%" PRIu64 " P2sup=%" PRIu64
            " P3sup=%" PRIu64 " edge_orphans=%" PRIu64 " (corners=%zu)\n",
            bi, dead_here, p2sup, p3sup, edge_orphans, n_c);
    }

done2:
    if (p3) { for (size_t i = 0; i < n_c; i++) free(p3[i]); free(p3); }
    free(p3n); free(p3cap); free(in_path);
done:
    if (cp) { for (size_t i = 0; i < n_c; i++) free(cp[i]); free(cp); }
    free(cids); free(cpn);
    #undef CIDX
    return 0;
}

// ── Process one building ──────────────────────────────────────────

static int process_bldg(rg_writer_t* w,
                         const uint8_t* up_bytes, size_t up_len,
                         double cx, double cy, double cz,
                         const float* local_pos_yup, size_t n_pts,
                         const int16_t* owner_local,
                         const float* residual_local,
                         double pp_dist_threshold,
                         const v7_br_params_t* params) {
    upstream_t up = {0};
    if (parse_upstream_full(up_bytes, up_len, cx, cy, cz, &up) != 0) {
        upstream_free(&up);
        return -1;
    }
    v7_br_outputs_t out;
    v7_br_outputs_init(&out);
    int compute_rc = v7_br_compute(
        up.planes,        up.n_planes,
        up.corners,       up.n_corners,
        up.edges,         up.n_edges,
        up.polygon_paths, up.n_polygon_paths,
        up.ridges,        up.n_ridges,
        up.nearby,        up.n_nearby,
        local_pos_yup,    n_pts,
        owner_local,
        residual_local,
        pp_dist_threshold,
        params, &out);
    if (compute_rc != 0) { upstream_free(&up); v7_br_outputs_free(&out); return -1; }

    // Pass through upstream except CORNERS + POLYGON_PATHS + EDGES, then
    // re-emit. EDGES is rebuilt from the post-merge polygon paths so R.2/
    // R.3/R.4's merges don't leave edges referencing dead corner ids, and
    // R.4's unified ridge corners surface as shared (n_plane_ids>=2) edges.
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) {
        upstream_free(&up); v7_br_outputs_free(&out); return -1;
    }
    const uint32_t skips[] = { RG_SECT_CORNERS, RG_SECT_POLYGON_PATHS, RG_SECT_EDGES };
    if (rg_writer_passthrough_n(w, &r, skips, 3) != 0) {
        upstream_free(&up); v7_br_outputs_free(&out); return -1;
    }
    if (reemit_corners(w, up_bytes, up_len, &out, cx, cy, cz) != 0 ||
        reemit_polygon_paths(w, &up, &out)                     != 0 ||
        reemit_edges(w, &up, &out, cx, cy, cz)                 != 0) {
        upstream_free(&up); v7_br_outputs_free(&out); return -1;
    }
    upstream_free(&up); v7_br_outputs_free(&out);
    return 0;
}

// ── Exec ──────────────────────────────────────────────────────────

static int v7_boundary_regularize_exec(step_context_t* ctx) {
    v7_br_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-boundary-regularize scope=tile "
                "max_iter=%d nudge_range=%.4f nudge_step=%.4f angle_snap_thresh_deg=%.2f\n",
                params.max_iterations, params.nudge_range_m,
                params.nudge_step_m, params.angle_snap_thresh_deg);
    }

    // Tile-wide positions (F64×3·N).
    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) {
        fprintf(stderr, "[v7-boundary-regularize] missing/malformed lidar-positions\n");
        return -1;
    }
    const size_t total_points = pos_bytes / (3 * sizeof(double));

    const size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-local-frames")         != n_buildings ||
        step_array_len(ctx, "boundary-polygons-roof-graph")  != n_buildings ||
        step_array_len(ctx, "grm-owner")                      != n_buildings ||
        step_array_len(ctx, "grm-residual")                   != n_buildings ||
        step_array_len(ctx, "bldg-thresholds")                != n_buildings) {
        fprintf(stderr, "[v7-boundary-regularize] count mismatch across per-bldg arrays\n");
        return -1;
    }

    blob_t b_roof = {0};
    int rc = 0;
    float*  local_pos = NULL;  size_t local_pos_cap = 0;

    // T0: env-gated consistency checker (M3). Read-only; never alters output.
    const bool br_check_on = (getenv("BR_CHECK") != NULL);
    br_check_tally_t br_tally = {0};

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // Per-bldg record + local-frame parse (task #72c Family B helper).
        const uint32_t* gidx = NULL;
        uint32_t n_pts = 0;
        double cx, cy, cz;
        if (v7_per_bldg_load_frame(ctx, bi, &gidx, &n_pts, &cx, &cy, &cz) != 0) goto skip_bldg;

        const uint8_t* up_bytes = NULL; size_t up_len = 0;
        if (step_array_read_bytes(ctx, "boundary-polygons-roof-graph", bi, &up_bytes, &up_len) != 0
            || !up_bytes || up_len < 32) goto skip_bldg;

        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts * sizeof(int16_t)) goto skip_bldg;
        const int16_t* own16 = (const int16_t*)own_bytes;

        const uint8_t* res_bytes = NULL; size_t res_len = 0;
        const float* residual_local = NULL;
        if (step_array_read_bytes(ctx, "grm-residual", bi, &res_bytes, &res_len) == 0
            && res_bytes && res_len == (size_t)n_pts * sizeof(float)) {
            residual_local = (const float*)res_bytes;
        }

        const uint8_t* th_bytes = NULL; size_t th_len = 0;
        if (step_array_read_bytes(ctx, "bldg-thresholds", bi, &th_bytes, &th_len) != 0
            || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) goto skip_bldg;
        v7_bldg_thresholds_t th; memcpy(&th, th_bytes, sizeof(th));
        const double pp_dist_threshold = th.dist_threshold_grm;

        // Subset tile positions to per-bldg in y-up local frame.
        if ((size_t)n_pts * 3 > local_pos_cap) {
            float* nb = (float*)safe_realloc_array(local_pos, (size_t)n_pts * 3, sizeof(float));
            if (!nb) { rc = -1; break; }
            local_pos = nb; local_pos_cap = (size_t)n_pts * 3;
        }
        for (uint32_t i = 0; i < n_pts; i++) {
            const uint32_t g = gidx[i];
            if ((size_t)g >= total_points) {
                local_pos[3 * i + 0] = local_pos[3 * i + 1] = local_pos[3 * i + 2] = 0.0f;
            } else {
                local_pos[3 * i + 0] = (float)(positions[3 * g + 0] - cx);
                local_pos[3 * i + 1] = (float)(positions[3 * g + 1] - cy);
                local_pos[3 * i + 2] = (float)(positions[3 * g + 2] - cz);
                axis_zup_to_yup_f(&local_pos[3 * i]);
            }
        }

        rg_writer_t w; rg_writer_init(&w);
        int prc = process_bldg(&w, up_bytes, up_len, cx, cy, cz,
                                local_pos, (size_t)n_pts, own16,
                                residual_local, pp_dist_threshold, &params);
        if (prc != 0) {
            rg_writer_free(&w);
            blob_push(&b_roof, NULL, 0);
            continue;
        }
        uint8_t* tlv_bytes = NULL; size_t tlv_len = 0;
        if (rg_writer_finalize(&w, &tlv_bytes, &tlv_len) != 0) {
            rg_writer_free(&w);
            blob_push(&b_roof, NULL, 0);
            continue;
        }
        rg_writer_free(&w);
        if (br_check_on) br_check_consistency(tlv_bytes, tlv_len, bi, &br_tally);
        if (blob_push(&b_roof, tlv_bytes, tlv_len) != 0) {
            free(tlv_bytes); rc = -1; break;
        }
        free(tlv_bytes);
        continue;

skip_bldg:
        blob_push(&b_roof, NULL, 0);
    }

    if (br_check_on) {
        fprintf(stderr,
            "[br-check] TOTAL over %" PRIu64 " bldgs: dead_id_refs=%" PRIu64
            " P2sup=%" PRIu64 " P3sup=%" PRIu64 " edge_orphans=%" PRIu64 "\n",
            br_tally.buildings, br_tally.dead_id_refs, br_tally.p2_superset_p3,
            br_tally.p3_superset_p2, br_tally.edge_orphans);
    }

    if (rc == 0) rc = emit_blob(ctx, "boundary-regularize-roof-graph", &b_roof);
    blob_free(&b_roof);
    free(local_pos);
    return rc;
}

// ── Step descriptor ───────────────────────────────────────────────

static const step_read_t v7_boundary_regularize_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",           .array = true},
    {.name = "building-local-frames",          .array = true},
    {.name = "boundary-polygons-roof-graph",   .array = true},
    {.name = "grm-owner",                       .array = true},
    {.name = "grm-residual",                    .array = true},
    {.name = "bldg-thresholds",                 .array = true},
};

static const step_write_t v7_boundary_regularize_writes[] = {
    {.name = "boundary-regularize-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_regularize_def = {
    .name             = "v7-boundary-regularize",
    .version          = 19,  // v19 (type-decouple, 2026-07-07): refuse cross-type corner merges — T5 cross-plane weld + T7 split-to-share now gate on plane_type; an allow-listed ridge/hip/valley can still span roof↔wall/ground, and the ridge-type gate alone let 7 cross-type corners through regularize on dahouet. Pairs with the upstream corner-pool decouple. v18 (in-loop corner insertion #25 — cache-bust re-bump, 2026-07-01): boundary-regularize can now ADD corners mid-optimization to recover under-built polygons. A per-edge fit-misfit metric (signed Σw·dist of owner-points-outside charged to each polygon edge, via pt_nearest_edge_uv) finds the worst-fitting edge; a corner is minted at the edge's uncovered-mass centroid (uv_to_3d), spliced into the ring, RETAG_DOF-tagged DOF-2, then refined by the descent over subsequent iterations. Interleaved (not a static post-pass) — a periodic in-loop sweep inserts one corner per under-covered plane per pass (self-int-guarded, per-plane budget, FATAL abort past the N_INSERT_MAX buffer cap). GROWTH: corner-parallel arrays over-sized to n_cap=n_corners+64, corners_in rebound to a mutable copy; the wrapper appends fresh CORNERS records for minted ids (they have no upstream record) so paths/edges resolve. On dahouet the terrain ring plane 0 goes 67%->2% uncovered (own_out 12364->12, +5 corners), gate green. Gated BR_CORNER_INSERT (default ON; =0 disables). Known: slight coverage drift on a few non-inserted planes (needs a strict cost-drop guard) + per-insert REBUILD cost. Prototype — NOT yet committed-quality; BEHAVIOUR CHANGE -> cache invalidates. v16 (snap-in position-snap, 2026-06-25): the T4 snap-in tagged a corner on-ridge + set DOF-1 when it drifted within tau_in perp, but never SET its position ONTO the crease — DOF-1 only slides PARALLEL, so a tagged corner could sit up to tau_in (0.30m) off the ridge (design §2: "position lies on the crease, set at every snap event" — the position half was missing). Now the snap-in PROJECTS the corner onto its single binding ridge (ridge_dof_for_corner for the bind + ridge_proj for the foot, clamped to the clipped segment) when it resolves to DOF-1; DOF-0 junctions (placed by merge/RI) untouched; ctx/pw caches rebuilt after the move. Gated BR_SNAP_POS (default ON; =0 reverts to tag-only v15). Fixes off-ridge shared corners that produced skewed/duplicate edges along a ridge (dahouet P2C11 @25cm off the 2-11 hip). BEHAVIOUR CHANGE -> cache invalidates. v15 (joint-move merge chase, 2026-06-24): the R.2/R.3 chase (CHASE_AFTER_MOVE — proximity-merge + collinear-drop) now runs on BOTH endpoints of an accepted JOINT two-corner move. Previously only SINGLE-corner moves chased; joint moves skipped it (a deferred "5.7g.4" TODO), so corners nudged close/coincident by a joint move were never merged and there is no final merge sweep — leaving duplicate/near-duplicate vertices (measured on dahouet: P4X5/P4C12 @3.3cm, RE19+RE34/P6C9 @6cm, and the COINCIDENT RE33+RE71 / P6C12+P24CP6 @0.1mm, all same-plane and R.2-eligible). On a topology change the now-stale joint_edges[] is rebuilt (joint_dirty) and the rest of the list is skipped that iteration. BEHAVIOUR CHANGE (more merges) → cache invalidates. v14 (coplanar DOF exclusion + rename, 2026-06-20): ridge type 4 renamed "flashing"→"coplanar" (it was a misnomer — real flashing is roof–wall = type 5 "step"; type 4 = two near-coplanar planes, dihedral <10°, whose crease line nA×nB is ill-conditioned). BEHAVIOUR: ridge_dof_for_corner now SKIPS type-4 (coplanar) ridges — a degenerate near-coplanar seam must NOT constrain DOF (slide-axis is noise; pinning via it spuriously locks). Type-4-ONLY exclusion, NOT RIDGE_TYPE_ALLOWED: step (type 5) is a real well-conditioned eave crease that legitimately pins DOF. On dahouet the lone coplanar ridge is a false ground-plane seam affecting ~1 corner (DOF un-pins). The 10° threshold is now a named const (COPLANAR_MAX_ANGLE_DEG, topology), documented as deliberately equal to the GRM merge angle. DOF change → cache invalidates. v13 (T7 SPLIT_TO_SHARE_CORNER, 2026-06-19): the only GROWING make-shared op. When a single-plane DOF-1 corner cA on an allow-listed ridge R lies ON a B-plane path-EDGE (u,v) whose BOTH endpoints carry ridge_id R, cA's EXISTING id is INSERTED into polys[B] between u,v → cA becomes shared {A,B} (cA does NOT move; the path-insert IS the membership update — Phase B). Wired in CHASE_AFTER_MOVE AFTER MERGE_CORNER, BEFORE T6/DROP_COLINEAR (§4.1 priority). compute-then-commit (§4.3c): PREPARE re-confirms the hardened cond-A (single-plane, on R, both-R B-edge bracket + PERP-to-segment ≤ τ_in + 3D-nearest disambiguation + folded-ridge tie-REFUSE + M4(b) ≥3-plane junction-REFUSE) and the M5 self-int reject in B's UV (cA INSERTED), then reserves capacity; COMMIT (memmove+write+n++) is allocation-free/infallible. Growth-safety: poly_reserve now HONORS poly_t.cap (was set-but-unread); ctxs[] gains a cap_ctx companion + ctx_reserve, and REBUILD_CTXS gained a defensive CTX_RESERVE bound-check (a SPLIT's +1 membership overflowed the once-sized pool). n_split_share re-pointed from the read-only post-loop census to the REAL op (no double-count); the census is preview-only now. Gated by BR_NO_SPLIT (default ON; =1 reverts to no-split for A/B). Expected ~2 splits (cond-A) on dahouet. Path/membership growth → cache invalidates. v12 (L1 orphan-kill, 2026-06-19): kill_orphaned_corners() marks dead any still-alive corner in NO live polygon path (a bystander left when a MERGE dedup / R.3 / T6 splice collapsed its last ring to n<3 — the corner-197 class). Such corners are already inert in-loop (path-derived membership skips them everywhere) but were EMITTED alive with stale data; now swept post-pre-loop-batch AND post-loop (before the out->dof[].alive emit), so the wrapper's alive-filter drops them. Output change (orphans no longer emitted) → cache invalidates. v11 (T6, 2026-06-19): DROP_REDUNDANT_ON_RIDGE — all-polys-agree ridge-cleanup. ONE helper (try_drop_redundant_on_ridge) used in TWO places. (1) PRE-LOOP reimplemented corner-centric (was a per-path pass): removes a redundant on-ridge midpoint ONLY when EVERY polygon containing it agrees (3-consecutive-share-ONE-ridge), splicing it from ALL atomically. BEHAVIOUR CHANGE vs v10 — the old per-path pass dropped a shared corner from one plane asymmetrically, which can destroy a shared edge another plane needs (§4.2: a corner straight in one polygon can be the ENDPOINT of a shared edge in another); the all-polys-agree rule KEEPS such corners. Re-sited AFTER the cs[].ridge_ids seed so pre-loop + in-loop share ONE tag source (the maintained cs[].ridge_ids, not corners_in[].ridge_ids) and ONE path store (polys[]); still a genuine PRE-pass (before any corner-moving mutation). (2) IN-LOOP wired in CHASE_AFTER_MOVE AFTER MERGE_CORNER and BEFORE DROP_COLINEAR (MERGE→T6→DROP cascade priority) — on a drop, chase_ci is repointed to a surviving neighbour + RETAG_DOF/REBUILD_CTXS/REBUILD_PW_EDGES. Kink-safe (a junction with >=2 ridge tags fails the share-ONE test → kept); DROP_COLINEAR still gated planes_of_n!=1 so it can't eat a shared corner. +0 on dahouet (the pre-loop sweep already forms the shared edges; in-loop fires when snap-in/T7 create mid-loop redundancies) but the all-polys-agree rule PREVENTS some asymmetric pre-loop drops (shared corners now kept) and is needed for T7. Path/membership change → cache invalidates. v10 (Phase B membership SSoT, 2026-06-19): polygon PATHS are the single source of truth for per-corner plane membership. planes_dyn (the stored, separately-maintained copy) is DELETED and replaced by planes_of(ci) — a pure path-scan (= the old RECONCILE_PLANES_DOWN body). Removed: the plane_ids seed+intersect, RECONCILE_PLANES_DOWN, MERGE union-up, R.2/R.3 inline unions. plane_ids (GEOMETRIC "lies on surface", ≤0.12m, a superset of path-membership — NOT a bug, see §2.4 DECISION UPDATE 2026-06-19) is no longer read anywhere in regularize. The R.0 DOF tag is now TOPOLOGICAL (path membership) not geometric n_plane_ids — fixing a spurious over-lock (corners coplanar with a FAR plane were tagged junctions and frozen) and moving ~111 corners on dahouet (optimizer-basin amplification of ~4 genuine coupled-DOF changes; A2 ridge-coupling re-locks 32/36 raw diffs, MIN-only). Also fixes the orphan stale-claim (a corner alive but in no path now reads membership 0, not a stale 1). The P2⇔P3 check's membership halves are now TRUE BY CONSTRUCTION; the check is kept for dead-id-in-path + edges⇔paths. Membership/DOF change → cache invalidates. v9 (T5, 2026-06-18): MERGE_CORNER cross-plane weld REPLACES the legacy R.4 (§4.0/§4.1). The R4_RIDGE_MERGE macro became MERGE_CORNER_XPLANE: (1) TYPE-GATE — only {ridge,hip,valley} weld (RIDGE_TYPE_ALLOWED, I1 §9); step (roof–wall eave) + flashing (degenerate) EXCLUDED, fixing an R.4 bug that ignored rg[].type and welded eaves. (2) POSITION via the constraint-aware corner-pool solver (v7p_solve_member_position, T1) — NOT a midpoint: a transient 2-member array from cA/cB, authority from DOF (0→FULL pin [M4(a) merge-into-junction], 1→AXIAL+crease axis, 2→NONE); survivor placed at the constrained meet (near-parallel axes guarded in the solver). (3) PRE-COMMIT self-int REJECT (M5 tier-1, v7_local_edges_cross_uv via merge_would_self_intersect): for each plane the survivor will belong to, build the would-be ring (lose.id→surv.id, survivor at the meet), project to that plane's UV, run the local O(n) edge-cross test; on a cross REJECT — nothing committed, clean revert. (4) REORDERED ABOVE R.3 (DROP_COLINEAR) in CHASE_AFTER_MOVE to honor MERGE→DROP cascade priority. Survivor tie-break unchanged (dof asc, plane-count desc, id asc). Still A/B-gated by BR_NO_R4. Position math + type-gate change merged geometry → cache invalidates. v8 (T4, 2026-06-18): in-loop make-shared snap-in + read-only M1 guard PREVIEW. (1) ridge `type` plumbed into rg[] (was dropped) + RIDGE_TYPE_ALLOWED({ridge,hip,valley}) allowlist (I1, §9); step/flashing excluded. (2) plane→ridges CSR index built once. (3) ridges_incident_to(C) = C's ridge_ids tags ∪ index-scanned reached ridges (tmax>0 ∧ perp≤τ_in ∧ t∈[0,tmax]), allow-listed. (4) SNAP-IN inside CHASE_AFTER_MOVE (after R.4): a corner reaching a NEW allow-listed on-R ridge gets push_unique + RETAG_DOF (min-DOF → DOF-1 + rg.rd axis, no freeze) + topo_changed; one-way/idempotent. EFFECT gated by BR_SNAP_IN (default ON) — with =0 the loop output is byte-identical to v7 (snap-in inert; the guard preview is read-only either way). (5) read-only M1 guard PREVIEW census post-loop: per alive single-plane on-R corner, cond-B (B corner within τ_partner on R, cross-plane) → n_would_merge; else cond-A (cA inside a both-R B path-edge) → n_split_share. NO mutation — tallies what T5/T7 SHARE would fire. With BR_SNAP_IN=1 (default) the snap-in CHANGES tags/DOF/sampling → cache invalidates. v7 (de-dup, 2026-06-18): the R.1 post-move chase (R.2/R.3/R.4) was hand-inlined TWICE (GREEDY + GLOBAL_BEST accept-points); T0b/A2 patched only the GREEDY copy, leaving the DEFAULT GLOBAL_BEST chase missing RECONCILE_PLANES_DOWN+RETAG_DOF on its inline R.2/R.3. Factored both into one CHASE_AFTER_MOVE macro (the correct GREEDY body) → GLOBAL_BEST now carries the hygiene. Byte-identical on dahouet BOTH modes (GREEDY 45f0a6c1, GLOBAL_BEST e3ddd97d — the added hygiene is inert here) but CAN change output on other buildings → cache invalidates. v6 (T3/A2, 2026-06-18): in-loop ridge_ids (P4) maintenance + DOF coupling (§4.6, min-DOF). cs[].ridge_ids added (seeded from corners_in[].ridge_ids, reality-intersected against rg[] clipped segments at τ_in=ridge_merge_coplanar_m; additively unioned on MERGE_CORNERS). rg[] build hoisted ABOVE R.0 DOF tag. DOF coupling (gated by BR_DOF_COUPLE, default ON): dof=min(plane_dof, ridge_dof), ridge_dof={>=2→0,==1→1,==0→2}; a DOF-1-by-ridge corner gets axis_a=rg.rd + has_axis_a (no freeze). Fixes the plane-count-only mis-tag (single-plane on-ridge corner → DOF-1, slides on crease). Junctions preserved (min keeps min(0,1)=0). DOF/sampling change → cache invalidates. v5 (T0b, 2026-06-17): planes_dyn (P2) reconciled DOWN to path-membership (P3, canonical) — seed-intersect with initial paths (drops seed-origin over-claims) + RECONCILE_PLANES_DOWN after every path-shrinking op (MERGE dedup-collapse/path-collapse-to-n<3, R.2, R.3, R.4) so a corner dropped from a plane's path no longer stale-claims it. Drives the P2⊋P3 consistency gate to 0. corner_planes still EMITTED from planes_dyn (not re-derived). DOF-1→DOF-2 shifts (stale-2-plane → correct-1-plane) change sampling; R.3 may newly remove corners. Output/DOF change → cache invalidates. v4 (T0, 2026-06-17): polygon_paths now re-emitted from FINAL post-loop polys[] (was a pre-loop snapshot missing in-loop R.2/R.3/R.4 edits → dead-id refs). EDGES rebuilds from the corrected paths → classify INTERIOR_SHARED + wall-detect output move. Output semantics changed → cache invalidates. v3 (2026-06-12): R.4 in-loop ridge edge-merge — cross-plane corners on a shared interior ridge unify into one shared corner (planes_dyn={A,B}, DOF→1); re-emits CORNERS with grown plane_ids. v2 (2026-05-30): default maxIterations 1000 → 200.
    .reads            = v7_boundary_regularize_reads,
    .n_reads          = sizeof(v7_boundary_regularize_reads) / sizeof(v7_boundary_regularize_reads[0]),
    .writes           = v7_boundary_regularize_writes,
    .n_writes         = sizeof(v7_boundary_regularize_writes) / sizeof(v7_boundary_regularize_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_regularize_exec,
};
