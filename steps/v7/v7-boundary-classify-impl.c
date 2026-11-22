// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-classify — algorithm impl. Port of bench-v2
// boundary-classify.js (Phase F perimeter edge classification).

#define _POSIX_C_SOURCE 200809L

#include "v7-boundary-classify-impl.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Defaults ──────────────────────────────────────────────────────

void v7_bclass_params_default(v7_bclass_params_t* p) {
    if (!p) return;
    p->perim_dist_3d           = 0.30;
    p->perim_dist_xz           = 0.30;
    p->perim_collinear_min_deg = 15.0;
    p->perim_min_overlap_m     = 0.30;
    p->wall_tilt_min_deg       = 72.0;
    p->pip_probe_offset_m      = 0.15;
    p->pip_step_len_m          = 0.10;
    p->pip_coverage_threshold  = 0.50;
    p->outward_side_eps        = 0.05;
    p->min_edge_len_3d         = 0.01;
    p->min_edge_len_xz         = 0.01;
    p->up_axis                 = 2;
}

// ── Outputs lifecycle ─────────────────────────────────────────────

void v7_bclass_outputs_init(v7_bclass_outputs_t* o) {
    if (o) memset(o, 0, sizeof(*o));
}

void v7_bclass_outputs_free(v7_bclass_outputs_t* o) {
    if (!o) return;
    if (o->edges) {
        for (size_t i = 0; i < o->n_edges; i++) free(o->edges[i].coverages);
        free(o->edges);
    }
    free(o->new_adjacencies);
    memset(o, 0, sizeof(*o));
}

// ── Internal: id → index maps (dense, sized to max_id + 1) ────────
//
// Plane and corner ids may have gaps (corners get new ids in merge);
// allocate arrays sized to max_id+1, initialize to UINT32_MAX, and use
// (uint32_t)-1 as "absent". Linear-time build, O(1) lookup.

typedef struct id_map {
    uint32_t* idx_by_id;   // [max_id+1], UINT32_MAX = absent
    uint32_t  cap;
} id_map_t;

static int id_map_build(id_map_t* m, uint32_t max_id_plus_one) {
    m->cap = max_id_plus_one;
    if (m->cap == 0) { m->idx_by_id = NULL; return 0; }
    m->idx_by_id = (uint32_t*)safe_malloc_array(m->cap, sizeof(uint32_t));
    if (!m->idx_by_id) return -1;
    for (uint32_t i = 0; i < m->cap; i++) m->idx_by_id[i] = UINT32_MAX;
    return 0;
}
static void id_map_free(id_map_t* m) { free(m->idx_by_id); m->idx_by_id = NULL; m->cap = 0; }
static inline uint32_t id_map_lookup(const id_map_t* m, uint32_t id) {
    if (!m->idx_by_id || id >= m->cap) return UINT32_MAX;
    return m->idx_by_id[id];
}
static inline void id_map_set(id_map_t* m, uint32_t id, uint32_t idx) {
    if (id < m->cap) m->idx_by_id[id] = idx;
}

// ── Internal scratch types ────────────────────────────────────────

typedef struct edge_geo {
    uint32_t edge_idx;       // index into input edges[]
    uint32_t primary_plane;  // edges[i].plane_ids[0]
    uint32_t plane_count;
    double   p1[3], p2[3];   // endpoints (xy-z, building-local)
    double   mx, my, mz;     // midpoint
    double   dx, dy, dz;     // 3D unit dir
    double   dxz_x, dxz_z;   // XZ unit dir (or 0,0 if degenerate)
    double   len3d, lenxz;
} edge_geo_t;

// One coverage candidate found in the inner loop; collected per outer
// edge then merged.
typedef struct cov_candidate {
    double   t0, t1;
    uint8_t  type;
    uint32_t other_plane_id;
    uint32_t other_edge_id;
} cov_candidate_t;

// ── Helpers ───────────────────────────────────────────────────────

static inline bool plane_is_wall(const v7_bclass_input_plane_t* p, double wall_tilt_min) {
    return p->tilt_deg >= wall_tilt_min;
}

// Signed area × 2 + XZ centroid for one polygon path (winding sign + centroid).
// Returns 0 if path < 3 valid corners; otherwise *out_signed_area2 is the
// signed double-area and *out_cx,*out_cz are the unweighted centroid.
static int polygon_winding_and_centroid_xz(const uint32_t* corner_ids, uint32_t n_corners,
                                            const v7_bclass_input_corner_t* corners,
                                            const id_map_t* corner_map,
                                            double* out_signed_area2,
                                            double* out_cx, double* out_cz) {
    if (n_corners < 3) return 0;
    double signed_area2 = 0;
    double cx = 0, cz = 0;
    uint32_t nv = 0;
    for (uint32_t i = 0; i < n_corners; i++) {
        uint32_t j = (i + 1) % n_corners;
        uint32_t ci = id_map_lookup(corner_map, corner_ids[i]);
        uint32_t cj = id_map_lookup(corner_map, corner_ids[j]);
        if (ci == UINT32_MAX || cj == UINT32_MAX) continue;
        double xi = corners[ci].point[0], zi = corners[ci].point[2];
        double xj = corners[cj].point[0], zj = corners[cj].point[2];
        signed_area2 += xi * zj - xj * zi;
        cx += xi; cz += zi; nv++;
    }
    *out_signed_area2 = signed_area2;
    if (nv > 0) { *out_cx = cx / nv; *out_cz = cz / nv; return 1; }
    return 0;
}

// Outward XZ unit vector for an edge anchored on its primary roof plane.
// Centroid-based when available; falls back to winding-based.
static void compute_outward_xz(const edge_geo_t* eg,
                                bool has_centroid, double cent_x, double cent_z,
                                bool has_winding, bool winding_ccw,
                                double* out_x, double* out_z) {
    if (has_centroid) {
        double to_mid_x = eg->mx - cent_x;
        double to_mid_z = eg->mz - cent_z;
        // Two candidates: (dxz_z, -dxz_x) and (-dxz_z, dxz_x). Project
        // (mid - centroid) onto candidate 1; positive sign → cand1 is outward.
        double dot1 = eg->dxz_z * to_mid_x + (-eg->dxz_x) * to_mid_z;
        if (dot1 >= 0) { *out_x =  eg->dxz_z; *out_z = -eg->dxz_x; }
        else            { *out_x = -eg->dxz_z; *out_z =  eg->dxz_x; }
        return;
    }
    bool ccw = has_winding ? winding_ccw : true;
    *out_x = ccw ?  eg->dxz_z : -eg->dxz_z;
    *out_z = ccw ? -eg->dxz_x :  eg->dxz_x;
}

// Point-in-polygon (XZ projection, ray-cast).
static bool point_in_poly_xz(double px, double pz,
                              const double* xs, const double* zs, uint32_t n) {
    bool inside = false;
    uint32_t j = n - 1;
    for (uint32_t i = 0; i < n; i++) {
        double xi = xs[i], zi = zs[i];
        double xj = xs[j], zj = zs[j];
        if ((zi > pz) != (zj > pz)) {
            double xint = (xj - xi) * (pz - zi) / (zj - zi) + xi;
            if (px < xint) inside = !inside;
        }
        j = i;
    }
    return inside;
}

static int cmp_cov_by_t0(const void* a, const void* b) {
    double aa = ((const cov_candidate_t*)a)->t0;
    double bb = ((const cov_candidate_t*)b)->t0;
    if (aa < bb) return -1;
    if (aa > bb) return 1;
    return 0;
}

// ── Per-plane XZ polygon scratch (PIP fallback) ───────────────────

typedef struct xz_polygon {
    uint32_t plane_id;
    double*  xs;
    double*  zs;
    uint32_t n;
    double   nx, ny, nz, d;  // plane equation (y = -(nx*x+nz*z+d)/ny, when ny!=0)
    bool     eq_ok;          // false if ny is near zero (degenerate)
} xz_polygon_t;

static void xz_polygon_free(xz_polygon_t* p) {
    if (!p) return;
    free(p->xs); free(p->zs);
    memset(p, 0, sizeof(*p));
}

// ── Main compute ──────────────────────────────────────────────────

int v7_bclass_compute(const v7_bclass_input_plane_t* planes, size_t n_planes,
                       const v7_bclass_input_corner_t* corners, size_t n_corners,
                       const v7_bclass_input_edge_t* edges, size_t n_edges,
                       const v7_bclass_input_polygon_path_t* polygon_paths, size_t n_polygon_paths,
                       const v7_bclass_input_ridge_t* ridges, size_t n_ridges,
                       const v7_bclass_params_t* params,
                       v7_bclass_outputs_t* out) {
    if (!planes || !corners || !edges || !params || !out) return -1;
    v7_bclass_outputs_init(out);

    const double WALL_TILT_MIN = params->wall_tilt_min_deg;
    const double PERIM_DIST_3D_SQ = params->perim_dist_3d * params->perim_dist_3d;
    const double PERIM_DIST_XZ_SQ = params->perim_dist_xz * params->perim_dist_xz;
    const double PERIM_COLLINEAR_COS = cos(params->perim_collinear_min_deg * M_PI / 180.0);
    const double OUTWARD_EPS = params->outward_side_eps;
    const double MIN_LEN_3D = params->min_edge_len_3d;
    const double MIN_LEN_XZ = params->min_edge_len_xz;

    int rc = -1;
    id_map_t plane_map = {0}, corner_map = {0}, ridge_map = {0};

    // ── Build id maps ─────────────────────────────────────────────
    uint32_t max_plane_id = 0, max_corner_id = 0, max_ridge_id = 0;
    for (size_t i = 0; i < n_planes;  i++) if (planes[i].id  > max_plane_id)  max_plane_id  = planes[i].id;
    for (size_t i = 0; i < n_corners; i++) if (corners[i].id > max_corner_id) max_corner_id = corners[i].id;
    for (size_t i = 0; i < n_ridges;  i++) if (ridges[i].id  > max_ridge_id)  max_ridge_id  = ridges[i].id;
    if (id_map_build(&plane_map,  n_planes  ? max_plane_id  + 1 : 0) != 0) goto cleanup;
    if (id_map_build(&corner_map, n_corners ? max_corner_id + 1 : 0) != 0) goto cleanup;
    if (id_map_build(&ridge_map,  n_ridges  ? max_ridge_id  + 1 : 0) != 0) goto cleanup;
    for (size_t i = 0; i < n_planes;  i++) id_map_set(&plane_map,  planes[i].id,  (uint32_t)i);
    for (size_t i = 0; i < n_corners; i++) id_map_set(&corner_map, corners[i].id, (uint32_t)i);
    for (size_t i = 0; i < n_ridges;  i++) id_map_set(&ridge_map,  ridges[i].id,  (uint32_t)i);

    // ── Phase B: per-plane winding + XZ centroid ──────────────────
    // Indexed by plane index (i in planes[]). Sentinel: nv==0 means no
    // centroid was computable.
    int8_t*  plane_has_winding  = (int8_t*) safe_calloc_array(n_planes, sizeof(int8_t));
    bool*    plane_winding_ccw_arr = (bool*)safe_calloc_array(n_planes, sizeof(bool));
    bool*    plane_has_centroid = (bool*) safe_calloc_array(n_planes, sizeof(bool));
    double*  plane_cent_x = (double*)safe_calloc_array(n_planes, sizeof(double));
    double*  plane_cent_z = (double*)safe_calloc_array(n_planes, sizeof(double));
    // Map plane_id → polygon_path index (for PIP + winding lookup).
    uint32_t* plane_to_path = (uint32_t*)safe_malloc_array(n_planes, sizeof(uint32_t));
    if (!plane_has_winding || !plane_winding_ccw_arr || !plane_has_centroid ||
        !plane_cent_x || !plane_cent_z || !plane_to_path) {
        free(plane_has_winding); free(plane_winding_ccw_arr); free(plane_has_centroid);
        free(plane_cent_x); free(plane_cent_z); free(plane_to_path);
        goto cleanup;
    }
    for (size_t i = 0; i < n_planes; i++) plane_to_path[i] = UINT32_MAX;

    for (size_t pi = 0; pi < n_polygon_paths; pi++) {
        uint32_t plane_idx = id_map_lookup(&plane_map, polygon_paths[pi].plane_id);
        if (plane_idx == UINT32_MAX) continue;
        plane_to_path[plane_idx] = (uint32_t)pi;
        if (plane_is_wall(&planes[plane_idx], WALL_TILT_MIN)) continue;
        double sa2 = 0, cx = 0, cz = 0;
        int got = polygon_winding_and_centroid_xz(
            polygon_paths[pi].corner_ids, polygon_paths[pi].n_corners,
            corners, &corner_map, &sa2, &cx, &cz);
        plane_has_winding[plane_idx]    = 1;
        plane_winding_ccw_arr[plane_idx] = (sa2 > 0);
        if (got) {
            plane_has_centroid[plane_idx] = true;
            plane_cent_x[plane_idx] = cx;
            plane_cent_z[plane_idx] = cz;
        }
    }

    // ── Phase C: edge geometry ────────────────────────────────────
    edge_geo_t* eg = (edge_geo_t*)safe_calloc_array(n_edges, sizeof(edge_geo_t));
    bool* eg_ok = (bool*)safe_calloc_array(n_edges, sizeof(bool));
    if (!eg || !eg_ok) { free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
                          free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup; }

    for (size_t ei = 0; ei < n_edges; ei++) {
        const v7_bclass_input_edge_t* E = &edges[ei];
        if (E->n_plane_ids < 1) continue;
        uint32_t ca = id_map_lookup(&corner_map, E->corner_a);
        uint32_t cb = id_map_lookup(&corner_map, E->corner_b);
        if (ca == UINT32_MAX || cb == UINT32_MAX) continue;
        double p1x = corners[ca].point[0], p1y = corners[ca].point[1], p1z = corners[ca].point[2];
        double p2x = corners[cb].point[0], p2y = corners[cb].point[1], p2z = corners[cb].point[2];
        double dx = p2x - p1x, dy = p2y - p1y, dz = p2z - p1z;
        double len3d = sqrt(dx * dx + dy * dy + dz * dz);
        double lenxz = sqrt(dx * dx + dz * dz);
        if (len3d < MIN_LEN_3D) continue;
        eg[ei].edge_idx      = (uint32_t)ei;
        eg[ei].primary_plane = E->plane_ids[0];
        eg[ei].plane_count   = E->n_plane_ids;
        eg[ei].p1[0] = p1x; eg[ei].p1[1] = p1y; eg[ei].p1[2] = p1z;
        eg[ei].p2[0] = p2x; eg[ei].p2[1] = p2y; eg[ei].p2[2] = p2z;
        eg[ei].mx = 0.5 * (p1x + p2x);
        eg[ei].my = 0.5 * (p1y + p2y);
        eg[ei].mz = 0.5 * (p1z + p2z);
        eg[ei].dx = dx / len3d; eg[ei].dy = dy / len3d; eg[ei].dz = dz / len3d;
        eg[ei].dxz_x = lenxz > MIN_LEN_XZ ? dx / lenxz : 0.0;
        eg[ei].dxz_z = lenxz > MIN_LEN_XZ ? dz / lenxz : 0.0;
        eg[ei].len3d = len3d;
        eg[ei].lenxz = lenxz;
        eg_ok[ei] = true;
    }

    // ── Allocate outputs ─────────────────────────────────────────
    out->edges = (v7_bclass_edge_t*)safe_calloc_array(n_edges ? n_edges : 1, sizeof(v7_bclass_edge_t));
    if (!out->edges) { free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
                       free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup; }
    out->n_edges = n_edges;
    for (size_t ei = 0; ei < n_edges; ei++) {
        out->edges[ei].edge_id = edges[ei].id;
        out->edges[ei].tag     = V7_BCLASS_TAG_UNSET;
    }

    // ── Phase D: classify each edge ──────────────────────────────
    // Growable buffer for new adjacencies (eave-shared candidates),
    // deduped against existing ridges in Phase E.
    size_t adj_cap = 16;
    v7_bclass_new_adjacency_t* adj = (v7_bclass_new_adjacency_t*)safe_malloc_array(adj_cap, sizeof(*adj));
    size_t adj_n = 0;
    if (!adj) { free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
                free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup; }

    // Scratch coverage list (reused per outer edge).
    size_t cov_cap = 16;
    cov_candidate_t* cov = (cov_candidate_t*)safe_malloc_array(cov_cap, sizeof(*cov));
    if (!cov) { free(adj); free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
                free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup; }

    for (size_t ei = 0; ei < n_edges; ei++) {
        if (!eg_ok[ei]) continue;
        const v7_bclass_input_edge_t* E = &edges[ei];
        v7_bclass_edge_t* oe = &out->edges[ei];

        // Partition planeIds into wall/roof.
        uint32_t n_wall = 0, n_roof = 0;
        uint32_t first_roof = UINT32_MAX;
        for (uint32_t k = 0; k < E->n_plane_ids; k++) {
            uint32_t pi = id_map_lookup(&plane_map, E->plane_ids[k]);
            if (pi == UINT32_MAX) continue;
            if (plane_is_wall(&planes[pi], WALL_TILT_MIN)) n_wall++;
            else { n_roof++; if (first_roof == UINT32_MAX) first_roof = E->plane_ids[k]; }
        }

        if (n_roof == 0) {
            oe->tag = V7_BCLASS_TAG_WALL;
            out->stats.wall++;
            continue;
        }
        if (eg[ei].plane_count >= 2 && n_wall == 0) {
            oe->tag = V7_BCLASS_TAG_INTERIOR_SHARED;
            out->stats.interior_shared++;
            continue;
        }

        // Both corners share a ridge between two roof planes → interior-shared.
        {
            uint32_t ca = id_map_lookup(&corner_map, E->corner_a);
            uint32_t cb = id_map_lookup(&corner_map, E->corner_b);
            if (ca != UINT32_MAX && cb != UINT32_MAX) {
                bool ridge_shared = false;
                const v7_bclass_input_corner_t* C0 = &corners[ca];
                const v7_bclass_input_corner_t* C1 = &corners[cb];
                for (uint32_t r1 = 0; r1 < C1->n_ridge_ids && !ridge_shared; r1++) {
                    uint32_t rid = C1->ridge_ids[r1];
                    bool in_c0 = false;
                    for (uint32_t r0 = 0; r0 < C0->n_ridge_ids; r0++) {
                        if (C0->ridge_ids[r0] == rid) { in_c0 = true; break; }
                    }
                    if (!in_c0) continue;
                    uint32_t ridx = id_map_lookup(&ridge_map, rid);
                    if (ridx == UINT32_MAX) continue;
                    uint32_t pAi = id_map_lookup(&plane_map, ridges[ridx].plane_a);
                    uint32_t pBi = id_map_lookup(&plane_map, ridges[ridx].plane_b);
                    if (pAi == UINT32_MAX || pBi == UINT32_MAX) continue;
                    if (!plane_is_wall(&planes[pAi], WALL_TILT_MIN) &&
                        !plane_is_wall(&planes[pBi], WALL_TILT_MIN)) {
                        ridge_shared = true;
                    }
                }
                if (ridge_shared) {
                    oe->tag = V7_BCLASS_TAG_INTERIOR_SHARED;
                    out->stats.interior_shared++;
                    continue;
                }
            }
        }

        // Pair-matching loop. first_roof is the reference plane id.
        uint32_t ref_pid = first_roof;
        uint32_t ref_idx = id_map_lookup(&plane_map, ref_pid);
        if (ref_idx == UINT32_MAX) {
            oe->tag = V7_BCLASS_TAG_PERIMETER;
            out->stats.perimeter++;
            continue;
        }

        // Outward direction.
        double outX, outZ;
        compute_outward_xz(&eg[ei],
                            plane_has_centroid[ref_idx], plane_cent_x[ref_idx], plane_cent_z[ref_idx],
                            plane_has_winding[ref_idx] != 0, plane_winding_ccw_arr[ref_idx],
                            &outX, &outZ);
        oe->has_outward_xz = true;
        oe->outward_xz[0] = (float)outX;
        oe->outward_xz[1] = (float)outZ;

        // Inner loop over all other-plane single-plane roof edges.
        size_t cov_n = 0;
        for (size_t fi = 0; fi < n_edges; fi++) {
            if (fi == ei) continue;
            if (!eg_ok[fi]) continue;
            if (eg[fi].primary_plane == ref_pid) continue;     // same plane skip
            if (eg[fi].plane_count >= 2) continue;             // skip multi-plane candidates
            uint32_t fpi = id_map_lookup(&plane_map, eg[fi].primary_plane);
            if (fpi == UINT32_MAX) continue;
            if (plane_is_wall(&planes[fpi], WALL_TILT_MIN)) continue;

            double cos3D = fabs(eg[ei].dx * eg[fi].dx + eg[ei].dy * eg[fi].dy + eg[ei].dz * eg[fi].dz);
            double cosXZ = fabs(eg[ei].dxz_x * eg[fi].dxz_x + eg[ei].dxz_z * eg[fi].dxz_z);

            double toFx = eg[fi].mx - eg[ei].mx;
            double toFz = eg[fi].mz - eg[ei].mz;
            if (toFx * outX + toFz * outZ < -OUTWARD_EPS) continue;

            uint8_t match_type = 0;
            if (cos3D >= PERIM_COLLINEAR_COS) {
                double d3dx = eg[fi].mx - eg[ei].mx;
                double d3dy = eg[fi].my - eg[ei].my;
                double d3dz = eg[fi].mz - eg[ei].mz;
                double dist3DSq = d3dx*d3dx + d3dy*d3dy + d3dz*d3dz;
                if (dist3DSq < PERIM_DIST_3D_SQ) match_type = V7_BCLASS_TAG_SHARED;
            }
            if (match_type == 0 && cosXZ >= PERIM_COLLINEAR_COS) {
                double dxzDx = eg[fi].mx - eg[ei].mx;
                double dxzDz = eg[fi].mz - eg[ei].mz;
                double distXZSq = dxzDx*dxzDx + dxzDz*dxzDz;
                if (distXZSq < PERIM_DIST_XZ_SQ) {
                    match_type = (eg[ei].my > eg[fi].my)
                        ? V7_BCLASS_TAG_OVERHANG_TOP
                        : V7_BCLASS_TAG_OVERHANG_BOTTOM;
                }
            }
            if (match_type == 0) continue;

            // Project F's endpoints onto E.
            double v1x = eg[fi].p1[0] - eg[ei].p1[0];
            double v1y = eg[fi].p1[1] - eg[ei].p1[1];
            double v1z = eg[fi].p1[2] - eg[ei].p1[2];
            double v2x = eg[fi].p2[0] - eg[ei].p1[0];
            double v2y = eg[fi].p2[1] - eg[ei].p1[1];
            double v2z = eg[fi].p2[2] - eg[ei].p1[2];
            double tF1 = (v1x * eg[ei].dx + v1y * eg[ei].dy + v1z * eg[ei].dz) / eg[ei].len3d;
            double tF2 = (v2x * eg[ei].dx + v2y * eg[ei].dy + v2z * eg[ei].dz) / eg[ei].len3d;
            double tLo = tF1 < tF2 ? tF1 : tF2;
            double tHi = tF1 > tF2 ? tF1 : tF2;
            double t0 = tLo > 0.0 ? tLo : 0.0;
            double t1 = tHi < 1.0 ? tHi : 1.0;
            if ((t1 - t0) * eg[ei].len3d < params->perim_min_overlap_m) continue;

            if (cov_n == cov_cap) {
                size_t new_cap = cov_cap * 2;
                cov_candidate_t* nb = (cov_candidate_t*)safe_realloc_array(cov, new_cap, sizeof(*nb));
                if (!nb) { free(cov); free(adj); free(eg); free(eg_ok); free(plane_has_winding);
                          free(plane_winding_ccw_arr); free(plane_has_centroid); free(plane_cent_x);
                          free(plane_cent_z); free(plane_to_path); goto cleanup; }
                cov = nb; cov_cap = new_cap;
            }
            cov[cov_n].t0 = t0;
            cov[cov_n].t1 = t1;
            cov[cov_n].type = match_type;
            cov[cov_n].other_plane_id = eg[fi].primary_plane;
            cov[cov_n].other_edge_id  = edges[fi].id;
            cov_n++;
        }

        // Resolve tag from coverages.
        if (cov_n == 0) {
            oe->tag = V7_BCLASS_TAG_PERIMETER;
            oe->covered_fraction = 0.0f;
            out->stats.perimeter++;
            continue;
        }

        // Sort by t0, compute union covered length, decide tag.
        qsort(cov, cov_n, sizeof(*cov), cmp_cov_by_t0);
        double covered_len = 0, merged_end = 0;
        for (size_t k = 0; k < cov_n; k++) {
            double s = cov[k].t0 > merged_end ? cov[k].t0 : merged_end;
            if (cov[k].t1 > s) {
                covered_len += cov[k].t1 - s;
                merged_end = cov[k].t1;
            }
        }
        // covered_fraction in v2 is just covered_len (already normalized to [0,1] in t-space).
        double covered_fraction = covered_len;

        bool has_shared = false, has_ov_top = false;
        for (size_t k = 0; k < cov_n; k++) {
            if (cov[k].type == V7_BCLASS_TAG_SHARED) has_shared = true;
            else if (cov[k].type == V7_BCLASS_TAG_OVERHANG_TOP) has_ov_top = true;
        }

        // Materialise coverages onto the output edge.
        v7_bclass_coverage_t* co = (v7_bclass_coverage_t*)safe_malloc_array(cov_n, sizeof(*co));
        if (!co) { free(cov); free(adj); free(eg); free(eg_ok); free(plane_has_winding);
                  free(plane_winding_ccw_arr); free(plane_has_centroid); free(plane_cent_x);
                  free(plane_cent_z); free(plane_to_path); goto cleanup; }
        for (size_t k = 0; k < cov_n; k++) {
            co[k].t0 = (float)cov[k].t0;
            co[k].t1 = (float)cov[k].t1;
            co[k].type = cov[k].type;
            co[k].other_plane_id = cov[k].other_plane_id;
            co[k].other_edge_id  = cov[k].other_edge_id;
        }
        oe->coverages = co;
        oe->n_coverages = (uint32_t)cov_n;
        oe->covered_fraction = (float)covered_fraction;

        if (covered_fraction > 0.2) {
            // Dominant tag (shared > overhang-top > overhang-bottom).
            if (has_shared)      { oe->tag = V7_BCLASS_TAG_SHARED;          out->stats.shared++; }
            else if (has_ov_top) { oe->tag = V7_BCLASS_TAG_OVERHANG_TOP;    out->stats.overhang_top++; }
            else                  { oe->tag = V7_BCLASS_TAG_OVERHANG_BOTTOM; out->stats.overhang_bottom++; }
        } else {
            oe->tag = V7_BCLASS_TAG_PERIMETER;
            out->stats.perimeter++;
        }

        // Eave-shared adjacencies — for 'shared' coverages only.
        for (size_t k = 0; k < cov_n; k++) {
            if (cov[k].type != V7_BCLASS_TAG_SHARED) continue;
            if (adj_n == adj_cap) {
                size_t new_cap = adj_cap * 2;
                v7_bclass_new_adjacency_t* nb = (v7_bclass_new_adjacency_t*)safe_realloc_array(adj, new_cap, sizeof(*nb));
                if (!nb) { free(cov); free(adj); free(eg); free(eg_ok); free(plane_has_winding);
                          free(plane_winding_ccw_arr); free(plane_has_centroid); free(plane_cent_x);
                          free(plane_cent_z); free(plane_to_path); goto cleanup; }
                adj = nb; adj_cap = new_cap;
            }
            adj[adj_n].plane_a   = eg[ei].primary_plane;
            adj[adj_n].plane_b   = cov[k].other_plane_id;
            adj[adj_n].type      = 1;  // eave-shared
            adj[adj_n].edge_id_a = edges[ei].id;
            adj[adj_n].edge_id_b = cov[k].other_edge_id;
            adj_n++;
        }
    }

    // ── Phase E: dedup adjacencies against existing ridges ───────
    // Existing ridges: keep an unordered_set-like growable array of
    // packed (lo,hi) plane-pair keys. Linear scan is fine — typical
    // ridge count is tens.
    uint64_t* keys = NULL;
    size_t keys_n = 0, keys_cap = 0;
    if (n_ridges + adj_n > 0) {
        keys_cap = n_ridges + adj_n + 4;
        keys = (uint64_t*)safe_malloc_array(keys_cap, sizeof(uint64_t));
        if (!keys) { free(cov); free(adj); free(eg); free(eg_ok); free(plane_has_winding);
                     free(plane_winding_ccw_arr); free(plane_has_centroid); free(plane_cent_x);
                     free(plane_cent_z); free(plane_to_path); goto cleanup; }
        for (size_t r = 0; r < n_ridges; r++) {
            uint32_t lo = ridges[r].plane_a < ridges[r].plane_b ? ridges[r].plane_a : ridges[r].plane_b;
            uint32_t hi = ridges[r].plane_a > ridges[r].plane_b ? ridges[r].plane_a : ridges[r].plane_b;
            keys[keys_n++] = ((uint64_t)hi << 32) | (uint64_t)lo;
        }
    }

    size_t deduped_n = 0;
    v7_bclass_new_adjacency_t* deduped = NULL;
    if (adj_n > 0) {
        deduped = (v7_bclass_new_adjacency_t*)safe_malloc_array(adj_n, sizeof(*deduped));
        if (!deduped) { free(keys); free(cov); free(adj); free(eg); free(eg_ok); free(plane_has_winding);
                        free(plane_winding_ccw_arr); free(plane_has_centroid); free(plane_cent_x);
                        free(plane_cent_z); free(plane_to_path); goto cleanup; }
        for (size_t a = 0; a < adj_n; a++) {
            uint32_t lo = adj[a].plane_a < adj[a].plane_b ? adj[a].plane_a : adj[a].plane_b;
            uint32_t hi = adj[a].plane_a > adj[a].plane_b ? adj[a].plane_a : adj[a].plane_b;
            uint64_t k = ((uint64_t)hi << 32) | (uint64_t)lo;
            bool seen = false;
            for (size_t s = 0; s < keys_n; s++) if (keys[s] == k) { seen = true; break; }
            if (seen) continue;
            keys[keys_n++] = k;
            deduped[deduped_n++] = adj[a];
        }
    }
    free(keys);
    free(adj);
    out->new_adjacencies = deduped;
    out->n_new_adjacencies = deduped_n;
    out->stats.new_adjacencies_after_dedup = (uint32_t)deduped_n;

    // ── Phase F: level-transition detection ─────────────────────
    // Group perimeter-tagged roof-wall edges by their wall planeId; if
    // a wall has 2+ different roof planes in its group, retag those
    // perimeter edges as level-transition.
    //
    // Storage: per-wall-plane growable lists. Bounded by n_edges; use
    // a per-wall-plane head/next chain to avoid quadratic scans.
    uint32_t* wall_head = (uint32_t*)safe_malloc_array(n_planes ? n_planes : 1, sizeof(uint32_t));
    uint32_t* wall_next = (uint32_t*)safe_malloc_array(n_edges  ? n_edges  : 1, sizeof(uint32_t));
    uint32_t* wall_roof = (uint32_t*)safe_malloc_array(n_edges  ? n_edges  : 1, sizeof(uint32_t));
    if (!wall_head || !wall_next || !wall_roof) {
        free(wall_head); free(wall_next); free(wall_roof);
        free(cov); free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
        free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup;
    }
    for (size_t i = 0; i < n_planes; i++) wall_head[i] = UINT32_MAX;

    for (size_t ei = 0; ei < n_edges; ei++) {
        if (out->edges[ei].tag != V7_BCLASS_TAG_PERIMETER) continue;
        const v7_bclass_input_edge_t* E = &edges[ei];
        uint32_t first_wall = UINT32_MAX, first_roof = UINT32_MAX;
        bool has_wall = false, has_roof = false;
        for (uint32_t k = 0; k < E->n_plane_ids; k++) {
            uint32_t pi = id_map_lookup(&plane_map, E->plane_ids[k]);
            if (pi == UINT32_MAX) continue;
            if (plane_is_wall(&planes[pi], WALL_TILT_MIN)) {
                has_wall = true;
                if (first_wall == UINT32_MAX) first_wall = (uint32_t)pi;
            } else {
                has_roof = true;
                if (first_roof == UINT32_MAX) first_roof = E->plane_ids[k];
            }
        }
        if (!has_wall || !has_roof) continue;
        // v2 records the edge into EVERY wall plane in the edge's
        // planeIds. Mirror that — re-walk to enroll all wall pi indices.
        for (uint32_t k = 0; k < E->n_plane_ids; k++) {
            uint32_t pi = id_map_lookup(&plane_map, E->plane_ids[k]);
            if (pi == UINT32_MAX) continue;
            if (!plane_is_wall(&planes[pi], WALL_TILT_MIN)) continue;
            wall_next[ei] = wall_head[pi];  // intrusive single-list head
            wall_head[pi] = (uint32_t)ei;
            wall_roof[ei] = first_roof;
        }
    }

    for (size_t pi = 0; pi < n_planes; pi++) {
        uint32_t h = wall_head[pi];
        if (h == UINT32_MAX) continue;
        // Count unique roof planes in this wall's group.
        uint32_t unique_roofs[16]; uint32_t unique_n = 0;
        bool overflow = false;
        for (uint32_t ei = h; ei != UINT32_MAX; ei = wall_next[ei]) {
            uint32_t rp = wall_roof[ei];
            bool seen = false;
            for (uint32_t u = 0; u < unique_n; u++) if (unique_roofs[u] == rp) { seen = true; break; }
            if (!seen) {
                if (unique_n < 16) unique_roofs[unique_n++] = rp;
                else { overflow = true; break; }
            }
        }
        if (!overflow && unique_n < 2) continue;
        // Retag all edges in this group.
        for (uint32_t ei = h; ei != UINT32_MAX; ei = wall_next[ei]) {
            if (out->edges[ei].tag != V7_BCLASS_TAG_PERIMETER) continue;
            out->edges[ei].tag = V7_BCLASS_TAG_LEVEL_TRANSITION;
            out->stats.perimeter--;
            out->stats.level_transition++;
        }
    }
    free(wall_head); free(wall_next); free(wall_roof);

    // ── Phase G: PIP overhang fallback ──────────────────────────
    // Build xz polygons for non-wall, non-rejected planes that have a
    // polygon path. Then for each remaining 'perimeter' edge, walk
    // along it, probe outward, count XZ-containment hits in other
    // planes' polygons, decide overhang-top vs overhang-bottom.

    xz_polygon_t* xz = (xz_polygon_t*)safe_calloc_array(n_planes ? n_planes : 1, sizeof(xz_polygon_t));
    size_t xz_n = 0;
    if (!xz) { free(cov); free(eg); free(eg_ok); free(plane_has_winding); free(plane_winding_ccw_arr);
               free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z); free(plane_to_path); goto cleanup; }

    for (size_t pi = 0; pi < n_planes; pi++) {
        if (planes[pi].rejected) continue;
        if (plane_is_wall(&planes[pi], WALL_TILT_MIN)) continue;
        uint32_t path_idx = plane_to_path[pi];
        if (path_idx == UINT32_MAX) continue;
        const v7_bclass_input_polygon_path_t* P = &polygon_paths[path_idx];
        if (P->n_corners < 3) continue;
        // Collect XZ vertices that have valid corner lookups.
        double* xs = (double*)safe_malloc_array(P->n_corners, sizeof(double));
        double* zs = (double*)safe_malloc_array(P->n_corners, sizeof(double));
        if (!xs || !zs) { free(xs); free(zs); continue; }
        uint32_t nv = 0;
        for (uint32_t k = 0; k < P->n_corners; k++) {
            uint32_t ci = id_map_lookup(&corner_map, P->corner_ids[k]);
            if (ci == UINT32_MAX) continue;
            xs[nv] = corners[ci].point[0];
            zs[nv] = corners[ci].point[2];
            nv++;
        }
        if (nv < 3) { free(xs); free(zs); continue; }
        xz[xz_n].plane_id = planes[pi].id;
        xz[xz_n].xs = xs; xz[xz_n].zs = zs; xz[xz_n].n = nv;
        xz[xz_n].nx = planes[pi].normal[0];
        xz[xz_n].ny = planes[pi].normal[1];
        xz[xz_n].nz = planes[pi].normal[2];
        xz[xz_n].d  = planes[pi].d;
        xz[xz_n].eq_ok = (fabs(planes[pi].normal[1]) > 1e-6);
        xz_n++;
    }

    const double PIP_STEP = params->pip_step_len_m;
    const double PIP_OFFSET = params->pip_probe_offset_m;
    const double PIP_COV = params->pip_coverage_threshold;
    for (size_t ei = 0; ei < n_edges; ei++) {
        if (!eg_ok[ei]) continue;
        if (out->edges[ei].tag != V7_BCLASS_TAG_PERIMETER) continue;
        const v7_bclass_input_edge_t* E = &edges[ei];
        // Pick reference roof plane (skip walls).
        uint32_t ref_pid = UINT32_MAX;
        for (uint32_t k = 0; k < E->n_plane_ids; k++) {
            uint32_t pi = id_map_lookup(&plane_map, E->plane_ids[k]);
            if (pi == UINT32_MAX) continue;
            if (!plane_is_wall(&planes[pi], WALL_TILT_MIN)) { ref_pid = E->plane_ids[k]; break; }
        }
        if (ref_pid == UINT32_MAX) continue;
        uint32_t ref_idx = id_map_lookup(&plane_map, ref_pid);
        if (ref_idx == UINT32_MAX) continue;

        double outX, outZ;
        compute_outward_xz(&eg[ei],
                            plane_has_centroid[ref_idx], plane_cent_x[ref_idx], plane_cent_z[ref_idx],
                            plane_has_winding[ref_idx] != 0, plane_winding_ccw_arr[ref_idx],
                            &outX, &outZ);
        double oLen = sqrt(outX * outX + outZ * outZ);
        if (oLen < 1e-12) oLen = 1.0;
        double oNx = outX / oLen, oNz = outZ / oLen;
        if (!out->edges[ei].has_outward_xz) {
            out->edges[ei].has_outward_xz = true;
            out->edges[ei].outward_xz[0] = (float)oNx;
            out->edges[ei].outward_xz[1] = (float)oNz;
        }

        uint32_t n_steps = (uint32_t)floor(eg[ei].len3d / PIP_STEP);
        if (n_steps < 2) n_steps = 2;
        uint32_t covered = 0, total = 0;
        uint32_t top_votes = 0, bottom_votes = 0;
        uint32_t best_other_pid = UINT32_MAX;

        for (uint32_t s = 0; s <= n_steps; s++) {
            double t = (double)s / (double)n_steps;
            double sx = eg[ei].p1[0] + t * (eg[ei].p2[0] - eg[ei].p1[0]);
            double sy = eg[ei].p1[1] + t * (eg[ei].p2[1] - eg[ei].p1[1]);
            double sz = eg[ei].p1[2] + t * (eg[ei].p2[2] - eg[ei].p1[2]);
            double probeX = sx + oNx * PIP_OFFSET;
            double probeZ = sz + oNz * PIP_OFFSET;
            total++;

            double best_y = -INFINITY;
            uint32_t best_pid = UINT32_MAX;
            for (size_t xi = 0; xi < xz_n; xi++) {
                if (xz[xi].plane_id == ref_pid) continue;
                if (!point_in_poly_xz(probeX, probeZ, xz[xi].xs, xz[xi].zs, xz[xi].n)) continue;
                if (!xz[xi].eq_ok) continue;
                double yAt = -(xz[xi].nx * probeX + xz[xi].nz * probeZ + xz[xi].d) / xz[xi].ny;
                if (yAt > best_y) { best_y = yAt; best_pid = xz[xi].plane_id; }
            }
            if (best_pid == UINT32_MAX) continue;
            covered++;
            best_other_pid = best_pid;
            if (sy > best_y) bottom_votes++;
            else             top_votes++;
        }
        if (total == 0) continue;
        double coverage = (double)covered / (double)total;
        if (coverage < PIP_COV) continue;

        if (bottom_votes > top_votes) {
            out->edges[ei].tag = V7_BCLASS_TAG_OVERHANG_TOP;
            out->stats.overhang_top_pip++;
            out->stats.perimeter--;
            out->stats.overhang_top++;
        } else {
            out->edges[ei].tag = V7_BCLASS_TAG_OVERHANG_BOTTOM;
            out->stats.overhang_bottom_pip++;
            out->stats.perimeter--;
            out->stats.overhang_bottom++;
        }
        out->edges[ei].has_pip_overhang = true;
        out->edges[ei].pip_coverage = (float)coverage;
        out->edges[ei].pip_other_plane_id = best_other_pid;
    }

    for (size_t xi = 0; xi < xz_n; xi++) xz_polygon_free(&xz[xi]);
    free(xz);
    free(cov);
    free(eg);
    free(eg_ok);
    free(plane_has_winding); free(plane_winding_ccw_arr);
    free(plane_has_centroid); free(plane_cent_x); free(plane_cent_z);
    free(plane_to_path);

    rc = 0;
cleanup:
    id_map_free(&plane_map);
    id_map_free(&corner_map);
    id_map_free(&ridge_map);
    if (rc != 0) v7_bclass_outputs_free(out);
    return rc;
}
