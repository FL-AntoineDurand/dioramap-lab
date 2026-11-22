// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-polygons-2 — step wrapper. Per-plane bp2 algorithm
// (cost-function-driven subset enumeration; see
// v7-boundary-polygons-2-impl.{h,c} + journal entry
// 2026-05-21-boundary-polygons-tsp-rethink).
//
// I/O signature is parallel to v7-boundary-polygons (bp1):
//   reads:  lidar-positions, per-building-records, building-local-frames,
//           grm-owner, grm-residual, grm-distance-threshold,
//           boundary-corners-roof-graph (per-bldg)
//   writes: boundary-polygons-roof-graph (per-bldg ARRAY_INLINE of TLV)
//
// Outputs a POLYGON_PATHS section (0x0B) per plane. EDGES section
// emission is deferred — needed only when this step is wired into the
// downstream merge/regularize chain (pipeline switch task #297e).
//
// Upstream TLV parsers (parse_corners / parse_grids / parse_nearby) are
// copied from v7-boundary-polygons.c. bp1 will be deleted once bp2
// takes over; until then the duplication is intentional — per user
// directive not to refactor bp1's stable code for shared primitives.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-boundary-polygons-2-impl.h"
#include "v7-boundary-polygons-input.h"  // v7_bp_input_* (split from bp1, task #71)
#include "v7-corner-pool.h"               // v7p_src_priority for D.3 NMS
#include "v7-plane-grids-parse.h"
#include "v7-roof-graph-format.h"

#define BLF_RECORD_BYTES 72

// ── Params ──

static int parse_params(const char* params_str, v7_bp2_params_t* p) {
    v7_bp2_params_default(p);
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
#define INT_FIELD(KEY, FIELD) \
    if ((j = json_object_get(obj, KEY)) && json_is_integer(j)) p->FIELD = (int)json_integer_value(j)
#define DBL_FIELD(KEY, FIELD) \
    if ((j = json_object_get(obj, KEY)) && json_is_number(j))  p->FIELD = json_number_value(j)
    INT_FIELD("fullEnumMax",              full_enum_max);
    INT_FIELD("kRandom",                  k_random);
    DBL_FIELD("weightWsd",                weight_wsd);
    DBL_FIELD("weightCompactness",        weight_compactness);
    DBL_FIELD("weightSpike",              weight_spike);
    DBL_FIELD("spikeThresholdDeg",        spike_threshold_deg);
    DBL_FIELD("cornerToFragmentRangeM",   corner_to_fragment_range_m);
    INT_FIELD("oroptWindow",              oropt_window);
    DBL_FIELD("wallTiltMinDeg",           wall_tilt_min_deg);
    if ((j = json_object_get(obj, "upAxis")) && json_is_integer(j))
        p->up_axis = (int32_t)json_integer_value(j);
#undef INT_FIELD
#undef DBL_FIELD
    json_decref(obj);
    return 0;
}

// ── Upstream parse — corners, plane_grids, nearby, planes ─────────────
//
// Copied from v7-boundary-polygons.c (parse_corners_block, parse_plane_grids,
// parse_nearby). Behavior is byte-identical; this is duplication on purpose
// to avoid touching bp1 while bp2 stabilises.

// One plane's grid-fill contour topology — outer perimeter + zero or
// more interior holes, projected to plane UV in F64 (converted from the
// F32 wire format on parse). Owned buffers; freed in upstream_free.
typedef struct bp2_contour_storage {
    uint32_t plane_id;
    double*  outerU;     double*  outerV;     int n_outer;
    int      n_holes;
    double** holesU;     double** holesV;     int* n_per_hole;
} bp2_contour_storage_t;

typedef struct upstream {
    v7_bp_input_plane_t* planes;             size_t n_planes;
    v7_bp_input_corner_t* corners;           size_t n_corners;
    v7_bp_input_plane_grid_t* grids;         size_t n_grids;
    v7_bp_input_nearby_per_plane_t* nearby;  size_t n_nearby;
    bp2_contour_storage_t* contours;         size_t n_contours;
    float** owned_oa;       size_t n_owned_oa;
    float** owned_outu;     size_t n_owned_outu;
    float** owned_outv;     size_t n_owned_outv;
    float** owned_bal;      size_t n_owned_bal;
} upstream_t;

static void upstream_free(upstream_t* u) {
    if (!u) return;
    free(u->planes);
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
    free(u->grids);
    for (size_t i = 0; i < u->n_nearby; i++) free(u->nearby[i].items);
    free(u->nearby);
    for (size_t i = 0; i < u->n_contours; i++) {
        bp2_contour_storage_t* C = &u->contours[i];
        free(C->outerU); free(C->outerV);
        for (int h = 0; h < C->n_holes; h++) { free(C->holesU[h]); free(C->holesV[h]); }
        free(C->holesU); free(C->holesV); free(C->n_per_hole);
    }
    free(u->contours);
    for (size_t i = 0; i < u->n_owned_oa; i++)   free(u->owned_oa[i]);
    for (size_t i = 0; i < u->n_owned_outu; i++) free(u->owned_outu[i]);
    for (size_t i = 0; i < u->n_owned_outv; i++) free(u->owned_outv[i]);
    for (size_t i = 0; i < u->n_owned_bal; i++)  free(u->owned_bal[i]);
    free(u->owned_oa); free(u->owned_outu); free(u->owned_outv); free(u->owned_bal);
    memset(u, 0, sizeof(*u));
}

// (parse_corners_block / parse_plane_grids / parse_nearby — same code as bp1)

static int parse_corners_block(rg_reader_t* r, upstream_t* u,
                                double cx, double cy, double cz) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_CORNERS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 16;
    u->corners = (v7_bp_input_corner_t*)calloc(cap, sizeof(*u->corners));
    if (!u->corners) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 32) continue;
        if (u->n_corners == cap) {
            cap *= 2;
            v7_bp_input_corner_t* nb = (v7_bp_input_corner_t*)realloc(u->corners, cap * sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_corners, 0, (cap - u->n_corners) * sizeof(*nb));
            u->corners = nb;
        }
        v7_bp_input_corner_t* C = &u->corners[u->n_corners++];
        memcpy(&C->id, rec + 0, 4);
        C->source = rec[4];
        uint8_t flags = rec[5];
        double pw[3]; memcpy(&pw[0], rec + 8, 8); memcpy(&pw[1], rec + 16, 8); memcpy(&pw[2], rec + 24, 8);
        C->point[0] = pw[0] - cx; C->point[1] = pw[1] - cy; C->point[2] = pw[2] - cz;
        size_t off = 32;
        uint32_t pc; memcpy(&pc, rec + off, 4); off += 8;
        if (pc > 0) {
            C->plane_ids = (uint32_t*)malloc(pc * sizeof(uint32_t));
            if (!C->plane_ids) return -1;
            memcpy(C->plane_ids, rec + off, pc * 4);
            C->n_plane_ids = pc;
            off += pc * 4;
            C->source_per_plane = (uint8_t*)malloc(pc);
            if (!C->source_per_plane) return -1;
            memcpy(C->source_per_plane, rec + off, pc);
            off += pc;
        }
        if (rg_parse_padded_u32_array(rec, &off, &C->ridge_ids, &C->n_ridge_ids) != 0) return -1;
        if (flags & 0x01u) {
            uint32_t nl; memcpy(&nl, rec + off, 4); off += 8;
            if (nl > 0) {
                C->name = (char*)malloc(nl + 1);
                if (!C->name) return -1;
                memcpy(C->name, rec + off, nl); C->name[nl] = '\0';
                off += (nl + 7) & ~(size_t)7;
            }
        }
        if (flags & 0x04u) {
            uint32_t dc; memcpy(&dc, rec + off, 4); off += 8;
            if (dc > 0) {
                C->dirs = (v7_bp_input_corner_dir_t*)malloc(dc * sizeof(*C->dirs));
                if (!C->dirs) return -1;
                for (uint32_t k = 0; k < dc; k++) {
                    memcpy(&C->dirs[k].plane_id, rec + off + 0, 4);
                    memcpy(C->dirs[k].dir_a,     rec + off + 8, 12);
                    memcpy(C->dirs[k].dir_b,     rec + off + 20, 12);
                    off += 32;
                }
                C->n_dirs = dc;
            }
        }
        if (flags & 0x20u) {
            uint32_t cc; memcpy(&cc, rec + off, 4); off += 8;
            if (cc > 0) {
                C->com_bisectors = (v7_bp_input_corner_com_bisector_t*)malloc(cc * sizeof(*C->com_bisectors));
                if (!C->com_bisectors) return -1;
                for (uint32_t k = 0; k < cc; k++) {
                    memcpy(&C->com_bisectors[k].plane_id,    rec + off + 0, 4);
                    memcpy(C->com_bisectors[k].com_bisector, rec + off + 4, 12);
                    off += 16;
                }
                C->n_com_bisectors = cc;
            }
        }
        if (flags & 0x40u) {
            uint32_t mc; memcpy(&mc, rec + off, 4); off += 8;
            if (mc > 0) {
                C->members = (v7_bc_member_t*)calloc(mc, sizeof(*C->members));
                if (!C->members) return -1;
                for (uint32_t k = 0; k < mc; k++) {
                    double mpw[3];
                    memcpy(&mpw[0], rec + off + 0,  8);
                    memcpy(&mpw[1], rec + off + 8,  8);
                    memcpy(&mpw[2], rec + off + 16, 8);
                    C->members[k].point[0] = mpw[0] - cx;
                    C->members[k].point[1] = mpw[1] - cy;
                    C->members[k].point[2] = mpw[2] - cz;
                    C->members[k].source       = rec[off + 24];
                    C->members[k].authority    = (v7_bc_authority_t)rec[off + 25];
                    C->members[k].has_axis     = rec[off + 26] != 0;
                    C->members[k].has_ridge_id = rec[off + 27] != 0;
                    if (C->members[k].has_axis) memcpy(C->members[k].axis, rec + off + 28, 12);
                    if (C->members[k].has_ridge_id) memcpy(&C->members[k].ridge_id, rec + off + 40, 4);
                    memcpy(&C->members[k].plane_id, rec + off + 44, 4);
                    off += 48;
                }
                C->n_members = mc;
            }
        }
    }
    return 0;
}

static int parse_plane_grids(rg_reader_t* r, upstream_t* u) {
    // #72c Family A — extracted to v7-plane-grids-parse.c (shared with
    // boundary-corners). v7_bp_input_plane_grid_t is a typedef alias of
    // v7_bc_input_plane_grid_t so the cast in the helper is identity.
    return v7_parse_plane_grids_section(r,
        &u->grids,     &u->n_grids,
        &u->owned_oa,   &u->n_owned_oa,
        &u->owned_outu, &u->n_owned_outu,
        &u->owned_outv, &u->n_owned_outv,
        &u->owned_bal,  &u->n_owned_bal);
}

// Parse PLANE_CONTOURS (RG_SECT_PLANE_CONTOURS = 0x11) — one record per
// plane that produced a grid-fill contour. Skip the inspection grids
// (flag 0x01) and nearby grids (flag 0x08) — bp3 only needs the outer
// loop (flag 0x02) and holes (flag 0x04), both as plane-UV polygons.
// Wire layout: see v7-boundary-signals.c writer + ROOF-GRAPH-FORMAT.md
// §4.17. UV vertices are F32 on the wire, converted to F64 here so the
// impl's v7_pt_edge_dist (which takes double*) can use them directly.
static int parse_plane_contours(rg_reader_t* r, upstream_t* u) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_PLANE_CONTOURS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 8;
    u->contours = (bp2_contour_storage_t*)calloc(cap, sizeof(*u->contours));
    if (!u->contours) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 40) continue;
        uint32_t plane_id, flags, n_cols, n_rows;
        memcpy(&plane_id, rec + 0, 4);
        memcpy(&flags,    rec + 4, 4);
        memcpy(&n_cols,   rec + 8, 4);
        memcpy(&n_rows,   rec + 12, 4);
        size_t n_cells = (size_t)n_cols * n_rows;
        size_t off = 40;
        if (flags & 0x01u) {
            // Skip density + smoothed + binary (padded).
            off += n_cells * 4 + n_cells * 4;
            size_t bin_padded = (n_cells + 7) & ~(size_t)7;
            off += bin_padded;
        }
        if (flags & 0x08u) {
            off += n_cells * 4 + n_cells * 4;
        }
        if (!(flags & 0x02u)) continue;  // no outer → can't build a polygon

        if (u->n_contours == cap) {
            cap *= 2;
            bp2_contour_storage_t* nb = (bp2_contour_storage_t*)realloc(
                u->contours, cap * sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_contours, 0, (cap - u->n_contours) * sizeof(*nb));
            u->contours = nb;
        }
        bp2_contour_storage_t* C = &u->contours[u->n_contours++];
        C->plane_id = plane_id;

        // Outer loop.
        uint32_t n_outer;
        memcpy(&n_outer, rec + off, 4);
        off += 8;  // n_outer + pad
        if (n_outer >= 3 && off + (size_t)n_outer * 2 * 4 <= rec_len) {
            C->n_outer = (int)n_outer;
            C->outerU  = (double*)malloc((size_t)n_outer * sizeof(double));
            C->outerV  = (double*)malloc((size_t)n_outer * sizeof(double));
            if (!C->outerU || !C->outerV) return -1;
            for (uint32_t k = 0; k < n_outer; k++) {
                float u_f, v_f;
                memcpy(&u_f, rec + off + (size_t)k * 8 + 0, 4);
                memcpy(&v_f, rec + off + (size_t)k * 8 + 4, 4);
                C->outerU[k] = (double)u_f;
                C->outerV[k] = (double)v_f;
            }
        }
        off += (size_t)n_outer * 2 * 4;

        if (flags & 0x04u) {
            uint32_t n_holes;
            memcpy(&n_holes, rec + off, 4);
            off += 8;  // n_holes + pad
            if (n_holes > 0) {
                C->n_holes      = (int)n_holes;
                C->holesU       = (double**)calloc(n_holes, sizeof(double*));
                C->holesV       = (double**)calloc(n_holes, sizeof(double*));
                C->n_per_hole   = (int*)calloc(n_holes, sizeof(int));
                if (!C->holesU || !C->holesV || !C->n_per_hole) return -1;
                for (uint32_t h = 0; h < n_holes; h++) {
                    uint32_t hn;
                    memcpy(&hn, rec + off, 4);
                    off += 8;  // hn + pad
                    if (hn < 3 || off + (size_t)hn * 2 * 4 > rec_len) {
                        off += (size_t)hn * 2 * 4;
                        continue;
                    }
                    C->n_per_hole[h] = (int)hn;
                    C->holesU[h] = (double*)malloc((size_t)hn * sizeof(double));
                    C->holesV[h] = (double*)malloc((size_t)hn * sizeof(double));
                    if (!C->holesU[h] || !C->holesV[h]) return -1;
                    for (uint32_t k = 0; k < hn; k++) {
                        float u_f, v_f;
                        memcpy(&u_f, rec + off + (size_t)k * 8 + 0, 4);
                        memcpy(&v_f, rec + off + (size_t)k * 8 + 4, 4);
                        C->holesU[h][k] = (double)u_f;
                        C->holesV[h][k] = (double)v_f;
                    }
                    off += (size_t)hn * 2 * 4;
                }
            }
        }
    }
    return 0;
}

static int parse_nearby(rg_reader_t* r, upstream_t* u, double cx, double cy, double cz) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_NEARBY_PROJECTIONS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    size_t cap = 8;
    u->nearby = (v7_bp_input_nearby_per_plane_t*)calloc(cap, sizeof(*u->nearby));
    if (!u->nearby) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 8) continue;
        if (u->n_nearby == cap) {
            cap *= 2;
            v7_bp_input_nearby_per_plane_t* nb = (v7_bp_input_nearby_per_plane_t*)realloc(u->nearby, cap * sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_nearby, 0, (cap - u->n_nearby) * sizeof(*nb));
            u->nearby = nb;
        }
        v7_bp_input_nearby_per_plane_t* N = &u->nearby[u->n_nearby++];
        memcpy(&N->plane_id, rec + 0, 4);
        memcpy(&N->n_items,  rec + 4, 4);
        if (N->n_items == 0) continue;
        N->items = (v7_bp_input_nearby_item_t*)calloc(N->n_items, sizeof(*N->items));
        if (!N->items) return -1;
        size_t off = 8;
        for (uint32_t k = 0; k < N->n_items; k++) {
            double pw[3]; memcpy(&pw[0], rec + off + 0, 8); memcpy(&pw[1], rec + off + 8, 8); memcpy(&pw[2], rec + off + 16, 8);
            N->items[k].position[0] = pw[0] - cx;
            N->items[k].position[1] = pw[1] - cy;
            N->items[k].position[2] = pw[2] - cz;
            memcpy(&N->items[k].signed_dist, rec + off + 24, 4);
            memcpy(&N->items[k].src_idx,     rec + off + 28, 4);
            off += 40;
        }
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
            u->planes = (v7_bp_input_plane_t*)calloc(n, sizeof(*u->planes));
            if (!u->planes) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_plane_record_t* P = (const rg_plane_record_t*)(base + (size_t)i * stride);
                u->planes[i].id = P->id;
                u->planes[i].rejected = (P->flags & RG_PLANE_REJECTED) != 0;
                u->planes[i].tilt_deg = (double)P->tilt_deg;
                u->planes[i].normal[0] = P->normal[0];
                u->planes[i].normal[1] = P->normal[1];
                u->planes[i].normal[2] = P->normal[2];
                u->planes[i].d = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                memcpy(u->planes[i].basis_e1, P->basis_e1, 24);
                memcpy(u->planes[i].basis_e2, P->basis_e2, 24);
            }
            u->n_planes = n;
        }
    }
    if (parse_corners_block(&r, u, cx, cy, cz) != 0) return -1;
    if (parse_plane_grids(&r, u) != 0) return -1;
    if (parse_plane_contours(&r, u) != 0) return -1;
    if (parse_nearby(&r, u, cx, cy, cz) != 0) return -1;
    return 0;
}

// ── Per-plane bp2 driver ────────────────────────────────────────────

typedef struct out_poly {
    uint32_t plane_id;
    uint32_t* corner_ids;
    uint32_t n_corners;
} out_poly_t;

static int process_plane(const v7_bp_input_plane_t* P,
                          const v7_bp_input_corner_t* corners, size_t n_corners,
                          const v7_bp_input_plane_grid_t* grids, size_t n_grids,
                          const v7_bp_input_nearby_per_plane_t* nearby, size_t n_nearby,
                          const bp2_contour_storage_t* contours, size_t n_contours,
                          const double* local_pos, size_t n_local_pos,
                          const int16_t* owner,
                          const v7_bp2_params_t* params,
                          out_poly_t* out) {
    out->plane_id = P->id;
    out->corner_ids = NULL;
    out->n_corners = 0;

    if (P->rejected) return 0;

    // Build anchor list — corners with this plane in plane_ids.
    // Per-plane position comes from members[] when present (matches bp1
    // D.3-input behavior); falls back to the corner's pool point otherwise.
    size_t cap = 32;
    v7_bp2_anchor_t* anchors = (v7_bp2_anchor_t*)malloc(cap * sizeof(*anchors));
    if (!anchors) return -1;
    size_t n_anchors = 0;
    for (size_t i = 0; i < n_corners; i++) {
        const v7_bp_input_corner_t* C = &corners[i];
        bool in_plane = false;
        for (uint32_t k = 0; k < C->n_plane_ids; k++) if (C->plane_ids[k] == P->id) { in_plane = true; break; }
        if (!in_plane) continue;
        double point[3] = { C->point[0], C->point[1], C->point[2] };
        for (uint32_t k = 0; k < C->n_members; k++) {
            if (C->members[k].plane_id == P->id) {
                point[0] = C->members[k].point[0];
                point[1] = C->members[k].point[1];
                point[2] = C->members[k].point[2];
                break;
            }
        }
        if (n_anchors == cap) {
            cap *= 2;
            v7_bp2_anchor_t* nb = (v7_bp2_anchor_t*)realloc(anchors, cap * sizeof(*nb));
            if (!nb) { free(anchors); return -1; }
            anchors = nb;
        }
        anchors[n_anchors].id = C->id;
        anchors[n_anchors].point[0] = point[0];
        anchors[n_anchors].point[1] = point[1];
        anchors[n_anchors].point[2] = point[2];
        anchors[n_anchors].source   = C->source;
        n_anchors++;
    }
    if (n_anchors < 3) { free(anchors); return 0; }

    // ── D.3 NMS dedup (matches bp1 D.3 + v2 bp2 wrapper) ────────────
    // Sort by source priority (ridge family > concave > convex > coplanar),
    // then walk and drop any anchor within 0.40 m of an already-kept one.
    // bp1 also uses span_deg as a tiebreak; we don't have it in bp2's anchor
    // struct yet, so priority-only is the v3-bp2 approximation. Without this
    // step, plane 5 keeps ~30 anchors clustered <0.5m apart and the TSP cycle
    // becomes a 100m zigzag instead of a clean ~50m perimeter (2026-05-29).
    {
        // Stable sort idx[] by source priority descending.
        uint32_t* idx = (uint32_t*)malloc(n_anchors * sizeof(uint32_t));
        if (!idx) { free(anchors); return -1; }
        for (size_t i = 0; i < n_anchors; i++) idx[i] = (uint32_t)i;
        for (size_t i = 1; i < n_anchors; i++) {
            uint32_t cur = idx[i];
            int pcur = v7p_src_priority(anchors[cur].source);
            size_t j = i;
            while (j > 0) {
                int pprev = v7p_src_priority(anchors[idx[j-1]].source);
                if (pprev >= pcur) break;
                idx[j] = idx[j-1]; j--;
            }
            idx[j] = cur;
        }
        // Walk in priority order, keep if no kept anchor is within 0.40 m.
        const double DEDUP_RSQ = 0.40 * 0.40;
        v7_bp2_anchor_t* kept = (v7_bp2_anchor_t*)malloc(n_anchors * sizeof(*kept));
        if (!kept) { free(idx); free(anchors); return -1; }
        size_t n_kept = 0;
        for (size_t k = 0; k < n_anchors; k++) {
            const v7_bp2_anchor_t* a = &anchors[idx[k]];
            bool drop = false;
            for (size_t m = 0; m < n_kept; m++) {
                double dx = a->point[0] - kept[m].point[0];
                double dy = a->point[1] - kept[m].point[1];
                double dz = a->point[2] - kept[m].point[2];
                if (dx*dx + dy*dy + dz*dz < DEDUP_RSQ) { drop = true; break; }
            }
            if (!drop) kept[n_kept++] = *a;
        }
        free(idx); free(anchors);
        anchors = kept; n_anchors = n_kept;
        if (n_anchors < 3) { free(anchors); return 0; }
    }

    // Group points: lidar-positions whose owner == plane_id. Note: positions
    // are tile-global; local_pos is bldg-local already.
    size_t n_group = 0;
    for (size_t i = 0; i < n_local_pos; i++) if (owner[i] >= 0 && (uint32_t)owner[i] == P->id) n_group++;
    double* group_pts = NULL;
    if (n_group > 0) {
        group_pts = (double*)malloc(n_group * 3 * sizeof(double));
        if (!group_pts) { free(anchors); return -1; }
        size_t k = 0;
        for (size_t i = 0; i < n_local_pos; i++) {
            if (owner[i] >= 0 && (uint32_t)owner[i] == P->id) {
                group_pts[k*3+0] = local_pos[i*3+0];
                group_pts[k*3+1] = local_pos[i*3+1];
                group_pts[k*3+2] = local_pos[i*3+2];
                k++;
            }
        }
    }

    // Nearby points for this plane.
    size_t n_nearby_pts = 0;
    double* nearby_pts = NULL;
    for (size_t i = 0; i < n_nearby; i++) {
        if (nearby[i].plane_id == P->id) {
            n_nearby_pts = nearby[i].n_items;
            if (n_nearby_pts > 0) {
                nearby_pts = (double*)malloc(n_nearby_pts * 3 * sizeof(double));
                if (!nearby_pts) { free(anchors); free(group_pts); return -1; }
                for (size_t k = 0; k < n_nearby_pts; k++) {
                    nearby_pts[k*3+0] = nearby[i].items[k].position[0];
                    nearby_pts[k*3+1] = nearby[i].items[k].position[1];
                    nearby_pts[k*3+2] = nearby[i].items[k].position[2];
                }
            }
            break;
        }
    }

    (void)grids; (void)n_grids;  // bp3 no longer uses grids — kept in signature for callers

    // Look up this plane's grid-fill contour and build the flat
    // v7_bp2_contour_t[] (outer + holes) the impl consumes.
    v7_bp2_contour_t* contour_arr = NULL;
    size_t n_contour_arr = 0;
    for (size_t i = 0; i < n_contours; i++) {
        if (contours[i].plane_id != P->id) continue;
        const bp2_contour_storage_t* C = &contours[i];
        size_t need = (C->n_outer >= 3 ? 1u : 0u) + (size_t)C->n_holes;
        if (need == 0) break;
        contour_arr = (v7_bp2_contour_t*)calloc(need, sizeof(*contour_arr));
        if (!contour_arr) { free(anchors); free(group_pts); free(nearby_pts); return -1; }
        if (C->n_outer >= 3) {
            contour_arr[n_contour_arr].polyU   = C->outerU;
            contour_arr[n_contour_arr].polyV   = C->outerV;
            contour_arr[n_contour_arr].n_pts   = C->n_outer;
            contour_arr[n_contour_arr].is_hole = false;
            n_contour_arr++;
        }
        for (int h = 0; h < C->n_holes; h++) {
            if (C->n_per_hole[h] < 3) continue;
            contour_arr[n_contour_arr].polyU   = C->holesU[h];
            contour_arr[n_contour_arr].polyV   = C->holesV[h];
            contour_arr[n_contour_arr].n_pts   = C->n_per_hole[h];
            contour_arr[n_contour_arr].is_hole = true;
            n_contour_arr++;
        }
        break;
    }

    // Wall classification: tilt_deg (from horizontal) high → wall plane.
    bool is_wall = (P->tilt_deg >= params->wall_tilt_min_deg);

    v7_bp2_inputs_t in = {0};
    in.anchors = anchors; in.n_anchors = n_anchors;
    in.group_pts = group_pts; in.n_group_pts = n_group;
    in.nearby_pts = nearby_pts; in.n_nearby_pts = n_nearby_pts;
    in.basis.e1[0] = P->basis_e1[0]; in.basis.e1[1] = P->basis_e1[1]; in.basis.e1[2] = P->basis_e1[2];
    in.basis.e2[0] = P->basis_e2[0]; in.basis.e2[1] = P->basis_e2[1]; in.basis.e2[2] = P->basis_e2[2];
    in.basis.normal[0] = P->normal[0]; in.basis.normal[1] = P->normal[1]; in.basis.normal[2] = P->normal[2];
    in.contours   = contour_arr;
    in.n_contours = n_contour_arr;
    in.is_wall    = is_wall;

    v7_bp2_output_t out_struct; v7_bp2_output_init(&out_struct);
    int rc = v7_bp2_compute(&in, &out_struct);
    free(contour_arr);
    free(anchors); free(group_pts); free(nearby_pts);
    if (rc != 0) { v7_bp2_output_free(&out_struct); return rc < 0 ? -1 : 0; }
    if (out_struct.n_polygons < 1) { v7_bp2_output_free(&out_struct); return 0; }

    const v7_bp2_polygon_t* poly = &out_struct.polygons[0];
    out->corner_ids = (uint32_t*)malloc(poly->n_corners * sizeof(uint32_t));
    if (!out->corner_ids) { v7_bp2_output_free(&out_struct); return -1; }
    memcpy(out->corner_ids, poly->corner_ids, poly->n_corners * sizeof(uint32_t));
    out->n_corners = (uint32_t)poly->n_corners;
    v7_bp2_output_free(&out_struct);
    return 0;
}

// ── Emit POLYGON_PATHS + EDGES sections ─────────────────────────────

// Linear-scan edge accumulator. Polygon counts are small (≤ ~hundreds
// of edges per building) so O(K·E) is fine. Each edge stores the set
// of plane_ids that visit it — shared-edge consumers downstream
// (boundary-merge) key on this.
typedef struct edge_acc {
    uint32_t corner_a, corner_b;     // sorted (a < b)
    double   dir[3]; double length;
    uint32_t* plane_ids; uint32_t n_plane_ids; uint32_t cap_plane_ids;
} edge_acc_t;

static const v7_bp_input_corner_t* find_corner_by_id(
        const v7_bp_input_corner_t* corners, size_t n, uint32_t id) {
    for (size_t i = 0; i < n; i++) if (corners[i].id == id) return &corners[i];
    return NULL;
}

static int emit_edges(rg_writer_t* w,
                       const out_poly_t* polys, size_t n_polys,
                       const v7_bp_input_corner_t* corners, size_t n_corners) {
    edge_acc_t* edges = NULL; size_t n_edges = 0, cap = 0;

    for (size_t pi = 0; pi < n_polys; pi++) {
        const out_poly_t* P = &polys[pi];
        if (P->n_corners < 3) continue;
        for (uint32_t k = 0; k < P->n_corners; k++) {
            uint32_t a = P->corner_ids[k];
            uint32_t b = P->corner_ids[(k + 1) % P->n_corners];
            if (a == b) continue;
            uint32_t aa = a < b ? a : b;
            uint32_t bb = a < b ? b : a;
            edge_acc_t* E = NULL;
            for (size_t e = 0; e < n_edges; e++) {
                if (edges[e].corner_a == aa && edges[e].corner_b == bb) { E = &edges[e]; break; }
            }
            if (!E) {
                if (n_edges == cap) {
                    cap = cap ? cap * 2 : 64;
                    edges = (edge_acc_t*)realloc(edges, cap * sizeof(*edges));
                    if (!edges) return -1;
                }
                E = &edges[n_edges++];
                memset(E, 0, sizeof(*E));
                E->corner_a = aa; E->corner_b = bb;
                const v7_bp_input_corner_t* Ca = find_corner_by_id(corners, n_corners, aa);
                const v7_bp_input_corner_t* Cb = find_corner_by_id(corners, n_corners, bb);
                if (Ca && Cb) {
                    double dx = Cb->point[0] - Ca->point[0];
                    double dy = Cb->point[1] - Ca->point[1];
                    double dz = Cb->point[2] - Ca->point[2];
                    double len = sqrt(dx*dx + dy*dy + dz*dz);
                    E->length = len;
                    if (len > 0) { E->dir[0] = dx/len; E->dir[1] = dy/len; E->dir[2] = dz/len; }
                }
            }
            // dedupe plane_ids
            bool present = false;
            for (uint32_t p = 0; p < E->n_plane_ids; p++) {
                if (E->plane_ids[p] == P->plane_id) { present = true; break; }
            }
            if (!present) {
                if (E->n_plane_ids == E->cap_plane_ids) {
                    E->cap_plane_ids = E->cap_plane_ids ? E->cap_plane_ids * 2 : 4;
                    E->plane_ids = (uint32_t*)realloc(E->plane_ids, E->cap_plane_ids * sizeof(uint32_t));
                    if (!E->plane_ids) { /* leak edges; caller bails */ return -1; }
                }
                E->plane_ids[E->n_plane_ids++] = P->plane_id;
            }
        }
    }

    int rc = 0;
    if (rg_writer_section_begin(w, RG_SECT_EDGES, 0) != 0) { rc = -1; goto cleanup; }
    for (size_t i = 0; i < n_edges; i++) {
        const edge_acc_t* E = &edges[i];
        uint32_t id = (uint32_t)i;
        size_t header = 32;
        size_t pids_block = 8 + (size_t)E->n_plane_ids * 4;
        pids_block = (pids_block + 7) & ~(size_t)7;
        size_t rec_len = header + pids_block;
        uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
        if (!rec) { rc = -1; break; }
        memcpy(rec + 0,  &id, 4);
        // flags @ +4 already zero
        memcpy(rec + 8,  &E->corner_a, 4);
        memcpy(rec + 12, &E->corner_b, 4);
        float dir_f[3] = { (float)E->dir[0], (float)E->dir[1], (float)E->dir[2] };
        memcpy(rec + 16, dir_f, 12);
        float len_f = (float)E->length;
        memcpy(rec + 28, &len_f, 4);
        uint32_t pcount = E->n_plane_ids, pad = 0;
        memcpy(rec + 32, &pcount, 4);
        memcpy(rec + 36, &pad,    4);
        for (uint32_t p = 0; p < E->n_plane_ids; p++) {
            memcpy(rec + 40 + (size_t)p * 4, &E->plane_ids[p], 4);
        }
        if (rg_writer_record(w, rec, rec_len) != 0) { free(rec); rc = -1; break; }
        free(rec);
    }
    if (rc == 0) rc = rg_writer_section_end(w);

cleanup:
    for (size_t i = 0; i < n_edges; i++) free(edges[i].plane_ids);
    free(edges);
    return rc;
}

static int emit_polygon_paths(rg_writer_t* w, const out_poly_t* polys, size_t n) {
    if (n == 0) return 0;
    if (rg_writer_section_begin(w, RG_SECT_POLYGON_PATHS, 0) != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        const out_poly_t* P = &polys[i];
        if (P->n_corners < 3) continue;
        size_t body = 8 + (size_t)P->n_corners * 4;
        size_t pad  = (8 - (body & 7u)) & 7u;
        size_t rec_len = body + pad;
        uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
        if (!rec) return -1;
        memcpy(rec + 0, &P->plane_id, 4);
        memcpy(rec + 4, &P->n_corners, 4);
        memcpy(rec + 8, P->corner_ids, (size_t)P->n_corners * 4);
        int rc = rg_writer_record(w, rec, rec_len);
        free(rec);
        if (rc != 0) return -1;
    }
    return rg_writer_section_end(w);
}

// ── process_bldg ────────────────────────────────────────────────────

static int process_bldg(rg_writer_t* w,
                         const uint8_t* up_bytes, size_t up_len,
                         const double* positions, size_t total_points,
                         const uint32_t* global_indices, uint32_t n_pts_local,
                         double cx, double cy, double cz,
                         const int16_t* owner,
                         const v7_bp2_params_t* params) {
    double* local_pos = (double*)malloc((size_t)n_pts_local * 3 * sizeof(double));
    if (!local_pos) return -1;
    for (uint32_t i = 0; i < n_pts_local; i++) {
        uint32_t gi = global_indices[i];
        if ((size_t)gi >= total_points) { free(local_pos); return -1; }
        local_pos[i*3 + 0] = positions[gi*3 + 0] - cx;
        local_pos[i*3 + 1] = positions[gi*3 + 1] - cy;
        local_pos[i*3 + 2] = positions[gi*3 + 2] - cz;
    }

    upstream_t up = {0};
    if (parse_upstream(up_bytes, up_len, cx, cy, cz, &up) != 0) {
        free(local_pos); upstream_free(&up); return -1;
    }

    // Per-plane compute.
    out_poly_t* polys = (out_poly_t*)calloc(up.n_planes, sizeof(out_poly_t));
    if (!polys) { free(local_pos); upstream_free(&up); return -1; }
    for (size_t pi = 0; pi < up.n_planes; pi++) {
        if (getenv("BP2_TRACE")) {
            fprintf(stderr, "[bp2] plane %u/%zu (id=%u tilt=%.1f) starting\n",
                    (uint32_t)pi, up.n_planes, up.planes[pi].id, up.planes[pi].tilt_deg);
        }
        if (process_plane(&up.planes[pi],
                            up.corners, up.n_corners,
                            up.grids, up.n_grids,
                            up.nearby, up.n_nearby,
                            up.contours, up.n_contours,
                            local_pos, n_pts_local, owner,
                            params, &polys[pi]) != 0) {
            fprintf(stderr, "[v7-boundary-polygons-2] plane=%u failed\n", up.planes[pi].id);
        }
    }
    free(local_pos);

    // Pass-through every upstream section unchanged.
    rg_reader_t r;
    if (rg_reader_open(&r, up_bytes, up_len) != 0) {
        for (size_t pi = 0; pi < up.n_planes; pi++) free(polys[pi].corner_ids);
        free(polys); upstream_free(&up); return -1;
    }
    if (rg_writer_passthrough(w, &r, RG_SECT_NONE) != 0) {
        for (size_t pi = 0; pi < up.n_planes; pi++) free(polys[pi].corner_ids);
        free(polys); upstream_free(&up); return -1;
    }

    // Emit POLYGON_PATHS + EDGES.
    int rc = emit_polygon_paths(w, polys, up.n_planes);
    if (rc == 0) rc = emit_edges(w, polys, up.n_planes, up.corners, up.n_corners);
    for (size_t pi = 0; pi < up.n_planes; pi++) free(polys[pi].corner_ids);
    free(polys); upstream_free(&up);
    return rc;
}

// ── Exec ────────────────────────────────────────────────────────────

static int v7_boundary_polygons_2_exec(step_context_t* ctx) {
    v7_bp2_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    v7_bp2_params_apply(&params);
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-boundary-polygons-2 scope=tile range=%.4f wall_tilt_min_deg=%.3f w_wsd=%.3f w_comp=%.3f w_spike=%.3f spike_thresh=%.3f full_enum_max=%d k_random=%d oropt_window=%d\n",
                params.corner_to_fragment_range_m,
                params.wall_tilt_min_deg,
                params.weight_wsd, params.weight_compactness, params.weight_spike,
                params.spike_threshold_deg,
                params.full_enum_max, params.k_random, params.oropt_window);
    }

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "grm-owner")                  != n_buildings ||
        step_array_len(ctx, "building-local-frames")      != n_buildings ||
        step_array_len(ctx, "boundary-corners-roof-graph")!= n_buildings) {
        fprintf(stderr, "[v7-boundary-polygons-2] count mismatch across per-bldg arrays\n");
        return -1;
    }

    blob_t b_roof = {0};
    int rc = 0;
    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec_bytes = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 68) { blob_push(&b_roof, NULL, 0); continue; }
        uint32_t schema, n_verts, n_pts_local;
        memcpy(&schema,      rec_bytes + 0, 4);
        memcpy(&n_verts,     rec_bytes + 4, 4);
        memcpy(&n_pts_local, rec_bytes + 8, 4);
        if (schema != 1) { blob_push(&b_roof, NULL, 0); continue; }
        const size_t idx_off = 68 + (size_t)n_verts * 24;
        if (idx_off + (size_t)n_pts_local * 4 > rec_len) { blob_push(&b_roof, NULL, 0); continue; }
        const uint32_t* global_indices = (const uint32_t*)(rec_bytes + idx_off);

        const uint8_t* frame_bytes = NULL; size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) { blob_push(&b_roof, NULL, 0); continue; }
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts_local * sizeof(int16_t)) {
            blob_push(&b_roof, NULL, 0); continue;
        }
        const int16_t* owner = (const int16_t*)own_bytes;

        const uint8_t* up_bytes = NULL; size_t up_len = 0;
        if (step_array_read_bytes(ctx, "boundary-corners-roof-graph", bi, &up_bytes, &up_len) != 0
            || !up_bytes || up_len < 32) {
            blob_push(&b_roof, NULL, 0); continue;
        }

        rg_writer_t w; rg_writer_init(&w);
        int prc = process_bldg(&w, up_bytes, up_len,
                                positions, total_points,
                                global_indices, n_pts_local,
                                cx, cy, cz, owner,
                                &params);
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

    if (rc == 0) rc = emit_blob(ctx, "boundary-polygons-roof-graph", &b_roof);
    blob_free(&b_roof);
    return rc;
}

// ── Step descriptor ────────────────────────────────────────────────

static const step_read_t v7_boundary_polygons_2_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",         .array = true},
    {.name = "building-local-frames",        .array = true},
    {.name = "grm-owner",                    .array = true},
    {.name = "boundary-corners-roof-graph",  .array = true},
};

static const step_write_t v7_boundary_polygons_2_writes[] = {
    {.name = "boundary-polygons-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_polygons_2_def = {
    .name             = "v7-boundary-polygons-2",
    .version          = 5,  // v5: arc-length contour ordering + windowed or-opt (2026-05-30, stripe-zigzag fix); v4: D.3 NMS dedup; v3: or-opt stale-v fix; v2: bp3 topology from RG_SECT_PLANE_CONTOURS
    .reads            = v7_boundary_polygons_2_reads,
    .n_reads          = sizeof(v7_boundary_polygons_2_reads) / sizeof(v7_boundary_polygons_2_reads[0]),
    .writes           = v7_boundary_polygons_2_writes,
    .n_writes         = sizeof(v7_boundary_polygons_2_writes) / sizeof(v7_boundary_polygons_2_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_polygons_2_exec,
};
