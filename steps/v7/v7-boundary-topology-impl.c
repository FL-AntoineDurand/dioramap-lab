// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-topology — pure compute implementation. See impl.h.
// Direct port of bench-v2 lib/pipeline/steps/boundary-edges/ridges.js
// + boundary-signals.js Phase A + class-filter.

#include "v7-boundary-topology-impl.h"
#include "v3/safe_alloc.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Params defaults ────────────────────────────────────────────────

void v7_topo_params_default(v7_topo_params_t* p) {
    p->adjacency_radius       = 0.5;
    p->min_contact_pts        = 3;
    p->class_filter_threshold = 0.50;
    p->min_plane_pts          = 6;
    p->spacing                = 0.316; // 1/sqrt(10) — v2 fallback when
                                       //   lidar-density artifact is missing.
                                       //   Prod step exec overrides with
                                       //   1/sqrt(per-bldg-density); bench
                                       //   harnesses keep this default.
    p->up_axis                = 1;     // y-up bench; v3 prod overrides to 2
}

// ── Outputs lifecycle ──────────────────────────────────────────────

void v7_topo_outputs_init(v7_topo_outputs_t* out) {
    memset(out, 0, sizeof(*out));
}
void v7_topo_outputs_free(v7_topo_outputs_t* out) {
    if (!out) return;
    free(out->planes);
    free(out->ridges);
    free(out->ridge_lines);
    memset(out, 0, sizeof(*out));
}

// ── Helpers: plane basis, intersection, edge type ─────────────────

// Build an orthonormal basis (e1, e2) on a plane with unit normal n.
// Port of geometry.js#planeBasis.
static void plane_basis(const double n[3], double e1[3], double e2[3]) {
    double rx = 1, ry = 0, rz = 0;
    if (fabs(n[0]) > 0.9) { rx = 0; ry = 1; }
    double ux = ry * n[2] - rz * n[1];
    double uy = rz * n[0] - rx * n[2];
    double uz = rx * n[1] - ry * n[0];
    double ulen = sqrt(ux*ux + uy*uy + uz*uz);
    if (ulen < 1e-12) { e1[0] = 1; e1[1] = 0; e1[2] = 0;
                        e2[0] = 0; e2[1] = 1; e2[2] = 0; return; }
    ux /= ulen; uy /= ulen; uz /= ulen;
    double vx = n[1] * uz - n[2] * uy;
    double vy = n[2] * ux - n[0] * uz;
    double vz = n[0] * uy - n[1] * ux;
    e1[0] = ux; e1[1] = uy; e1[2] = uz;
    e2[0] = vx; e2[1] = vy; e2[2] = vz;
}

// 3×3 determinant. Take non-const ptr to placate -Wpedantic when callers
// pass non-const local arrays; we never write through the parameter.
static inline double det3(double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
         - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
         + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

// Solve for the line of intersection of two planes via Cramer's rule.
// Returns 0 on success (line_point + line_direction filled), -1 on
// parallel/degenerate. Port of postprocess/plane-intersection.js.
static int plane_intersection_line(const double n1[3], double d1,
                                   const double n2[3], double d2,
                                   double line_point[3],
                                   double line_direction[3]) {
    double dir[3] = {
        n1[1] * n2[2] - n1[2] * n2[1],
        n1[2] * n2[0] - n1[0] * n2[2],
        n1[0] * n2[1] - n1[1] * n2[0],
    };
    double dl = sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dl < 1e-12) return -1;
    line_direction[0] = dir[0] / dl;
    line_direction[1] = dir[1] / dl;
    line_direction[2] = dir[2] / dl;

    // Solve:
    //   n1·p = -d1
    //   n2·p = -d2
    //   dir·p = 0
    double A[3][3] = {
        { n1[0], n1[1], n1[2] },
        { n2[0], n2[1], n2[2] },
        { line_direction[0], line_direction[1], line_direction[2] },
    };
    double det = det3(A);
    if (fabs(det) < 1e-12) return -1;

    double bvec[3] = { -d1, -d2, 0.0 };
    for (int i = 0; i < 3; i++) {
        double M[3][3];
        memcpy(M, A, sizeof(M));
        for (int row = 0; row < 3; row++) M[row][i] = bvec[row];
        line_point[i] = det3(M) / det;
    }
    return 0;
}

// Edge-type classifier. Port of geometry.js#classifyEdgeType. The
// `up_axis` parameter swaps y/z for v3 z-up vs v2 y-up: every
// "upness" check is on that axis component (heightA/B, lineDir.up,
// nA.up, nB.up).
static uint8_t classify_edge_type(const double nA[3], double centA_up,
                                  const double nB[3], double centB_up,
                                  const double line_dir[3],
                                  int up_axis) {
    double dot = nA[0]*nB[0] + nA[1]*nB[1] + nA[2]*nB[2];
    if (dot >  1.0) dot =  1.0;
    if (dot < -1.0) dot = -1.0;
    // FP-trap audit #274 note: acos near ±1 has degraded precision
    // (derivative is infinite at the endpoints). Not a coin flip — just
    // a precision loss. The thresholds below (10°, 60°) are far enough
    // from 0/180° that the precision loss is < 1e-6 degrees here. The
    // classification IS threshold-sensitive at exactly 10° / 60°, but
    // that's an unavoidable property of any threshold-based classifier
    // and is consistent across IEEE-754 implementations (no sign coin
    // flip pattern). No action needed.
    double angle_deg = acos(dot) * 180.0 / M_PI;

    double up_idx = (up_axis == 2) ? 2 : 1;
    double line_up = fabs(line_dir[(int)up_idx]);
    int line_horiz = (line_up < 0.3) ? 1 : 0;

    // COPLANAR (type 4, formerly mislabeled "flashing"): the two planes' normals are
    // within COPLANAR_MAX_ANGLE_DEG, so the surface barely folds and the crease line
    // nA×nB is ill-conditioned (→0). Carries NO positive semantics downstream —
    // excluded from make-shared (I1) and from DOF coupling (the line is degenerate).
    // The 10° value is deliberate and equals the GRM coplanar-merge angle
    // (grow-refit-merge merge_angle_deg=10): a pair this near-coplanar was a merge
    // candidate; surviving as two planes means a height step / artifact, not a crease.
    const double COPLANAR_MAX_ANGLE_DEG = 10.0;
    if (angle_deg < COPLANAR_MAX_ANGLE_DEG) return 4;  // coplanar (near-coplanar seam)

    if (line_horiz) {
        double upA = nA[(int)up_idx], upB = nB[(int)up_idx];
        if (upA > 0.3 && upB > 0.3) {
            (void)centA_up; (void)centB_up;  // v2 returns 'ridge' regardless
            return 1;  // ridge
        }
        return 5;  // step
    }

    if (angle_deg > 60.0) {
        double avg_up = (nA[(int)up_idx] + nB[(int)up_idx]) / 2.0;
        if (avg_up > 0.3) return 2;  // hip
        return 3;  // valley
    }

    return 1;  // ridge
}

// ── Adjacency graph (Phase A.1) ───────────────────────────────────

typedef struct adj_pair {
    uint32_t a, b;          // plane group ids, a < b
    uint32_t count;
} adj_pair_t;

// Hash map (open-addressed) for pair → count. Key = (a<<32)|b.
typedef struct adj_map {
    uint64_t* keys;
    uint32_t* vals;
    size_t cap;
    size_t n;
} adj_map_t;

static void adj_map_init(adj_map_t* m, size_t cap) {
    m->cap = cap;
    m->keys = (uint64_t*)safe_calloc_array(cap, sizeof(uint64_t));
    m->vals = (uint32_t*)safe_calloc_array(cap, sizeof(uint32_t));
    m->n = 0;
}
static void adj_map_free(adj_map_t* m) {
    free(m->keys); free(m->vals);
    memset(m, 0, sizeof(*m));
}
static uint64_t adj_hash(uint64_t key) {
    // Splittable64.
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return key;
}
static void adj_map_inc(adj_map_t* m, uint32_t a, uint32_t b) {
    if (a == b) return;
    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;
    uint64_t key = ((uint64_t)lo << 32) | hi;
    uint64_t h = adj_hash(key);
    // Reserve sentinel 0 — never store key=0 (lo=hi=0 not possible since a!=b).
    for (size_t i = 0; i < m->cap; i++) {
        size_t slot = (h + i) % m->cap;
        if (m->keys[slot] == 0) {
            m->keys[slot] = key;
            m->vals[slot] = 1;
            m->n++;
            return;
        }
        if (m->keys[slot] == key) {
            m->vals[slot]++;
            return;
        }
    }
    // Full — should never happen with right cap. Drop silently.
}

// Phase A.1 — Build per-pair adjacency by kNN proximity on building points.
// Iterates each point with owner ≥ 0, walks its neighbours within
// adjacency_radius, counts cross-group contacts. Pairs with
// count < min_contact_pts are filtered out.
static int build_adjacency(const double* positions, size_t n_pts_local,
                           const int16_t* owner,
                           size_t n_seeds,
                           const kd_tree_t* tree,
                           double radius,
                           int min_contacts,
                           adj_pair_t** out_pairs,
                           size_t* out_n_pairs) {
    *out_pairs = NULL;
    *out_n_pairs = 0;
    if (n_seeds == 0) return 0;

    adj_map_t m;
    // Upper bound on pairs: K*(K-1)/2. Use 4× for hash slack.
    size_t cap = n_seeds * (n_seeds - 1) / 2 + 16;
    cap *= 4;
    if (cap < 64) cap = 64;
    adj_map_init(&m, cap);
    if (!m.keys || !m.vals) { adj_map_free(&m); return -1; }

    int knn_cap = 32;
    kd_neighbor_t* neighbours = (kd_neighbor_t*)safe_malloc_array(knn_cap, sizeof(kd_neighbor_t));
    if (!neighbours) { adj_map_free(&m); return -1; }

    for (size_t gi = 0; gi < n_pts_local; gi++) {
        int16_t mo = owner[gi];
        if (mo < 0 || (size_t)mo >= n_seeds) continue;
        double q[3] = {
            positions[gi * 3 + 0],
            positions[gi * 3 + 1],
            positions[gi * 3 + 2],
        };
        // Grow the buffer if needed.
        int cnt = 0;
        while (1) {
            if (kd_radius_search(tree, q, radius, neighbours, knn_cap, &cnt) != 0) {
                free(neighbours); adj_map_free(&m); return -1;
            }
            if (cnt <= knn_cap) break;
            knn_cap = cnt + 8;
            kd_neighbor_t* n2 = (kd_neighbor_t*)safe_realloc_array(neighbours, knn_cap, sizeof(kd_neighbor_t));
            if (!n2) { free(neighbours); adj_map_free(&m); return -1; }
            neighbours = n2;
        }
        for (int k = 0; k < cnt; k++) {
            int nbi = neighbours[k].idx;
            if ((size_t)nbi == gi) continue;
            int16_t no = owner[nbi];
            if (no < 0 || (size_t)no >= n_seeds || no == mo) continue;
            adj_map_inc(&m, (uint32_t)mo, (uint32_t)no);
        }
    }
    free(neighbours);

    // Extract pairs above min_contacts.
    adj_pair_t* pairs = (adj_pair_t*)safe_malloc_array(m.n, sizeof(adj_pair_t));
    if (!pairs && m.n > 0) { adj_map_free(&m); return -1; }
    size_t k = 0;
    for (size_t s = 0; s < m.cap; s++) {
        if (m.keys[s] == 0) continue;
        if ((int)m.vals[s] < min_contacts) continue;
        pairs[k].a = (uint32_t)(m.keys[s] >> 32);
        pairs[k].b = (uint32_t)(m.keys[s] & 0xFFFFFFFFu);
        pairs[k].count = m.vals[s];
        k++;
    }
    adj_map_free(&m);
    *out_pairs = pairs;
    *out_n_pairs = k;
    return 0;
}

// ── Ridge lines (Phase A.2) ───────────────────────────────────────

typedef struct group_pts {
    uint32_t* indices;   // local point indices belonging to this group
    size_t n;
} group_pts_t;

// Project a group's points onto a line, return min/max t along it.
static void project_group(const group_pts_t* g, const double* positions,
                          const double line_point[3], const double line_dir[3],
                          double* out_min_t, double* out_max_t) {
    double minT = INFINITY, maxT = -INFINITY;
    for (size_t i = 0; i < g->n; i++) {
        size_t gi = g->indices[i];
        double dx = positions[gi * 3 + 0] - line_point[0];
        double dy = positions[gi * 3 + 1] - line_point[1];
        double dz = positions[gi * 3 + 2] - line_point[2];
        double t = dx * line_dir[0] + dy * line_dir[1] + dz * line_dir[2];
        if (t < minT) minT = t;
        if (t > maxT) maxT = t;
    }
    *out_min_t = minT;
    *out_max_t = maxT;
}

// Sample which side of the ridge a group sits on (for side_sign).
static int sample_side(const group_pts_t* g, const double* positions,
                       const double line_point[3], const double line_dir[3],
                       const double side_vec[3], double near_ridge_dist) {
    double radSq = near_ridge_dist * near_ridge_dist;
    double sum = 0;
    int cnt = 0;
    for (size_t i = 0; i < g->n; i++) {
        size_t gi = g->indices[i];
        double dx = positions[gi * 3 + 0] - line_point[0];
        double dy = positions[gi * 3 + 1] - line_point[1];
        double dz = positions[gi * 3 + 2] - line_point[2];
        double t = dx * line_dir[0] + dy * line_dir[1] + dz * line_dir[2];
        double px = dx - t * line_dir[0];
        double py = dy - t * line_dir[1];
        double pz = dz - t * line_dir[2];
        if (px*px + py*py + pz*pz > radSq) continue;
        sum += side_vec[0] * dx + side_vec[1] * dy + side_vec[2] * dz;
        cnt++;
    }
    if (cnt == 0) return 0;
    double mean = sum / cnt;
    if (mean > 0) return 1;
    if (mean < 0) return -1;
    return 0;
}

// Build the interior-oriented side axis for ONE plane of a ridge: perpendicular
// to the ridge AND lying in this plane's surface (line_dir × normal), unit, then
// flipped to point from the ridge toward the plane's body. Returns false (axis
// undefined) when the cross product is degenerate (line_dir ∥ normal) or no
// near-ridge points yield a definite interior side. This replaces the old
// "one side_vec from plane A + two sampled signs" model: each plane now carries
// its own axis, so D.2b judges a plane-B corner in plane B's frame instead of
// plane A's (which collapsed on steep dihedrals). See journal
// 2026-06-11-d2b-side-vec-wrong-plane-frame.
static bool compute_side_axis(const double line_dir[3], const double normal[3],
                              const group_pts_t* g, const double* positions,
                              const double line_point[3], double near_ridge_dist,
                              double out[3]) {
    double sv[3] = {
        line_dir[1] * normal[2] - line_dir[2] * normal[1],
        line_dir[2] * normal[0] - line_dir[0] * normal[2],
        line_dir[0] * normal[1] - line_dir[1] * normal[0],
    };
    double len = sqrt(sv[0]*sv[0] + sv[1]*sv[1] + sv[2]*sv[2]);
    if (len <= 1e-12) { out[0] = out[1] = out[2] = 0.0; return false; }
    sv[0] /= len; sv[1] /= len; sv[2] /= len;
    int s = sample_side(g, positions, line_point, line_dir, sv, near_ridge_dist);
    if (s == 0) { out[0] = out[1] = out[2] = 0.0; return false; }  // no definite interior
    if (s < 0) { sv[0] = -sv[0]; sv[1] = -sv[1]; sv[2] = -sv[2]; }
    out[0] = sv[0]; out[1] = sv[1]; out[2] = sv[2];
    return true;
}

// ── Phase A: Compose ──────────────────────────────────────────────

int v7_topo_compute(const double* positions, size_t n_pts_local,
                    const int16_t* owner,
                    const uint8_t* classes_local,
                    const v7_topo_input_seed_t* seeds, size_t n_seeds,
                    const kd_tree_t* tree,
                    const v7_topo_params_t* params,
                    v7_topo_outputs_t* out) {
    v7_topo_outputs_init(out);

    // 1. Group points per seed plane.
    group_pts_t* groups = (group_pts_t*)safe_calloc_array(n_seeds, sizeof(group_pts_t));
    if (!groups && n_seeds > 0) return -1;

    // Two-pass: count then fill.
    for (size_t i = 0; i < n_pts_local; i++) {
        int16_t g = owner[i];
        if (g >= 0 && (size_t)g < n_seeds) groups[g].n++;
    }
    for (size_t g = 0; g < n_seeds; g++) {
        if (groups[g].n == 0) continue;
        groups[g].indices = (uint32_t*)malloc(groups[g].n * sizeof(uint32_t));
        if (!groups[g].indices) {
            for (size_t k = 0; k < g; k++) free(groups[k].indices);
            free(groups);
            return -1;
        }
        groups[g].n = 0;  // re-use as write cursor
    }
    for (size_t i = 0; i < n_pts_local; i++) {
        int16_t g = owner[i];
        if (g >= 0 && (size_t)g < n_seeds) {
            groups[g].indices[groups[g].n++] = (uint32_t)i;
        }
    }

    // 2. Adjacency graph (Phase A.1).
    adj_pair_t* pairs = NULL;
    size_t n_pairs = 0;
    if (build_adjacency(positions, n_pts_local, owner, n_seeds, tree,
                        params->adjacency_radius, params->min_contact_pts,
                        &pairs, &n_pairs) != 0) {
        goto fail;
    }

    // 3. Ridge lines (Phase A.2) — one record per adj pair that survives
    // overlap + sparse-contact filters.
    v7_topo_ridge_t* ridges = (v7_topo_ridge_t*)safe_malloc_array(n_pairs, sizeof(v7_topo_ridge_t));
    v7_topo_ridge_line_t* rlines = (v7_topo_ridge_line_t*)safe_malloc_array(n_pairs, sizeof(v7_topo_ridge_line_t));
    if ((!ridges || !rlines) && n_pairs > 0) {
        free(ridges); free(rlines); free(pairs);
        goto fail;
    }
    size_t n_kept = 0;
    int up_axis = (params->up_axis == 2) ? 2 : 1;
    double spacing = params->spacing > 0 ? params->spacing : 0.316;

    for (size_t pi = 0; pi < n_pairs; pi++) {
        uint32_t a = pairs[pi].a, b = pairs[pi].b;
        const group_pts_t* gA = &groups[a];
        const group_pts_t* gB = &groups[b];
        if (gA->n < 3 || gB->n < 3) continue;
        if (seeds[a].tombstoned || seeds[b].tombstoned) continue;

        double line_point[3], line_dir[3];
        if (plane_intersection_line(seeds[a].normal, seeds[a].d,
                                    seeds[b].normal, seeds[b].d,
                                    line_point, line_dir) != 0) continue;

        double minTA, maxTA, minTB, maxTB;
        project_group(gA, positions, line_point, line_dir, &minTA, &maxTA);
        project_group(gB, positions, line_point, line_dir, &minTB, &maxTB);

        double clip_min = minTA > minTB ? minTA : minTB;
        double clip_max = maxTA < maxTB ? maxTA : maxTB;
        double clip_len = clip_max - clip_min;
        if (clip_len <= 0) continue;

        double union_min = minTA < minTB ? minTA : minTB;
        double union_max = maxTA > maxTB ? maxTA : maxTB;
        double union_len = union_max - union_min;
        double overlap = (union_len > 0) ? (clip_len / union_len) : 0;
        if (overlap < 0.15) continue;

        double contacts_per_m = (double)pairs[pi].count / clip_len;
        double expected_cpm = sqrt(1.0 / (1.0 / sqrt(gA->n > 0 ? (double)gA->n : 1.0)));
        if (contacts_per_m < expected_cpm * 0.3 && clip_len > spacing * 3.0) continue;

        // Edge-type classify — use centroid up-axis component for height comparison.
        double centA_up = 0, centB_up = 0;
        for (size_t i = 0; i < gA->n; i++) centA_up += positions[gA->indices[i] * 3 + up_axis];
        for (size_t i = 0; i < gB->n; i++) centB_up += positions[gB->indices[i] * 3 + up_axis];
        centA_up /= (double)gA->n;
        centB_up /= (double)gB->n;
        uint8_t edge_type = classify_edge_type(seeds[a].normal, centA_up,
                                                seeds[b].normal, centB_up,
                                                line_dir, up_axis);

        // Trim ridge elevation for non-horizontal lines.
        if (fabs(line_dir[up_axis]) > 0.03) {
            double maxUpA = -INFINITY, maxUpB = -INFINITY, minUpA = INFINITY, minUpB = INFINITY;
            for (size_t i = 0; i < gA->n; i++) {
                double y = positions[gA->indices[i] * 3 + up_axis];
                if (y > maxUpA) maxUpA = y;
                if (y < minUpA) minUpA = y;
            }
            for (size_t i = 0; i < gB->n; i++) {
                double y = positions[gB->indices[i] * 3 + up_axis];
                if (y > maxUpB) maxUpB = y;
                if (y < minUpB) minUpB = y;
            }
            double maxY = maxUpA > maxUpB ? maxUpA : maxUpB;
            double minY = minUpA < minUpB ? minUpA : minUpB;
            double tHi = (maxY - line_point[up_axis]) / line_dir[up_axis];
            double tLo = (minY - line_point[up_axis]) / line_dir[up_axis];
            if (tHi < tLo) { double tmp = tHi; tHi = tLo; tLo = tmp; }
            if (maxTA > tHi) maxTA = tHi;
            if (maxTB > tHi) maxTB = tHi;
            if (minTA < tLo) minTA = tLo;
            if (minTB < tLo) minTB = tLo;
            // Recompute clip after trim.
            clip_min = minTA > minTB ? minTA : minTB;
            clip_max = maxTA < maxTB ? maxTA : maxTB;
            if (clip_max <= clip_min) continue;
        }

        // Per-plane interior-oriented side axes (each judged in ITS OWN plane's
        // surface — see compute_side_axis). Replaces the old single side_vec
        // built from plane A's normal, which D.2b then (mis)used for plane B's
        // corners too.
        double side_vec_a[3], side_vec_b[3];
        bool has_side_a = compute_side_axis(line_dir, seeds[a].normal, gA, positions,
                                            line_point, spacing * 3.0, side_vec_a);
        bool has_side_b = compute_side_axis(line_dir, seeds[b].normal, gB, positions,
                                            line_point, spacing * 3.0, side_vec_b);

        // Emit ridge record.
        v7_topo_ridge_t* R = &ridges[n_kept];
        R->id = (uint32_t)n_kept;
        R->plane_a = a;
        R->plane_b = b;
        R->type = edge_type;
        for (int k = 0; k < 3; k++) {
            R->from[k] = line_point[k] + clip_min * line_dir[k];
            R->to[k]   = line_point[k] + clip_max * line_dir[k];
        }
        R->length = clip_max - clip_min;
        R->contact_count = pairs[pi].count;
        R->overlap_ratio = (float)overlap;

        v7_topo_ridge_line_t* L = &rlines[n_kept];
        L->ridge_id = (uint32_t)n_kept;
        L->has_line = true;
        L->has_side_a = has_side_a;
        L->has_side_b = has_side_b;
        for (int k = 0; k < 3; k++) {
            L->line_point[k]     = line_point[k];
            L->line_direction[k] = line_dir[k];
        }
        L->t_min_a = (float)minTA;
        L->t_max_a = (float)maxTA;
        L->t_min_b = (float)minTB;
        L->t_max_b = (float)maxTB;
        for (int k = 0; k < 3; k++) {
            L->side_vec_a[k] = (float)side_vec_a[k];
            L->side_vec_b[k] = (float)side_vec_b[k];
        }

        n_kept++;
    }
    free(pairs);

    // 4. Class filter + ridge-neighbor rescue.
    // For each plane: compute classStats, candidate-reject if gvw > threshold,
    // then unreject if any non-rejected RIDGE neighbour exists.
    v7_topo_plane_t* planes = (v7_topo_plane_t*)safe_calloc_array(n_seeds, sizeof(v7_topo_plane_t));
    if (!planes && n_seeds > 0) {
        free(ridges); free(rlines);
        goto fail;
    }

    // Pass 1: compute class stats + candidate rejection.
    uint8_t* cand_rej = (uint8_t*)safe_calloc_array(n_seeds, sizeof(uint8_t));
    if (!cand_rej && n_seeds > 0) {
        free(planes); free(ridges); free(rlines);
        goto fail;
    }
    for (size_t g = 0; g < n_seeds; g++) {
        v7_topo_plane_t* P = &planes[g];
        P->id = (uint32_t)g;
        memcpy(P->normal, seeds[g].normal, sizeof(double) * 3);
        P->d = seeds[g].d;
        P->n_pts = (uint32_t)groups[g].n;

        if (seeds[g].tombstoned) {
            P->rejected = true;
            P->rejection_reason = 4;  // tombstoned
            continue;
        }
        if (groups[g].n < (size_t)params->min_plane_pts) {
            P->rejected = true;
            P->rejection_reason = 3;  // low-nPts
            continue;
        }

        // Centroid + classification stats.
        double cx = 0, cy = 0, cz = 0;
        uint32_t cnt[8] = {0};  // total, building, ground, lowVeg, medVeg, highVeg, water, other
        for (size_t i = 0; i < groups[g].n; i++) {
            uint32_t li = groups[g].indices[i];
            cx += positions[li * 3 + 0];
            cy += positions[li * 3 + 1];
            cz += positions[li * 3 + 2];
            if (classes_local) {
                uint8_t c = classes_local[li];
                cnt[0]++;
                if      (c == 6) cnt[1]++;
                else if (c == 2) cnt[2]++;
                else if (c == 3) cnt[3]++;
                else if (c == 4) cnt[4]++;
                else if (c == 5) cnt[5]++;
                else if (c == 9) cnt[6]++;
                else             cnt[7]++;
            }
        }
        P->centroid[0] = cx / groups[g].n;
        P->centroid[1] = cy / groups[g].n;
        P->centroid[2] = cz / groups[g].n;

        // Tilt = acos(|n_up|) — degrees from horizontal-up.
        double up_n = fabs(seeds[g].normal[up_axis]);
        if (up_n > 1.0) up_n = 1.0;
        P->tilt_deg = (uint16_t)(acos(up_n) * 180.0 / M_PI + 0.5);

        if (classes_local) {
            P->has_class_stats = true;
            P->cs_total    = cnt[0]; P->cs_building = cnt[1];
            P->cs_ground   = cnt[2]; P->cs_low_veg  = cnt[3];
            P->cs_med_veg  = cnt[4]; P->cs_high_veg = cnt[5];
            P->cs_water    = cnt[6]; P->cs_other    = cnt[7];
            if (cnt[0] > 0) {
                double gvw_frac = (double)(cnt[2] + cnt[3] + cnt[4] + cnt[5] + cnt[6]) / (double)cnt[0];
                if (gvw_frac > params->class_filter_threshold) {
                    cand_rej[g] = 1;
                }
            }
        }

        // Basis for surviving planes (computed even for candidate-rejected; will be
        // overridden to false if rejection sticks).
        plane_basis(seeds[g].normal, P->basis_e1, P->basis_e2);
        P->has_basis = true;
    }

    // Pass 2: ridge-neighbor adjacency.
    // For each plane, set of plane ids it's ridge-adjacent to.
    // n_kept ridges already represent the topology.
    uint8_t* unrejected = (uint8_t*)safe_calloc_array(n_seeds, sizeof(uint8_t));
    if (!unrejected && n_seeds > 0) {
        free(cand_rej); free(planes); free(ridges); free(rlines);
        goto fail;
    }
    if (classes_local) {
        // Initialize: rejected[g] = cand_rej[g]
        // Then for each rejected g: if any ridge-neighbour is NOT cand_rej, rescue.
        for (size_t g = 0; g < n_seeds; g++) unrejected[g] = !cand_rej[g];
        // Iterate ridges; if EITHER endpoint is rejected, check the OTHER's cand status.
        // (v2 logic: only rescues from a non-cand neighbour, doesn't propagate.)
        for (size_t r = 0; r < n_kept; r++) {
            uint32_t a = ridges[r].plane_a;
            uint32_t b = ridges[r].plane_b;
            if (cand_rej[a] && !cand_rej[b]) unrejected[a] = 1;
            if (cand_rej[b] && !cand_rej[a]) unrejected[b] = 1;
        }
        // Apply.
        for (size_t g = 0; g < n_seeds; g++) {
            v7_topo_plane_t* P = &planes[g];
            // Skip planes already rejected for tombstoned / low-nPts.
            if (P->rejection_reason == 3 || P->rejection_reason == 4) continue;
            if (cand_rej[g] && !unrejected[g]) {
                P->rejected = true;
                // 'class-filter' = 2 in spec §5.1; v2 uses 'gvw>50%' = 1 for this case.
                P->rejection_reason = 1;  // gvw>50%
            } else {
                P->rejected = false;
                P->rejection_reason = 0;
            }
        }
    }
    free(unrejected);
    free(cand_rej);

    // 5. Free group buffers.
    for (size_t g = 0; g < n_seeds; g++) free(groups[g].indices);
    free(groups);

    out->planes      = planes;
    out->n_planes    = n_seeds;
    out->ridges      = ridges;
    out->n_ridges    = n_kept;
    out->ridge_lines = rlines;
    return 0;

fail:
    for (size_t g = 0; g < n_seeds; g++) free(groups[g].indices);
    free(groups);
    v7_topo_outputs_free(out);
    return -1;
}
