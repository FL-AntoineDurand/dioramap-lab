// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-wall-detect — step wrapper. Consumes the per-bldg roof-graph
// TLV from v7-boundary-classify + global lidar-positions + per-bldg
// grm-owner + global beam-directions (optional) + per-bldg
// bldg-thresholds. Runs v7_wd_compute per-building, emits:
//
//   - wall-detect-roof-graph (per-bldg TLV) — passthrough upstream
//     with PLANES (append new walls), CORNERS (append 4 per wall),
//     POLYGON_PATHS (replace merged-walls' paths + append new walls)
//     replaced. All other sections pass through.
//
//   - wd-owner (per-bldg ARRAY_INLINE I16) — mutated per-bldg owner
//     with wall-grabbed points reassigned to their wall plane id.
//     Stays I16 (not widened to I32 like v2) — typical building has
//     <100 planes total, well within I16 range.
//
// outwardXZ for each edge comes from the PERIMETER_TAGS overlay
// (flag bit 0x02 has_outward_xz) — first downstream consumer of
// the spec extension landed in 5.7f.

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
#include "v7-derive-thresholds.h"
#include "v7-wall-detect-impl.h"
#include "v7-perbldg-gather.h"  // v7_per_bldg_load_frame (#72c Family B)
#include "v7-roof-graph-format.h"

#define BLF_RECORD_BYTES 72

// ── Axis convention ──────────────────────────────────────────────
//
// See v7-boundary-classify.c for the full explanation. v3 stores
// z-up; the impl is hardcoded y-up (matches the bench harness reading
// v2 dumps directly). Swap z-up→y-up at every input parse / per-bldg
// subset, swap y-up→z-up at every TLV / artifact emit.
//
//   v3 (z-up) → v2 (y-up):  (X, Y, Z) → (X,  Z, -Y)
//   v2 (y-up) → v3 (z-up):  (X, Y, Z) → (X, -Z,  Y)

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
static inline void axis_zup_to_yup_xy_2d(float xy[2]) {
    // Forward (read from TLV): (X_v3, Y_v3) → (X_yup, Z_yup).
    const float x = xy[0], y = xy[1];
    xy[0] = x; xy[1] = -y;
}

// ── Params ────────────────────────────────────────────────────────

static int parse_params(const char* params_str, v7_wd_params_t* p) {
    v7_wd_params_default(p);
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    int rc = 0;
    if (json_number_or_default(json_object_get(obj, "searchWidthM"),         0.0, 100.0, "searchWidthM",         &p->search_width_m)              != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "searchLengthBufferM"),  0.0, 100.0, "searchLengthBufferM",  &p->search_length_buffer_m)      != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "overhangCapM"),         0.0, 100.0, "overhangCapM",         &p->overhang_cap_m)              != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "mergeAngleDeg"),        0.0, 180.0, "mergeAngleDeg",        &p->merge_angle_deg)             != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "pointMatchDistM"),      0.0, 10.0,  "pointMatchDistM",      &p->point_match_dist_m)          != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "minBeamDownness"),      0.0, 1.0,   "minBeamDownness",      &p->min_beam_downness)           != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "yTopBufferM"),          0.0, 10.0,  "yTopBufferM",          &p->y_top_buffer_m)              != 0) rc = -1;
    if (json_number_or_default(json_object_get(obj, "wallTiltMinDeg"),       0.0, 180.0, "wallTiltMinDeg",       &p->wall_tilt_min_deg)           != 0) rc = -1;
    if (json_int32_or_default (json_object_get(obj, "upAxis"),               1, 2,        "upAxis",              &p->up_axis)                     != 0) rc = -1;
    json_decref(obj);
    return rc;
}

// ── Upstream parse (FIELD-LEVEL inputs to v7_wd_compute) ─────────
//
// Wall-detect needs typed inputs only for: planes (id/normal/d/tilt/rejected),
// corners (id/point), edges (id/corner_a/corner_b/plane_ids), perim_tags
// (edge_id/tag/outward_xz). It does NOT need RIDGES, RIDGE_LINES,
// PLANE_GRIDS, etc. — those pass through verbatim.

typedef struct upstream {
    v7_wd_input_plane_t*       planes;       size_t n_planes;
    v7_wd_input_corner_t*      corners;      size_t n_corners;
    v7_wd_input_edge_t*        edges;        size_t n_edges;
    v7_wd_input_perim_tag_t*   perim_tags;   size_t n_perim_tags;
} upstream_t;

static void upstream_free(upstream_t* u) {
    if (!u) return;
    free(u->planes);
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
    free(u->perim_tags);
    memset(u, 0, sizeof(*u));
}

static int parse_corners(rg_reader_t* r, upstream_t* u, double cx, double cy, double cz) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_CORNERS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
    size_t cap = 16;
    u->corners = (v7_wd_input_corner_t*)safe_calloc_array(cap, sizeof(*u->corners));
    if (!u->corners) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 32) continue;
        if (u->n_corners == cap) {
            size_t nc = cap * 2;
            v7_wd_input_corner_t* nb = (v7_wd_input_corner_t*)safe_realloc_array(u->corners, nc, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_corners, 0, (nc - u->n_corners) * sizeof(*nb));
            u->corners = nb; cap = nc;
        }
        v7_wd_input_corner_t* C = &u->corners[u->n_corners++];
        memcpy(&C->id, rec + 0, 4);
        C->source = rec[4];
        const uint8_t flags = rec[5];
        double pw[3];
        memcpy(&pw[0], rec + 8,  8);
        memcpy(&pw[1], rec + 16, 8);
        memcpy(&pw[2], rec + 24, 8);
        C->point[0] = pw[0] - cx; C->point[1] = pw[1] - cy; C->point[2] = pw[2] - cz;
        axis_zup_to_yup_d(C->point);
        // Walk variable trailer; we only need plane_ids for owner check (skip rest).
        size_t off = 32;
        uint32_t pc; memcpy(&pc, rec + off, 4); off += 8;
        if (pc > 0) {
            C->plane_ids = (uint32_t*)safe_malloc_array(pc, sizeof(uint32_t));
            if (!C->plane_ids) return -1;
            memcpy(C->plane_ids, rec + off, pc * 4);
            C->n_plane_ids = pc;
            off += pc * 4 + pc;  // skip source_per_plane
        }
        // Rest of trailer is irrelevant for wall-detect.
        (void)flags;
    }
    return 0;
}

static int parse_edges(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_EDGES, &sect, &slen) != 0) return 0;
    rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
    size_t cap = 16;
    u->edges = (v7_wd_input_edge_t*)safe_calloc_array(cap, sizeof(*u->edges));
    if (!u->edges) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 40) continue;
        if (u->n_edges == cap) {
            size_t nc = cap * 2;
            v7_wd_input_edge_t* nb = (v7_wd_input_edge_t*)safe_realloc_array(u->edges, nc, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_edges, 0, (nc - u->n_edges) * sizeof(*nb));
            u->edges = nb; cap = nc;
        }
        v7_wd_input_edge_t* E = &u->edges[u->n_edges++];
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

static int parse_perim_tags(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_PERIMETER_TAGS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
    size_t cap = 16;
    u->perim_tags = (v7_wd_input_perim_tag_t*)safe_calloc_array(cap, sizeof(*u->perim_tags));
    if (!u->perim_tags) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 8) continue;
        if (u->n_perim_tags == cap) {
            size_t nc = cap * 2;
            v7_wd_input_perim_tag_t* nb = (v7_wd_input_perim_tag_t*)safe_realloc_array(u->perim_tags, nc, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_perim_tags, 0, (nc - u->n_perim_tags) * sizeof(*nb));
            u->perim_tags = nb; cap = nc;
        }
        v7_wd_input_perim_tag_t* PT = &u->perim_tags[u->n_perim_tags++];
        memcpy(&PT->edge_id, rec + 0, 4);
        PT->tag = rec[4];
        const uint8_t flags = rec[5];
        size_t off = 8;
        if (flags & 0x02u) {
            PT->has_outward_xz = true;
            memcpy(&PT->outward_xz[0], rec + off, 4);
            memcpy(&PT->outward_xz[1], rec + off + 4, 4);
            axis_zup_to_yup_xy_2d(PT->outward_xz);
            // off += 8; — wall-detect doesn't need detail block
        }
        // Rest of detail block irrelevant for wall-detect.
    }
    return 0;
}

static int parse_upstream(const uint8_t* tlv_bytes, size_t tlv_len,
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
            u->planes = (v7_wd_input_plane_t*)safe_calloc_array(n, sizeof(*u->planes));
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
                u->planes[i].d         = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                memcpy(u->planes[i].basis_e1, P->basis_e1, 24);
                memcpy(u->planes[i].basis_e2, P->basis_e2, 24);
                axis_zup_to_yup_d(u->planes[i].normal);
                axis_zup_to_yup_d(u->planes[i].basis_e1);
                axis_zup_to_yup_d(u->planes[i].basis_e2);
            }
            u->n_planes = n;
        }
    }
    if (parse_corners(&r, u, cx, cy, cz) != 0) return -1;
    if (parse_edges(&r, u)                != 0) return -1;
    if (parse_perim_tags(&r, u)          != 0) return -1;
    return 0;
}

// ── Emit new wall planes (append to PLANES section) ──────────────
//
// PLANES is fixed-stride 128 B. Wrapper strategy:
//  1. begin section
//  2. memcpy every upstream record verbatim
//  3. synthesize one record per new wall (deriving basis from normal)
//  4. end section
//
// For merged walls (existing_plane_id != UINT32_MAX), the upstream
// PLANES record stays unchanged — wall-detect only updates the merged
// plane's POLYGON_PATHS entry (the algo doesn't change normal/d/tilt
// of merged planes, by spec).

static int emit_planes(rg_writer_t* w,
                        const uint8_t* up_bytes, size_t up_len,
                        const v7_wd_outputs_t* o,
                        double cx, double cy, double cz) {
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) return -1;
    const uint8_t* sect = NULL; size_t slen = 0;
    uint32_t n_up = 0, stride = sizeof(rg_plane_record_t);
    if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &slen) == 0) {
        rg_section_header(sect, slen, &n_up, &stride);
        if (stride != sizeof(rg_plane_record_t)) n_up = 0;
    }
    if (rg_writer_section_begin(w, RG_SECT_PLANES, sizeof(rg_plane_record_t)) != 0) return -1;
    // Passthrough upstream records.
    if (sect && n_up > 0) {
        const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
        for (uint32_t i = 0; i < n_up; i++) {
            if (rg_writer_record(w, base + (size_t)i * stride, sizeof(rg_plane_record_t)) != 0) return -1;
        }
    }
    // Append new walls.
    for (size_t i = 0; i < o->n_wall_planes; i++) {
        const v7_wd_wall_plane_t* wp = &o->wall_planes[i];
        if (wp->is_merged) continue;  // merged walls reuse the upstream PLANES record
        rg_plane_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.id       = wp->id;
        rec.flags    = RG_PLANE_HAS_BASIS | RG_PLANE_HAS_POLYGON;
        // Tilt is u16 — clamp to fit (90 always fits, but be defensive).
        rec.tilt_deg = (uint16_t)(wp->tilt_deg + 0.5);
        // Impl outputs y-up centroid + normal in building-local. Swap each
        // to z-up, then shift to world for the TLV record.
        double centroid_v3[3] = { wp->centroid[0], wp->centroid[1], wp->centroid[2] };
        double normal_v3[3]   = { wp->normal[0],   wp->normal[1],   wp->normal[2]   };
        axis_yup_to_zup_d(centroid_v3);
        axis_yup_to_zup_d(normal_v3);
        rec.centroid[0] = centroid_v3[0] + cx;
        rec.centroid[1] = centroid_v3[1] + cy;
        rec.centroid[2] = centroid_v3[2] + cz;
        rec.normal[0]   = normal_v3[0];
        rec.normal[1]   = normal_v3[1];
        rec.normal[2]   = normal_v3[2];
        // d is rotation-invariant under axis swap; shift to world using z-up normal.
        rec.d = wp->d - (normal_v3[0] * cx + normal_v3[1] * cy + normal_v3[2] * cz);
        // Wall basis in z-up: e1 = Z (up), e2 = e1 × normal = (-ny, nx, 0).
        rec.basis_e1[0] = 0.0; rec.basis_e1[1] = 0.0; rec.basis_e1[2] = 1.0;
        rec.basis_e2[0] = -normal_v3[1];
        rec.basis_e2[1] =  normal_v3[0];
        rec.basis_e2[2] =  0.0;
        rec.plane_type  = RG_PLANE_TYPE_WALL;  // these are detected walls
        if (rg_writer_record(w, &rec, sizeof(rec)) != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── Emit CORNERS (passthrough records verbatim + append wall corners) ──
//
// Variable-stride. The passthrough trick: iterate upstream records via
// rg_record_iter_next, pass each (rec, rec_len) directly to
// rg_writer_record — no field-by-field re-parsing needed.
//
// Wall corners are SIMPLE records: 32-byte header + plane_ids block (1
// entry + 1 source byte + pad to 8) + 8B ridge-block (0 entries) + name
// block (8B prefix + name + pad). No dirs, no com_bisectors, no members.

static int emit_corners(rg_writer_t* w,
                         const uint8_t* up_bytes, size_t up_len,
                         const v7_wd_outputs_t* o,
                         double cx, double cy, double cz) {
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) return -1;
    if (rg_writer_section_begin(w, RG_SECT_CORNERS, 0) != 0) return -1;
    const uint8_t* sect = NULL; size_t slen = 0;
    if (rg_reader_section(&r, RG_SECT_CORNERS, &sect, &slen) == 0) {
        rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
        const uint8_t* rec; uint32_t rec_len;
        while (rg_record_iter_next(&it, &rec, &rec_len)) {
            if (rg_writer_record(w, rec, rec_len) != 0) return -1;
        }
    }
    // Append wall corners. Each is a minimal record:
    //   header(32) + plane_block(8 + 4 + 1 → pad to 8 = 16) + ridge_block(8) + name_block(8 + len + pad)
    for (size_t i = 0; i < o->n_wall_corners; i++) {
        const v7_wd_wall_corner_t* wc = &o->wall_corners[i];
        const size_t name_len = strlen(wc->name);
        const size_t name_blk = (8 + name_len + 7) & ~(size_t)7;
        const size_t plane_blk = 16;   // 8B prefix + 4B id + 1B src + 3B pad → 16
        const size_t ridge_blk = 8;    // 8B prefix, 0 entries
        const size_t rec_len = 32 + plane_blk + ridge_blk + (name_len > 0 ? name_blk : 0);
        uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
        if (!rec) return -1;
        const uint8_t SOURCE_WALL_DETECT = 0;  // no dedicated enum value — wall corners aren't pool-managed
        memcpy(rec + 0, &wc->id, 4);
        rec[4] = SOURCE_WALL_DETECT;
        rec[5] = name_len > 0 ? 0x01u : 0u;   // has_name flag
        // n_merged stays 0; wall corners are atomic.
        // Impl outputs y-up; swap to v3 z-up then shift to world.
        double pv3[3] = { wc->point[0], wc->point[1], wc->point[2] };
        axis_yup_to_zup_d(pv3);
        double pw[3] = { pv3[0] + cx, pv3[1] + cy, pv3[2] + cz };
        memcpy(rec + 8,  &pw[0], 8);
        memcpy(rec + 16, &pw[1], 8);
        memcpy(rec + 24, &pw[2], 8);
        size_t off = 32;
        const uint32_t one = 1, zero = 0;
        memcpy(rec + off, &one,  4);
        memcpy(rec + off + 4, &zero, 4);
        memcpy(rec + off + 8, &wc->plane_id, 4);
        rec[off + 12] = SOURCE_WALL_DETECT;  // source_per_plane
        off = 32 + plane_blk;
        memcpy(rec + off, &zero, 4);  // n_ridge_ids = 0
        memcpy(rec + off + 4, &zero, 4);
        off += ridge_blk;
        if (name_len > 0) {
            const uint32_t nl = (uint32_t)name_len;
            memcpy(rec + off, &nl, 4);
            memcpy(rec + off + 4, &zero, 4);
            memcpy(rec + off + 8, wc->name, name_len);
        }
        int wr = rg_writer_record(w, rec, rec_len);
        free(rec);
        if (wr != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── Emit POLYGON_PATHS — passthrough upstream EXCEPT replace any path
//    whose plane_id matches a merged wall, then append fresh paths
//    for ALL wall planes (new + merged — merged ones have their old
//    path replaced).

static int emit_polygon_paths(rg_writer_t* w,
                                const uint8_t* up_bytes, size_t up_len,
                                const v7_wd_outputs_t* o) {
    // Build set of plane_ids that wall-detect now owns.
    uint32_t* owned_ids = (uint32_t*)safe_malloc_array(o->n_wall_planes ? o->n_wall_planes : 1, sizeof(uint32_t));
    if (!owned_ids) return -1;
    for (size_t i = 0; i < o->n_wall_planes; i++) owned_ids[i] = o->wall_planes[i].id;
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) { free(owned_ids); return -1; }
    if (rg_writer_section_begin(w, RG_SECT_POLYGON_PATHS, 0) != 0) { free(owned_ids); return -1; }
    const uint8_t* sect = NULL; size_t slen = 0;
    if (rg_reader_section(&r, RG_SECT_POLYGON_PATHS, &sect, &slen) == 0) {
        rg_record_iter_t it; rg_record_iter_init(&it, sect, slen);
        const uint8_t* rec; uint32_t rec_len;
        while (rg_record_iter_next(&it, &rec, &rec_len)) {
            if (rec_len < 8) continue;
            uint32_t plane_id; memcpy(&plane_id, rec + 0, 4);
            bool owned = false;
            for (size_t i = 0; i < o->n_wall_planes; i++) {
                if (owned_ids[i] == plane_id) { owned = true; break; }
            }
            if (owned) continue;  // skip — we'll emit a fresh path below
            if (rg_writer_record(w, rec, rec_len) != 0) { free(owned_ids); return -1; }
        }
    }
    // Append fresh polygon paths for every wall plane (4 corners each).
    for (size_t i = 0; i < o->n_wall_planes; i++) {
        const v7_wd_wall_plane_t* wp = &o->wall_planes[i];
        // 8B header + 4 × 4B corner ids = 24 B, already 8-aligned.
        const size_t rec_len = 24;
        uint8_t rec[24];
        memcpy(rec + 0, &wp->id, 4);
        const uint32_t n_corners = 4;
        memcpy(rec + 4, &n_corners, 4);
        memcpy(rec + 8, wp->polygon_corner_ids, 16);
        if (rg_writer_record(w, rec, rec_len) != 0) { free(owned_ids); return -1; }
    }
    free(owned_ids);
    return rg_writer_section_end(w);
}

// ── Process one building ──────────────────────────────────────────

static int process_bldg(rg_writer_t* w,
                         const uint8_t* up_bytes, size_t up_len,
                         double cx, double cy, double cz,
                         const float* positions_local, size_t n_pts_local,
                         const float* beam_dirs_local,
                         int32_t* owner_local,
                         double pp_dist_threshold,
                         const v7_wd_params_t* params,
                         v7_wd_outputs_t* out) {
    upstream_t up = {0};
    if (parse_upstream(up_bytes, up_len, cx, cy, cz, &up) != 0) {
        upstream_free(&up);
        return -1;
    }
    v7_wd_outputs_init(out);
    int crc = v7_wd_compute(
        up.planes, up.n_planes,
        up.corners, up.n_corners,
        up.edges, up.n_edges,
        up.perim_tags, up.n_perim_tags,
        positions_local, n_pts_local,
        beam_dirs_local,
        owner_local,
        pp_dist_threshold,
        params, out);
    if (crc != 0) {
        upstream_free(&up);
        return -1;
    }
    // Pass through everything EXCEPT PLANES/CORNERS/POLYGON_PATHS.
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) { upstream_free(&up); return -1; }
    uint32_t skip[] = { RG_SECT_PLANES, RG_SECT_CORNERS, RG_SECT_POLYGON_PATHS };
    if (rg_writer_passthrough_n(w, &r, skip, 3) != 0) { upstream_free(&up); return -1; }
    if (emit_planes      (w, up_bytes, up_len, out, cx, cy, cz) != 0) { upstream_free(&up); return -1; }
    if (emit_corners     (w, up_bytes, up_len, out, cx, cy, cz) != 0) { upstream_free(&up); return -1; }
    if (emit_polygon_paths(w, up_bytes, up_len, out)            != 0) { upstream_free(&up); return -1; }
    upstream_free(&up);
    return 0;
}

// ── Exec ──────────────────────────────────────────────────────────

static int v7_wall_detect_exec(step_context_t* ctx) {
    v7_wd_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    // Tile-wide positions (F64×3·N — same convention as GRM).
    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) {
        fprintf(stderr, "[v7-wall-detect] missing/malformed lidar-positions\n");
        return -1;
    }
    const size_t total_points = pos_bytes / (3 * sizeof(double));

    // Tile-wide beam-directions (F32×3·N, optional).
    size_t bd_bytes = 0;
    const float* beam_dirs_tile = (const float*)step_read(ctx, "bi-directions", &bd_bytes);
    if (beam_dirs_tile && bd_bytes / (3 * sizeof(float)) != total_points) {
        fprintf(stderr, "[v7-wall-detect] beam-directions size mismatch — ignoring\n");
        beam_dirs_tile = NULL;
    }

    const size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-local-frames")         != n_buildings ||
        step_array_len(ctx, "boundary-classify-roof-graph") != n_buildings ||
        step_array_len(ctx, "grm-owner")                       != n_buildings ||
        step_array_len(ctx, "bldg-thresholds")                 != n_buildings) {
        fprintf(stderr, "[v7-wall-detect] count mismatch across per-bldg arrays\n");
        return -1;
    }

    blob_t b_roof = {0}, b_owner = {0};
    int rc = 0;
    float*  local_pos = NULL;  size_t local_pos_cap = 0;
    float*  local_beam = NULL; size_t local_beam_cap = 0;
    int32_t* local_owner32 = NULL; size_t local_owner_cap = 0;
    uint32_t wall_runs_total = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // Per-bldg record + local-frame parse (task #72c Family B helper).
        const uint32_t* gidx = NULL;
        uint32_t n_pts = 0;
        double cx, cy, cz;
        if (v7_per_bldg_load_frame(ctx, bi, &gidx, &n_pts, &cx, &cy, &cz) != 0) goto skip_bldg;

        const uint8_t* up_bytes = NULL; size_t up_len = 0;
        if (step_array_read_bytes(ctx, "boundary-classify-roof-graph", bi, &up_bytes, &up_len) != 0
            || !up_bytes || up_len < 32) goto skip_bldg;

        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts * sizeof(int16_t)) goto skip_bldg;
        const int16_t* own16 = (const int16_t*)own_bytes;

        const uint8_t* th_bytes = NULL; size_t th_len = 0;
        if (step_array_read_bytes(ctx, "bldg-thresholds", bi, &th_bytes, &th_len) != 0
            || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) goto skip_bldg;
        v7_bldg_thresholds_t th; memcpy(&th, th_bytes, sizeof(th));
        const double pp_dist_threshold = th.dist_threshold_grm;

        // Grow local buffers.
        if ((size_t)n_pts * 3 > local_pos_cap) {
            float* nb = (float*)safe_realloc_array(local_pos, (size_t)n_pts * 3, sizeof(float));
            if (!nb) { rc = -1; break; }
            local_pos = nb; local_pos_cap = (size_t)n_pts * 3;
        }
        if (beam_dirs_tile && (size_t)n_pts * 3 > local_beam_cap) {
            float* nb = (float*)safe_realloc_array(local_beam, (size_t)n_pts * 3, sizeof(float));
            if (!nb) { rc = -1; break; }
            local_beam = nb; local_beam_cap = (size_t)n_pts * 3;
        }
        if ((size_t)n_pts > local_owner_cap) {
            int32_t* nb = (int32_t*)safe_realloc_array(local_owner32, (size_t)n_pts, sizeof(int32_t));
            if (!nb) { rc = -1; break; }
            local_owner32 = nb; local_owner_cap = (size_t)n_pts;
        }

        // Subset tile positions / beam-dirs to per-bldg, shift positions to local,
        // then swap z-up → y-up so the impl sees the bench-equivalent frame.
        for (uint32_t i = 0; i < n_pts; i++) {
            const uint32_t g = gidx[i];
            if ((size_t)g >= total_points) {
                local_pos[3 * i + 0] = local_pos[3 * i + 1] = local_pos[3 * i + 2] = 0.0f;
                if (local_beam) { local_beam[3*i+0] = 0.0f; local_beam[3*i+1] = -1.0f; local_beam[3*i+2] = 0.0f; }
            } else {
                local_pos[3 * i + 0] = (float)(positions[3 * g + 0] - cx);
                local_pos[3 * i + 1] = (float)(positions[3 * g + 1] - cy);
                local_pos[3 * i + 2] = (float)(positions[3 * g + 2] - cz);
                axis_zup_to_yup_f(&local_pos[3 * i]);
                if (local_beam) {
                    local_beam[3*i+0] = beam_dirs_tile[3*g+0];
                    local_beam[3*i+1] = beam_dirs_tile[3*g+1];
                    local_beam[3*i+2] = beam_dirs_tile[3*g+2];
                    axis_zup_to_yup_f(&local_beam[3 * i]);
                }
            }
            local_owner32[i] = (int32_t)own16[i];
        }

        rg_writer_t w; rg_writer_init(&w);
        v7_wd_outputs_t out;
        int prc = process_bldg(&w, up_bytes, up_len, cx, cy, cz,
                                local_pos, (size_t)n_pts,
                                beam_dirs_tile ? local_beam : NULL,
                                local_owner32, pp_dist_threshold,
                                &params, &out);
        if (prc != 0) {
            rg_writer_free(&w);
            v7_wd_outputs_free(&out);
            goto skip_bldg;
        }
        wall_runs_total += out.n_walls_created + out.n_walls_merged;
        // Sanity: every wall plane id must fit in I16 (we keep owner as I16).
        for (size_t i = 0; i < out.n_wall_planes; i++) {
            if (out.wall_planes[i].id > 32767u) {
                fprintf(stderr, "[v7-wall-detect] bldg %zu wall plane id %u exceeds INT16_MAX — bump owner stride to I32\n",
                        bi, out.wall_planes[i].id);
                rg_writer_free(&w);
                v7_wd_outputs_free(&out);
                rc = -1;
                break;
            }
        }
        if (rc != 0) break;

        uint8_t* tlv_bytes = NULL; size_t tlv_len = 0;
        if (rg_writer_finalize(&w, &tlv_bytes, &tlv_len) != 0) {
            rg_writer_free(&w);
            v7_wd_outputs_free(&out);
            goto skip_bldg;
        }
        rg_writer_free(&w);
        if (blob_push(&b_roof, tlv_bytes, tlv_len) != 0) {
            free(tlv_bytes); v7_wd_outputs_free(&out); rc = -1; break;
        }
        free(tlv_bytes);

        // Emit per-bldg owner (I16 narrowed from local_owner32).
        int16_t* owner16 = (int16_t*)safe_malloc_array((size_t)n_pts, sizeof(int16_t));
        if (!owner16) { v7_wd_outputs_free(&out); rc = -1; break; }
        for (uint32_t i = 0; i < n_pts; i++) owner16[i] = (int16_t)local_owner32[i];
        if (blob_push(&b_owner, owner16, (size_t)n_pts * sizeof(int16_t)) != 0) {
            free(owner16); v7_wd_outputs_free(&out); rc = -1; break;
        }
        free(owner16);
        v7_wd_outputs_free(&out);
        continue;
skip_bldg:
        blob_push(&b_roof,  NULL, 0);
        blob_push(&b_owner, NULL, 0);
    }

    free(local_pos); free(local_beam); free(local_owner32);

    if (rc == 0) {
        if (emit_blob(ctx, "wall-detect-roof-graph", &b_roof) != 0) rc = -1;
        if (rc == 0 && emit_blob(ctx, "wd-owner", &b_owner) != 0) rc = -1;
    }
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-wall-detect total_walls=%u\n", wall_runs_total);
    }
    blob_free(&b_roof); blob_free(&b_owner);
    return rc;
}

// ── Step descriptor ───────────────────────────────────────────────

static const step_read_t v7_wall_detect_reads[] = {
    {.name = "lidar-positions"},
    {.name = "bi-directions"},
    {.name = "per-building-records",           .array = true},
    {.name = "building-local-frames",          .array = true},
    {.name = "boundary-classify-roof-graph", .array = true},
    {.name = "grm-owner",                       .array = true},
    {.name = "bldg-thresholds",                 .array = true},
};

static const step_write_t v7_wall_detect_writes[] = {
    {.name = "wall-detect-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "wd-owner",               .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_wall_detect_def = {
    .name             = "v7-wall-detect",
    .version          = 2,  // v2: z-up output (axis swap at wrapper boundary)
    .reads            = v7_wall_detect_reads,
    .n_reads          = sizeof(v7_wall_detect_reads) / sizeof(v7_wall_detect_reads[0]),
    .writes           = v7_wall_detect_writes,
    .n_writes         = sizeof(v7_wall_detect_writes) / sizeof(v7_wall_detect_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_wall_detect_exec,
};
