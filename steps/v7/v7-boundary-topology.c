// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-topology — step wrapper.
//
// First sub-step of the split bench-v2 boundary-signals (Phase A +
// class filter). Produces the topology skeleton — PLANES, RIDGES,
// RIDGE_LINES — that v7-boundary-signals then enriches with corners,
// nearby projections, and per-plane signal grids.
//
// Reads:
//   lidar-positions      (F64×3·N tile-wide)
//   per-building-records (per-bldg — for globalIndices)
//   building-local-frames (per-bldg — informational, not consumed by impl)
//   building-kd-trees    (per-bldg — kd-tree over building-local positions)
//   grm-owner            (per-bldg Int16, -1 unassigned)
//   grm-seed-planes      (per-bldg ARRAY_INLINE — authoritative post-merge
//                         plane equations, including tombstoned)
//   classifications      (U8×N tile-wide; optional — gates class filter)
//   lidar-density        (per-bldg F64 or tile-wide F64 — for spacing)
//
// Writes:
//   boundary-topology-roof-graph  (per-bldg ARRAY_INLINE of TLV blobs)
//
// Per ROOF-GRAPH-FORMAT.md, each TLV holds:
//   - PLANES         (one per grm seed plane, with rejection flags + basis)
//   - RIDGES         (one per surviving cross-plane adj pair)
//   - RIDGE_LINES    (parallel to RIDGES, full line geometry)
//
// Dropped per audit (boundary-signals re-audit, 2026-05-11):
//   - kd-tree, curvatures-k32, linearity (vestigial reads in v2)
//   - boundary-candidates*, seed-planes (vestigial — K from grm)
//   - ts-weights for plane fit (we trust grm-seed-planes equations)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v7-derive-thresholds.h"

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "kd_tree.h"
#include "v7-boundary-topology-impl.h"
#include "v7-roof-graph-format.h"

// grm-seed-planes record header layout (matches v7-grow-refit-merge-impl.h
// v7_grm_plane_record_t — 72 B):
//   f64×3 normal | f64 d | f64 tilt | f64 az | f64 rms | u32 inlier |
//   u32 src_seed | u8 is_ghost | u8 tombstoned | u8×6 _reserved
#define GRM_SEED_HDR_BYTES 72

// building-local-frames record header: f64 cx, cy, cz, ... (72 B total
// per build-building-kd-trees.c BLF_RECORD_BYTES).
#define BLF_RECORD_BYTES 72

// ── Params ──

static int parse_params(const char* params_str, v7_topo_params_t* p) {
    v7_topo_params_default(p);
    p->up_axis = 2;  // production z-up LAZ
    // Sentinel: -1 means "auto-compute per-bldg spacing from local positions"
    // in exec below. Pipeline `params.spacing` override (if real) wins via
    // the JSON loop. Bench harnesses bypass parse_params entirely so they
    // keep the v7_topo_params_default value (0.316).
    p->spacing = -1.0;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "adjacencyRadius"))   && json_is_number(j))    p->adjacency_radius       = json_number_value(j);
    j = json_object_get(obj, "minContactPts");
    if (json_int32_or_default(j, 1, 1000000, "minContactPts", &p->min_contact_pts) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "classFilterThreshold")) && json_is_number(j)) p->class_filter_threshold = json_number_value(j);
    j = json_object_get(obj, "minPlanePts");
    if (json_int32_or_default(j, 1, 1000000, "minPlanePts", &p->min_plane_pts) != 0) { json_decref(obj); return -1; }
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "spacing"))           && json_is_number(j))    p->spacing                = json_number_value(j);
    json_decref(obj);
    return 0;
}

// ── Parse one bldg's grm-seed-planes blob into v7_topo_input_seed_t[] ──

static int parse_grm_seeds(const uint8_t* bytes, size_t len,
                            v7_topo_input_seed_t** out, size_t* out_n) {
    *out = NULL; *out_n = 0;
    if (!bytes || len < 8) return 0;
    // Count records.
    size_t n = 0;
    {
        size_t cursor = 0;
        const uint8_t* h; size_t rl;
        while (blob_next(bytes, len, &cursor, &h, &rl)) {
            if (rl >= GRM_SEED_HDR_BYTES) n++;
        }
    }
    if (n == 0) return 0;
    v7_topo_input_seed_t* arr = (v7_topo_input_seed_t*)calloc(n, sizeof(*arr));
    if (!arr) return -1;
    size_t k = 0;
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(bytes, len, &cursor, &h, &rl)) {
        if (rl < GRM_SEED_HDR_BYTES) continue;
        memcpy(&arr[k].normal[0], h +  0, 8);
        memcpy(&arr[k].normal[1], h +  8, 8);
        memcpy(&arr[k].normal[2], h + 16, 8);
        memcpy(&arr[k].d,         h + 24, 8);
        // tilt/az/rms/inlier/src_seed/is_ghost ignored — we only need
        // (normal, d, tombstoned).
        arr[k].tombstoned = (h[65] != 0);  // byte offset 65 in record
        k++;
    }
    *out = arr; *out_n = k;
    return 0;
}

// ── Pack one bldg's v7_topo_outputs into a roof-graph TLV blob ──

static int pack_planes_section(rg_writer_t* w, const v7_topo_outputs_t* out) {
    if (rg_writer_section_begin(w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) != 0) return -1;
    for (size_t i = 0; i < out->n_planes; i++) {
        const v7_topo_plane_t* P = &out->planes[i];
        rg_plane_record_t r = {0};
        r.id = P->id;
        r.flags = 0;
        if (P->rejected)        r.flags |= RG_PLANE_REJECTED;
        if (P->has_basis)       r.flags |= RG_PLANE_HAS_BASIS;
        if (P->has_class_stats) r.flags |= RG_PLANE_HAS_CLASS_STATS;
        r.tilt_deg          = P->tilt_deg;
        r.centroid[0]       = P->centroid[0];
        r.centroid[1]       = P->centroid[1];
        r.centroid[2]       = P->centroid[2];
        r.normal[0]         = P->normal[0];
        r.normal[1]         = P->normal[1];
        r.normal[2]         = P->normal[2];
        r.d                 = P->d;
        r.n_pts             = P->n_pts;
        r.rejection_reason  = P->rejection_reason;
        r.slug_id           = 0;  // viz-only convenience; not computed here yet
        if (P->has_basis) {
            memcpy(r.basis_e1, P->basis_e1, sizeof(double) * 3);
            memcpy(r.basis_e2, P->basis_e2, sizeof(double) * 3);
        }
        // plane_type — topology is the SOLE writer of the exclusive roof/wall/ground
        // classification; it resolves it here (via the one canonical classifier) and
        // every consumer just READS r.plane_type.
        r.plane_type = (uint8_t)rg_classify_plane_type(
            P->tilt_deg, P->has_class_stats, P->cs_ground, P->cs_total);
        if (rg_writer_record(w, &r, sizeof(r)) != 0) return -1;
    }
    return rg_writer_section_end(w);
}

static int pack_ridges_section(rg_writer_t* w, const v7_topo_outputs_t* out) {
    if (rg_writer_section_begin(w, RG_SECT_RIDGES, sizeof(rg_ridge_record_t)) != 0) return -1;
    for (size_t i = 0; i < out->n_ridges; i++) {
        const v7_topo_ridge_t* R = &out->ridges[i];
        rg_ridge_record_t r = {0};
        r.id            = R->id;
        r.plane_a       = R->plane_a;
        r.plane_b       = R->plane_b;
        r.type          = R->type;
        memcpy(r.from, R->from, sizeof(double) * 3);
        memcpy(r.to,   R->to,   sizeof(double) * 3);
        r.length        = R->length;
        r.contact_count = R->contact_count;
        r.overlap_ratio = R->overlap_ratio;
        if (rg_writer_record(w, &r, sizeof(r)) != 0) return -1;
    }
    return rg_writer_section_end(w);
}

static int pack_ridge_lines_section(rg_writer_t* w, const v7_topo_outputs_t* out) {
    if (rg_writer_section_begin(w, RG_SECT_RIDGE_LINES, sizeof(rg_ridge_line_record_t)) != 0) return -1;
    for (size_t i = 0; i < out->n_ridges; i++) {
        const v7_topo_ridge_line_t* L = &out->ridge_lines[i];
        rg_ridge_line_record_t r = {0};
        r.ridge_id   = L->ridge_id;
        // flags: bit0 = has_line, bit1 = has_side_a, bit2 = has_side_b. Each
        // has_side_* bit gates D.2b's side test for the corresponding plane's
        // corners; the per-plane oriented side_vec_a/b carry the interior
        // direction (the old side_sign_a/b are folded into that orientation).
        r.flags      = 0u;
        if (L->has_line)   r.flags |= 0x01u;
        if (L->has_side_a) r.flags |= 0x02u;
        if (L->has_side_b) r.flags |= 0x04u;
        memcpy(r.line_point,     L->line_point,     sizeof(double) * 3);
        memcpy(r.line_direction, L->line_direction, sizeof(double) * 3);
        r.t_min_a = L->t_min_a;
        r.t_max_a = L->t_max_a;
        r.t_min_b = L->t_min_b;
        r.t_max_b = L->t_max_b;
        memcpy(r.side_vec_a, L->side_vec_a, sizeof(float) * 3);
        memcpy(r.side_vec_b, L->side_vec_b, sizeof(float) * 3);
        if (rg_writer_record(w, &r, sizeof(r)) != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// PLANE_VOIDS (0x12) — per-plane void_id list. Input is the bldg's slice
// of `grm-plane-voids`, framed by GRM's `blob_push` as a sequence of
// 8-aligned records (one record per (plane, void_id) pair, payload is
// the 8-byte pair). Sorted by plane_id (GRM emits in plane-iteration
// order). We unwrap, group by plane_id, and emit one roof-graph
// variable-stride record per group:
//   u32 plane_id, u32 n_void_ids, u32×n_void_ids
// Pass `n_planes` to bound plane_id sanity-check; pairs with plane_id
// out of range are skipped (shouldn't happen — GRM only emits plane
// indices in [0, n_planes)).
static int pack_plane_voids_section(rg_writer_t* w,
                                     const uint8_t* voids_bytes, size_t voids_len,
                                     uint32_t n_planes) {
    if (!voids_bytes || voids_len == 0) return 0;  // no voids → no section

    // First pass: count pairs + flatten into a temp (plane_id, void_id)
    // array, dropping invalid records. Simpler than threading the
    // record walk through the grouping logic.
    size_t cap = voids_len / 16 + 8;   // 16 = 8-byte header + 8-byte payload (worst case)
    uint32_t* pairs = (uint32_t*)malloc(cap * 2 * sizeof(uint32_t));
    if (!pairs) return -1;
    size_t n_pairs = 0;
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(voids_bytes, voids_len, &cursor, &h, &rl)) {
        if (rl != 8) continue;
        if (n_pairs == cap) {
            cap *= 2;
            uint32_t* grown = (uint32_t*)realloc(pairs, cap * 2 * sizeof(uint32_t));
            if (!grown) { free(pairs); return -1; }
            pairs = grown;
        }
        memcpy(&pairs[n_pairs*2 + 0], h + 0, 4);
        memcpy(&pairs[n_pairs*2 + 1], h + 4, 4);
        n_pairs++;
    }
    if (n_pairs == 0) { free(pairs); return 0; }

    if (rg_writer_section_begin(w, RG_SECT_PLANE_VOIDS, 0) != 0) {
        free(pairs); return -1;
    }
    size_t i = 0;
    while (i < n_pairs) {
        const uint32_t plane_id = pairs[i*2 + 0];
        if (plane_id >= n_planes) { i++; continue; }
        size_t j = i + 1;
        while (j < n_pairs && pairs[j*2 + 0] == plane_id) j++;
        const uint32_t n_voids = (uint32_t)(j - i);
        const size_t rec_len = 8 + (size_t)n_voids * 4;
        uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
        if (!rec) { free(pairs); return -1; }
        memcpy(rec + 0, &plane_id, 4);
        memcpy(rec + 4, &n_voids,  4);
        for (uint32_t k = 0; k < n_voids; k++) {
            memcpy(rec + 8 + k*4, &pairs[(i + k)*2 + 1], 4);
        }
        int wrc = rg_writer_record(w, rec, rec_len);
        free(rec);
        if (wrc != 0) { free(pairs); return -1; }
        i = j;
    }
    free(pairs);
    return rg_writer_section_end(w);
}

static int pack_topo_blob(const v7_topo_outputs_t* out,
                          const uint8_t* voids_bytes, size_t voids_len,
                          uint8_t** out_bytes, size_t* out_len) {
    rg_writer_t w; rg_writer_init(&w);
    int rc = 0;
    if (rc == 0) rc = pack_planes_section(&w, out);
    if (rc == 0) rc = pack_ridges_section(&w, out);
    if (rc == 0) rc = pack_ridge_lines_section(&w, out);
    if (rc == 0) rc = pack_plane_voids_section(&w, voids_bytes, voids_len, (uint32_t)out->n_planes);
    if (rc == 0) rc = rg_writer_finalize(&w, out_bytes, out_len);
    rg_writer_free(&w);
    return rc;
}

// ── Exec ──

static int v7_boundary_topology_exec(step_context_t* ctx) {
    v7_topo_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-kd-trees") != n_buildings ||
        step_array_len(ctx, "grm-owner")         != n_buildings ||
        step_array_len(ctx, "grm-seed-planes")   != n_buildings) {
        fprintf(stderr, "[v7-boundary-topology] count mismatch across per-bldg arrays\n");
        return -1;
    }

    size_t cls_bytes = 0;
    const uint8_t* classifications = (const uint8_t*)step_read(ctx, "lidar-classifications", &cls_bytes);
    if (classifications && cls_bytes != total_points) classifications = NULL;

    blob_t b_roof = {0};
    double* local_pos = NULL;
    float*  local_f32 = NULL;   // for centroid-shift + F32 roundtrip
    uint8_t* local_cls = NULL;
    size_t   local_cap = 0;
    int rc = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // ── Per-building-records: get globalIndices ──
        const uint8_t* rec_bytes = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 12) goto skip_bldg;
        uint32_t schema, n_verts, n_pts_local;
        memcpy(&schema,      rec_bytes + 0, 4);
        memcpy(&n_verts,     rec_bytes + 4, 4);
        memcpy(&n_pts_local, rec_bytes + 8, 4);
        if (schema != 1) goto skip_bldg;
        const size_t idx_off = 68 + (size_t)n_verts * 24;
        if (idx_off + (size_t)n_pts_local * 4 > rec_len) goto skip_bldg;
        const uint32_t* global_indices = (const uint32_t*)(rec_bytes + idx_off);
        if (n_pts_local < 4) goto skip_bldg;

        if ((size_t)n_pts_local > local_cap) {
            local_cap = (size_t)n_pts_local * 2;
            local_pos = (double*)realloc(local_pos, local_cap * 3 * sizeof(double));
            local_f32 = (float*) realloc(local_f32, local_cap * 3 * sizeof(float));
            local_cls = (uint8_t*)realloc(local_cls, local_cap * sizeof(uint8_t));
            if (!local_pos || !local_f32 || !local_cls) { rc = -1; break; }
        }

        // Get building centroid (cx, cy, cz) from building-local-frames —
        // the kd-tree was built over centroid-shifted F32-cast positions
        // (build-building-kd-trees.c convention). Replicate that transform
        // exactly so distance lookups match.
        const uint8_t* frame_bytes = NULL; size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) goto skip_bldg;
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        for (size_t i = 0; i < n_pts_local; i++) {
            uint32_t gi = global_indices[i];
            if ((size_t)gi >= total_points) { rc = -1; goto bldg_fail; }
            local_f32[i * 3 + 0] = (float)(positions[gi * 3 + 0] - cx);
            local_f32[i * 3 + 1] = (float)(positions[gi * 3 + 1] - cy);
            local_f32[i * 3 + 2] = (float)(positions[gi * 3 + 2] - cz);
            local_cls[i]         = classifications ? classifications[gi] : 0;
        }
        for (size_t i = 0; i < n_pts_local * 3; i++) {
            local_pos[i] = (double)local_f32[i];
        }
        const uint8_t* cls_arg = classifications ? local_cls : NULL;

        // ── Per-building spacing override (when not pinned via params).
        //    Mirrors v2 boundary-signals.js#L385: spacing = 1/sqrt(density)
        //    where density is the building's local-frame horizontal-area
        //    density (n_pts / Δh1·Δh2). Critical for the ridge gates in
        //    v7_topo_compute: `contacts_per_m >= 0.3·sqrt(n)·...` floor +
        //    `side_vec * spacing*3` side-sample distance. Hardcoded 0.316
        //    fallback under-samples by 41% on dahouet (actual spacing 0.224
        //    at per-bldg density 19.87).
        // Spacing sourced from bldg-thresholds (single source of truth —
        // see v7-derive-thresholds.c). Pipeline params (`spacing` JSON key)
        // override; otherwise read per-bldg from the artifact.
        v7_topo_params_t bldg_params = params;
        if (bldg_params.spacing <= 0.0) {
            const uint8_t* th_bytes = NULL; size_t th_len = 0;
            if (step_array_read_bytes(ctx, "bldg-thresholds", bi,
                                       &th_bytes, &th_len) != 0
                || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) {
                fprintf(stderr, "[v7-boundary-topology] bldg%zu missing bldg-thresholds record\n", bi);
                rc = -1; goto bldg_fail;
            }
            v7_bldg_thresholds_t th;
            memcpy(&th, th_bytes, sizeof(th));
            bldg_params.spacing = th.spacing;
            if (getenv("DERIV_LOG")) {
                fprintf(stderr, "[DERIV] step=v7-boundary-topology scope=bldg%zu n_pts=%u density=%.6f spacing=%.6f src_spacing=bldg-thresholds\n",
                        bi, n_pts_local, th.density, bldg_params.spacing);
            }
        } else if (getenv("DERIV_LOG")) {
            fprintf(stderr, "[DERIV] step=v7-boundary-topology scope=bldg%zu n_pts=%u spacing=%.6f src_spacing=param\n",
                    bi, n_pts_local, bldg_params.spacing);
        }

        // ── kd-tree ──
        const uint8_t* tree_bytes = NULL; size_t tree_len = 0;
        if (step_array_read_bytes(ctx, "building-kd-trees", bi, &tree_bytes, &tree_len) != 0
            || !tree_bytes || tree_len < 4) goto skip_bldg;
        kd_tree_t tree;
        if (kd_deserialize(&tree, local_pos, n_pts_local, tree_bytes, tree_len) != 0) {
            goto skip_bldg;
        }

        // ── grm-owner ──
        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts_local * sizeof(int16_t)) {
            kd_free(&tree); goto skip_bldg;
        }
        const int16_t* owner = (const int16_t*)own_bytes;

        // ── grm-seed-planes ──
        const uint8_t* seed_bytes = NULL; size_t seed_len = 0;
        if (step_array_read_bytes(ctx, "grm-seed-planes", bi, &seed_bytes, &seed_len) != 0) {
            kd_free(&tree); goto skip_bldg;
        }

        // ── grm-plane-voids (optional — only present when dv2 ran upstream) ──
        // Pass-through into the roof-graph as RG_SECT_PLANE_VOIDS so the
        // (plane_id → void_id) mapping survives downstream consumers
        // (boundary-signals + later steps) without them needing to read
        // an additional artifact.
        const uint8_t* voids_bytes = NULL; size_t voids_len = 0;
        if (step_array_len(ctx, "grm-plane-voids") == n_buildings) {
            if (step_array_read_bytes(ctx, "grm-plane-voids", bi, &voids_bytes, &voids_len) != 0) {
                voids_bytes = NULL; voids_len = 0;
            }
        }
        v7_topo_input_seed_t* seeds = NULL; size_t n_seeds = 0;
        if (parse_grm_seeds(seed_bytes, seed_len, &seeds, &n_seeds) != 0) {
            kd_free(&tree); rc = -1; break;
        }

        // ── Compute ──
        v7_topo_outputs_t out;
        v7_topo_outputs_init(&out);
        const int compute_rc = v7_topo_compute(local_pos, n_pts_local,
                                                owner, cls_arg,
                                                seeds, n_seeds,
                                                &tree, &bldg_params, &out);
        kd_free(&tree);
        free(seeds);
        if (compute_rc != 0) { v7_topo_outputs_free(&out); rc = -1; break; }

        // ── Shift compute outputs back to tile-world coords ──
        // Compute ran in building-local (centroid-shifted F32-cast) space
        // for kd-tree compatibility. Viz + downstream expect tile-world.
        // Positional fields get (cx,cy,cz) added; direction vectors (normal,
        // basis, line_direction, side_vec) are translation-invariant. Plane
        // equation: d_world = d_local − n·c.
        for (size_t i = 0; i < out.n_planes; i++) {
            v7_topo_plane_t* P = &out.planes[i];
            P->centroid[0] += cx;
            P->centroid[1] += cy;
            P->centroid[2] += cz;
            P->d -= P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz;
        }
        for (size_t i = 0; i < out.n_ridges; i++) {
            v7_topo_ridge_t* R = &out.ridges[i];
            R->from[0] += cx; R->from[1] += cy; R->from[2] += cz;
            R->to[0]   += cx; R->to[1]   += cy; R->to[2]   += cz;
        }
        for (size_t i = 0; i < out.n_ridges; i++) {
            v7_topo_ridge_line_t* L = &out.ridge_lines[i];
            L->line_point[0] += cx;
            L->line_point[1] += cy;
            L->line_point[2] += cz;
        }

        // ── Pack TLV + push ──
        uint8_t* tlv_bytes = NULL; size_t tlv_len = 0;
        const int pack_rc = pack_topo_blob(&out, voids_bytes, voids_len, &tlv_bytes, &tlv_len);
        v7_topo_outputs_free(&out);
        if (pack_rc != 0) { free(tlv_bytes); rc = -1; break; }
        if (blob_push(&b_roof, tlv_bytes, tlv_len) != 0) {
            free(tlv_bytes); rc = -1; break;
        }
        free(tlv_bytes);
        continue;

    bldg_fail:
        rc = -1;
        break;

    skip_bldg:
        // Push an empty record to preserve per-bldg array index parity.
        blob_push(&b_roof, NULL, 0);
    }

    free(local_pos);
    free(local_f32);
    free(local_cls);

    if (rc == 0) rc = emit_blob(ctx, "boundary-topology-roof-graph", &b_roof);
    blob_free(&b_roof);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_boundary_topology_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "building-kd-trees",     .array = true},
    {.name = "grm-owner",             .array = true},
    {.name = "grm-seed-planes",       .array = true},
    {.name = "grm-plane-voids",       .array = true},
    {.name = "bldg-thresholds",       .array = true},
    {.name = "lidar-classifications"},
};

static const step_write_t v7_boundary_topology_writes[] = {
    {.name = "boundary-topology-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_topology_def = {
    .name             = "v7-boundary-topology",
    // v4 (2026-05-24): repack grm-plane-voids into roof-graph
    //                   RG_SECT_PLANE_VOIDS (0x12), preserving the
    //                   per-plane dv2 void-id membership through merges.
    // v2 (2026-05-16): spacing auto-computed per-bldg as 1/sqrt(density)
    //                   from local positions bbox; mirrors v2 boundary-
    //                   signals.js#L385 (which previously fell back to
    //                   sqrt(1/10)=0.316 when lidar-density was missing).
    .version          = 6,   // v6: plane_type field (roof/wall/ground) written here (sole classifier) + PLANES record 128→136B — 2026-07-04. Cascades a recompute through all downstream roof-graph steps (their input hash changes), so no per-step bump needed. v5: per-plane oriented side_vec_a/b (drop single side_vec + side_sign_a/b); RIDGE_LINES record 88→96B — 2026-06-11
    .reads            = v7_boundary_topology_reads,
    .n_reads          = sizeof(v7_boundary_topology_reads) / sizeof(v7_boundary_topology_reads[0]),
    .writes           = v7_boundary_topology_writes,
    .n_writes         = sizeof(v7_boundary_topology_writes) / sizeof(v7_boundary_topology_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_topology_exec,
};
