// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-classify — step wrapper. Consumes the per-bldg roof-graph
// TLV from v7-boundary-regularize (Path-B reorder 2026-06-12: classify now
// runs after regularize), runs Phase F perimeter edge classification
// + level-transition detection + PIP overhang fallback. Emits:
//
//   - PERIMETER_TAGS  (0x0C)   NEW — per-edge tag + outward_xz + coverage detail
//   - NEW_ADJACENCIES (0x0D)   NEW — eave-shared plane-pair links (deduped vs ridges)
//
// All upstream sections pass through unchanged.
//
// Reads:
//   per-building-records                 (per-bldg)
//   building-local-frames                (per-bldg — centroid)
//   boundary-regularize-roof-graph       (per-bldg TLV input)
//
// Writes:
//   boundary-classify-roof-graph         (per-bldg ARRAY_INLINE of TLV)
//
// Note: PERIMETER_TAGS spec extension (forward-compatible) — bit 0x02
// in the per-record flags byte indicates an outward_xz f32×2 payload
// immediately after the 8-byte fixed header.

#define _POSIX_C_SOURCE 200809L

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
#include "v7-boundary-classify-impl.h"
#include "v7-corners-parse.h"
#include "v7-roof-graph-format.h"

#define BLF_RECORD_BYTES 72

// ── Axis convention ──────────────────────────────────────────────
//
// v7-load-real-test-case loads bench-v2 fixtures with an explicit
// axis swap (v7-load-real-test-case.c L286-288):
//   v3.X = v2.X
//   v3.Y = -v2.Z
//   v3.Z = v2.Y       (Z is up in v3, was Y in v2)
//
// All v3 steps store positions in this z-up frame. The classify impl
// (and the bench harness that validates it byte-equivalent against v2
// dumps) is hardcoded to y-up: position[1] is height, outward_xz
// components are X and Z of the v2 horizontal plane.
//
// To keep the impl simple AND let the bench keep validating against
// v2-frame dumps directly, we swap at the wrapper boundary: z-up to
// y-up on input parse, y-up to z-up on TLV emit.
//
//   v3 (z-up) → v2 (y-up):  (X, Y, Z) → (X,  Z, -Y)
//   v2 (y-up) → v3 (z-up):  (X, Y, Z) → (X, -Z,  Y)

static inline void axis_zup_to_yup(double p[3]) {
    const double x = p[0], y = p[1], z = p[2];
    p[0] = x; p[1] = z; p[2] = -y;
}

static inline void axis_yup_to_zup_xz_2d(float xz[2]) {
    // outward_xz in y-up is (X_yup, Z_yup) — horizontal components.
    // The 3D vec is (X_yup, 0, Z_yup); to z-up it becomes (X_yup, -Z_yup, 0).
    // Stored back into the 2-component field as (X_v3, Y_v3) horizontal pair.
    const float x = xz[0], z = xz[1];
    xz[0] = x; xz[1] = -z;
}

// ── Params ────────────────────────────────────────────────────────

static int parse_params(const char* params_str, v7_bclass_params_t* p) {
    v7_bclass_params_default(p);
    p->up_axis = 2;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    // Fail-loud + range-checked (json_number_or_default): wrong type,
    // non-finite, or out-of-range now fails the step instead of silently
    // keeping the default. Ranges: distances ∈ [0,1000] m, angles ∈
    // [0,180]°, coverage ratio ∈ [0,1], eps ∈ [0,100].
    #define BCLASS_NUM(key, lo, hi, field) \
        if (json_number_or_default(json_object_get(obj, key), (lo), (hi), key, &p->field) != 0) \
            { json_decref(obj); return -1; }
    BCLASS_NUM("perimDist3D",          0.0, 1000.0, perim_dist_3d);
    BCLASS_NUM("perimDistXZ",          0.0, 1000.0, perim_dist_xz);
    BCLASS_NUM("perimCollinearMinDeg", 0.0,  180.0, perim_collinear_min_deg);
    BCLASS_NUM("perimMinOverlapM",     0.0, 1000.0, perim_min_overlap_m);
    BCLASS_NUM("wallTiltMinDeg",       0.0,  180.0, wall_tilt_min_deg);
    BCLASS_NUM("pipProbeOffsetM",      0.0, 1000.0, pip_probe_offset_m);
    BCLASS_NUM("pipStepLenM",          0.0, 1000.0, pip_step_len_m);
    BCLASS_NUM("pipCoverageThreshold", 0.0,    1.0, pip_coverage_threshold);
    BCLASS_NUM("outwardSideEps",       0.0,  100.0, outward_side_eps);
    #undef BCLASS_NUM
    json_t* j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 1, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// ── Upstream parse ────────────────────────────────────────────────
//
// Reads PLANES, RIDGES, CORNERS, EDGES, POLYGON_PATHS. Mirrors the
// boundary-merge upstream parser; minor differences: ridges parsed as
// just (id, plane_a, plane_b); no ridge-line geometry needed.

typedef struct upstream {
    v7_bclass_input_plane_t*          planes;        size_t n_planes;
    v7_bclass_input_ridge_t*          ridges;        size_t n_ridges;
    v7_bclass_input_corner_t*         corners;       size_t n_corners;
    v7_bclass_input_edge_t*           edges;         size_t n_edges;
    v7_bclass_input_polygon_path_t*   polygon_paths; size_t n_polygon_paths;
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
    memset(u, 0, sizeof(*u));
}

// CORNERS reader — #72c Family D, extracted to v7-corners-parse.c
// (shared with regularize).
static int parse_corners(rg_reader_t* r, upstream_t* u,
                          double cx, double cy, double cz) {
    return v7_parse_corners_section(r, &u->corners, &u->n_corners, cx, cy, cz);
}

// EDGES reader — same as boundary-merge.c.
static int parse_edges(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_EDGES, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 16;
    u->edges = (v7_bclass_input_edge_t*)safe_calloc_array(cap, sizeof(*u->edges));
    if (!u->edges) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 40) continue;
        if (u->n_edges == cap) {
            cap *= 2;
            v7_bclass_input_edge_t* nb = (v7_bclass_input_edge_t*)safe_realloc_array(u->edges, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_edges, 0, (cap - u->n_edges) * sizeof(*nb));
            u->edges = nb;
        }
        v7_bclass_input_edge_t* E = &u->edges[u->n_edges++];
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

// POLYGON_PATHS reader — same as boundary-merge.c.
static int parse_polygon_paths(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_POLYGON_PATHS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 16;
    u->polygon_paths = (v7_bclass_input_polygon_path_t*)safe_calloc_array(cap, sizeof(*u->polygon_paths));
    if (!u->polygon_paths) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 8) continue;
        if (u->n_polygon_paths == cap) {
            cap *= 2;
            v7_bclass_input_polygon_path_t* nb = (v7_bclass_input_polygon_path_t*)safe_realloc_array(u->polygon_paths, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_polygon_paths, 0, (cap - u->n_polygon_paths) * sizeof(*nb));
            u->polygon_paths = nb;
        }
        v7_bclass_input_polygon_path_t* P = &u->polygon_paths[u->n_polygon_paths++];
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
            u->planes = (v7_bclass_input_plane_t*)safe_calloc_array(n, sizeof(*u->planes));
            if (!u->planes) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_plane_record_t* P = (const rg_plane_record_t*)(base + (size_t)i * stride);
                u->planes[i].id        = P->id;
                u->planes[i].rejected  = (P->flags & RG_PLANE_REJECTED) != 0;
                u->planes[i].tilt_deg  = (double)P->tilt_deg;
                u->planes[i].normal[0] = P->normal[0];
                u->planes[i].normal[1] = P->normal[1];
                u->planes[i].normal[2] = P->normal[2];
                // d transforms world→local in z-up frame (rotation-invariant,
                // so the same numeric d is valid in y-up local too).
                u->planes[i].d         = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                axis_zup_to_yup(u->planes[i].normal);
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
            u->ridges = (v7_bclass_input_ridge_t*)safe_calloc_array(n, sizeof(*u->ridges));
            if (!u->ridges) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_ridge_record_t* R = (const rg_ridge_record_t*)(base + (size_t)i * stride);
                u->ridges[i].id       = R->id;
                u->ridges[i].plane_a  = R->plane_a;
                u->ridges[i].plane_b  = R->plane_b;
            }
            u->n_ridges = n;
        }
    }
    if (parse_corners(&r, u, cx, cy, cz) != 0) return -1;
    if (parse_edges(&r, u)                != 0) return -1;
    if (parse_polygon_paths(&r, u)        != 0) return -1;
    return 0;
}

// ── Emitters ──────────────────────────────────────────────────────
//
// PERIMETER_TAGS (0x0C) — variable stride.
// NEW_ADJACENCIES (0x0D) — fixed stride 24 B.

static int emit_perimeter_tags(rg_writer_t* w, const v7_bclass_outputs_t* out) {
    // Count records to emit (skip tag==UNSET).
    size_t n_emit = 0;
    for (size_t i = 0; i < out->n_edges; i++) {
        if (out->edges[i].tag != V7_BCLASS_TAG_UNSET) n_emit++;
    }
    if (n_emit == 0) return 0;
    if (rg_writer_section_begin(w, RG_SECT_PERIMETER_TAGS, 0) != 0) return -1;
    for (size_t i = 0; i < out->n_edges; i++) {
        const v7_bclass_edge_t* E = &out->edges[i];
        if (E->tag == V7_BCLASS_TAG_UNSET) continue;
        // Fixed header: 8 B. Optional outward_xz: 8 B. Optional detail block.
        // Detail layout (when has_detail):
        //   u32 n_coverages; u32 _pad
        //   n_coverages × { f32 t0; f32 t1; u8 type; u8 _pad[3]; u32 other_plane_id; u32 other_edge_id; u32 _pad }  = 24 B
        //   f32 covered_fraction; u32 _pad
        //   u8 has_pip_overhang; u8 _pad[7]
        //   if has_pip_overhang: f32 coverage; u32 other_plane_id
        bool has_outward = E->has_outward_xz;
        bool has_detail  = (E->n_coverages > 0) || E->has_pip_overhang;
        size_t off = 8;
        size_t outward_block = has_outward ? 8 : 0;
        size_t cov_hdr = has_detail ? 8 : 0;
        size_t cov_body = has_detail ? (size_t)E->n_coverages * 24 : 0;
        size_t covered_block = has_detail ? 8 : 0;
        size_t pip_block = has_detail ? (E->has_pip_overhang ? 16 : 8) : 0;
        size_t rec_len = off + outward_block + cov_hdr + cov_body + covered_block + pip_block;
        // 8-align
        rec_len = (rec_len + 7) & ~(size_t)7;
        uint8_t* rec = (uint8_t*)safe_calloc_array(rec_len, 1);
        if (!rec) return -1;
        memcpy(rec + 0, &E->edge_id, 4);
        rec[4] = E->tag;
        uint8_t flags = (has_detail ? 0x01u : 0u) | (has_outward ? 0x02u : 0u);
        rec[5] = flags;
        size_t p = 8;
        if (has_outward) {
            // Impl emits y-up outward (X_yup, Z_yup); swap to v3 z-up
            // horizontal pair (X_v3, Y_v3) = (X_yup, -Z_yup).
            float out_xy[2] = { E->outward_xz[0], E->outward_xz[1] };
            axis_yup_to_zup_xz_2d(out_xy);
            memcpy(rec + p, &out_xy[0], 4);
            memcpy(rec + p + 4, &out_xy[1], 4);
            p += 8;
        }
        if (has_detail) {
            uint32_t ncov = E->n_coverages, pad = 0;
            memcpy(rec + p, &ncov, 4); memcpy(rec + p + 4, &pad, 4); p += 8;
            for (uint32_t k = 0; k < ncov; k++) {
                const v7_bclass_coverage_t* C = &E->coverages[k];
                memcpy(rec + p + 0,  &C->t0, 4);
                memcpy(rec + p + 4,  &C->t1, 4);
                rec[p + 8] = C->type;
                memcpy(rec + p + 12, &C->other_plane_id, 4);
                memcpy(rec + p + 16, &C->other_edge_id,  4);
                p += 24;
            }
            memcpy(rec + p, &E->covered_fraction, 4); p += 8;
            uint8_t hp = E->has_pip_overhang ? 1u : 0u;
            rec[p] = hp; p += 8;
            if (E->has_pip_overhang) {
                memcpy(rec + p, &E->pip_coverage, 4);
                memcpy(rec + p + 4, &E->pip_other_plane_id, 4);
                p += 8;
            }
        }
        int rc = rg_writer_record(w, rec, rec_len);
        free(rec);
        if (rc != 0) return -1;
    }
    return rg_writer_section_end(w);
}

static int emit_new_adjacencies(rg_writer_t* w, const v7_bclass_outputs_t* out) {
    if (out->n_new_adjacencies == 0) return 0;
    if (rg_writer_section_begin(w, RG_SECT_NEW_ADJACENCIES, sizeof(rg_new_adj_record_t)) != 0) return -1;
    for (size_t i = 0; i < out->n_new_adjacencies; i++) {
        const v7_bclass_new_adjacency_t* A = &out->new_adjacencies[i];
        rg_new_adj_record_t r;
        memset(&r, 0, sizeof(r));
        r.plane_a   = A->plane_a;
        r.plane_b   = A->plane_b;
        r.type      = A->type;
        r.edge_id_a = A->edge_id_a;
        r.edge_id_b = A->edge_id_b;
        if (rg_writer_record(w, &r, sizeof(r)) != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── Process one building ──────────────────────────────────────────

static int process_bldg(rg_writer_t* w,
                         const uint8_t* up_bytes, size_t up_len,
                         double cx, double cy, double cz,
                         const v7_bclass_params_t* params) {
    upstream_t up = {0};
    if (parse_upstream_full(up_bytes, up_len, cx, cy, cz, &up) != 0) {
        upstream_free(&up);
        return -1;
    }
    v7_bclass_outputs_t out;
    v7_bclass_outputs_init(&out);
    int compute_rc = v7_bclass_compute(
        up.planes, up.n_planes,
        up.corners, up.n_corners,
        up.edges, up.n_edges,
        up.polygon_paths, up.n_polygon_paths,
        up.ridges, up.n_ridges,
        params, &out);
    if (compute_rc != 0) { upstream_free(&up); v7_bclass_outputs_free(&out); return -1; }

    // Pass through everything from upstream; we don't replace any
    // existing section, only add 0x0C + 0x0D.
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) {
        upstream_free(&up); v7_bclass_outputs_free(&out); return -1;
    }
    if (rg_writer_passthrough(w, &r, RG_SECT_NONE) != 0) {
        upstream_free(&up); v7_bclass_outputs_free(&out); return -1;
    }
    if (emit_perimeter_tags(w, &out)   != 0 ||
        emit_new_adjacencies(w, &out)  != 0) {
        upstream_free(&up); v7_bclass_outputs_free(&out); return -1;
    }
    upstream_free(&up); v7_bclass_outputs_free(&out);
    return 0;
}

// ── Exec ──────────────────────────────────────────────────────────

static int v7_boundary_classify_exec(step_context_t* ctx) {
    v7_bclass_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-boundary-classify scope=tile "
                "perim_dist_3d=%.6f perim_dist_xz=%.6f perim_collinear_min_deg=%.3f "
                "perim_min_overlap_m=%.6f wall_tilt_min_deg=%.3f pip_step_len_m=%.4f\n",
                params.perim_dist_3d, params.perim_dist_xz,
                params.perim_collinear_min_deg, params.perim_min_overlap_m,
                params.wall_tilt_min_deg, params.pip_step_len_m);
    }

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-local-frames")         != n_buildings ||
        step_array_len(ctx, "boundary-regularize-roof-graph")     != n_buildings) {
        fprintf(stderr, "[v7-boundary-classify] count mismatch across per-bldg arrays\n");
        return -1;
    }

    blob_t b_roof = {0};
    int rc = 0;
    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec_bytes = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 12) { blob_push(&b_roof, NULL, 0); continue; }
        uint32_t schema;
        memcpy(&schema, rec_bytes + 0, 4);
        if (schema != 1) { blob_push(&b_roof, NULL, 0); continue; }

        const uint8_t* frame_bytes = NULL; size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) { blob_push(&b_roof, NULL, 0); continue; }
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        const uint8_t* up_bytes = NULL; size_t up_len = 0;
        if (step_array_read_bytes(ctx, "boundary-regularize-roof-graph", bi, &up_bytes, &up_len) != 0
            || !up_bytes || up_len < 32) {
            blob_push(&b_roof, NULL, 0); continue;
        }

        rg_writer_t w; rg_writer_init(&w);
        int prc = process_bldg(&w, up_bytes, up_len, cx, cy, cz, &params);
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
        if (blob_push(&b_roof, tlv_bytes, tlv_len) != 0) {
            free(tlv_bytes); rc = -1; break;
        }
        free(tlv_bytes);
    }

    if (rc == 0) rc = emit_blob(ctx, "boundary-classify-roof-graph", &b_roof);
    blob_free(&b_roof);
    return rc;
}

// ── Step descriptor ───────────────────────────────────────────────

static const step_read_t v7_boundary_classify_reads[] = {
    {.name = "per-building-records",           .array = true},
    {.name = "building-local-frames",          .array = true},
    {.name = "boundary-regularize-roof-graph",      .array = true},
};

static const step_write_t v7_boundary_classify_writes[] = {
    {.name = "boundary-classify-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_classify_def = {
    .name             = "v7-boundary-classify",
    .version          = 2,  // v2: z-up output (axis swap at wrapper boundary)
    .reads            = v7_boundary_classify_reads,
    .n_reads          = sizeof(v7_boundary_classify_reads) / sizeof(v7_boundary_classify_reads[0]),
    .writes           = v7_boundary_classify_writes,
    .n_writes         = sizeof(v7_boundary_classify_writes) / sizeof(v7_boundary_classify_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_classify_exec,
};
