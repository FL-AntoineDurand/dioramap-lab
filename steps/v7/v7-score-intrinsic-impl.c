// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-score-intrinsic — see header.

#include "v7-score-intrinsic-impl.h"

#include <math.h>
#include <string.h>

// ── Helpers ─────────────────────────────────────────────────────────

// Signed point-to-plane distance: n·p + d (with |n| = 1, so already metric).
static inline double signed_distance(const v7_score_plane_t* pl,
                                      const double p[3]) {
    return pl->normal[0]*p[0] + pl->normal[1]*p[1] + pl->normal[2]*p[2] + pl->d;
}

static inline double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

// Init every metric to V7_SCORE_NA so the caller sees "not computed"
// rather than stale zeros from a partial fill. Sentinel is -1 (not NaN)
// to satisfy v3's `-warn-special-float nan` audit discipline; visualiser
// + tests check `v < 0` to detect n/a. n_planes/n_points stays 0 (uint).
static void result_init(v7_score_intrinsic_result_t* r) {
    memset(r, 0, sizeof(*r));
    r->version = V7_SCORE_INTRINSIC_VERSION;
    r->completeness        = V7_SCORE_NA;
    r->residual_rms        = V7_SCORE_NA;
    r->residual_score      = V7_SCORE_NA;
    r->footprint_coverage  = V7_SCORE_NA;
    r->footprint_overshoot = V7_SCORE_NA;
    r->plane_overlap       = V7_SCORE_NA;
    r->composite           = V7_SCORE_NA;
}

// ── Composite (adaptive — only blend metrics that are finite). ──────
//
// Mirrors v2 compositeScoreV3's null-drop + weight-redistribution
// (lib/metrics/composite.js:115), restricted to the intrinsic set we
// compute here. As more metrics land (rasterisation), add them here +
// bump V7_SCORE_INTRINSIC_VERSION.
//
// Weights are the "always-available" rows from v2 composite V3, scaled
// to sum to 100 over whatever's available:
//     residual_score        12
//     completeness           8
//     footprint_coverage     4   (when implemented)
//     penaltyAvg             6   (mean of overshoot + overlap; when impl)
//
// Edge cases:
//   - all finite metrics 0 → composite = 0
//   - no finite metrics    → composite = NaN (caller should ignore)
static double compute_composite(const v7_score_intrinsic_result_t* r) {
    double weights[4] = {12, 8, 4, 6};
    double values[4]  = {r->residual_score, r->completeness,
                         r->footprint_coverage,
                         /* penalty avg below */ V7_SCORE_NA};
    // Both rasterisation penalties present → use the mean; one present
    // → use it alone; neither present → leave as V7_SCORE_NA (dropped).
    if (r->footprint_overshoot >= 0.0 && r->plane_overlap >= 0.0) {
        values[3] = 0.5 * (r->footprint_overshoot + r->plane_overlap);
    } else if (r->footprint_overshoot >= 0.0) {
        values[3] = r->footprint_overshoot;
    } else if (r->plane_overlap >= 0.0) {
        values[3] = r->plane_overlap;
    }

    double tw = 0.0, sum = 0.0;
    for (int i = 0; i < 4; i++) {
        if (values[i] >= 0.0) {
            tw  += weights[i];
            sum += weights[i] * clamp01(values[i]);
        }
    }
    if (tw <= 0.0) return V7_SCORE_NA;
    return (sum / tw) * 100.0;
}

// ── Public entry ───────────────────────────────────────────────────

int v7_score_intrinsic_compute(const double* positions,
                                const int16_t* owner,
                                size_t n_points_total,
                                const v7_score_plane_t* planes,
                                size_t n_planes,
                                v7_score_intrinsic_result_t* out) {
    if (!out) return -1;
    result_init(out);

    if (!positions || !owner) return -1;
    if (n_planes > 0 && !planes) return -1;
    if (n_planes > (size_t)INT16_MAX) return -1;  // owner is int16

    out->n_planes        = (uint32_t)n_planes;
    out->n_points_total  = (uint32_t)n_points_total;

    // ── Completeness + residual: one sweep over points ──
    size_t n_assigned = 0;
    double sum_sq = 0.0;   // sum of distance²
    double sum_w  = 0.0;   // count (= weight, since we don't yet weight)
    int    any_distance_blew_up = 0;

    for (size_t i = 0; i < n_points_total; i++) {
        int16_t pid = owner[i];
        if (pid < 0) continue;                  // unassigned
        if ((size_t)pid >= n_planes) continue;  // defensive — out-of-range
        n_assigned++;

        const v7_score_plane_t* pl = &planes[pid];
        double sd = signed_distance(pl, positions + 3 * i);
        if (!isfinite(sd)) { any_distance_blew_up = 1; continue; }
        sum_sq += sd * sd;
        sum_w  += 1.0;
    }
    out->n_points_assigned = (uint32_t)n_assigned;

    out->completeness = (n_points_total > 0)
        ? clamp01((double)n_assigned / (double)n_points_total)
        : 0.0;

    if (sum_w > 0.0 && !any_distance_blew_up) {
        out->residual_rms   = sqrt(sum_sq / sum_w);
        // residual_score: 1 at RMS=0, 0 at RMS≥V7_SCORE_RMS_REFERENCE
        // (matches v2 residual.js:res scoring).
        double s = 1.0 - out->residual_rms / V7_SCORE_RMS_REFERENCE;
        out->residual_score = clamp01(s);
    } else if (n_assigned == 0) {
        // No assigned points → residual is undefined. In the "no model"
        // case (no planes at all) we treat residual_score as 0 (worst)
        // so a building with zero detected planes ranks at the bottom
        // rather than being silently dropped by the composite
        // redistribution; otherwise leave it as V7_SCORE_NA.
        out->residual_rms   = V7_SCORE_NA;
        out->residual_score = (n_planes == 0) ? 0.0 : V7_SCORE_NA;
    }
    // else: leave V7_SCORE_NA (a distance blew up — surface the anomaly)

    // ── Rasterisation metrics: NOT YET IMPLEMENTED — see TODO ──────
    //
    // TODO(v2-of-step): footprint_coverage / footprint_overshoot /
    // plane_overlap via XZ rasterisation of plane hulls vs footprint.
    // Needs: per-plane hull polygon (or inlier-bbox → convex hull) and
    // the per-building footprint. Bench-v2 does this in
    // lib/metrics/coverage.js + geometry-quality.js; cell size ≈ 0.1 m
    // is the v2 default. Bump V7_SCORE_INTRINSIC_VERSION when added.

    out->composite = compute_composite(out);
    return 0;
}

// ── Pack/unpack ────────────────────────────────────────────────────
//
// memcpy-based for portability — never reinterpret_cast across alignment
// boundaries. The wire layout is documented in the header.

#define WR_U32(p, off, v) do { uint32_t _x = (v); memcpy((p)+(off), &_x, 4); } while (0)
#define WR_F64(p, off, v) do { double   _x = (v); memcpy((p)+(off), &_x, 8); } while (0)
#define RD_U32(p, off, v) memcpy(&(v), (p)+(off), 4)
#define RD_F64(p, off, v) memcpy(&(v), (p)+(off), 8)

size_t v7_score_intrinsic_pack(const v7_score_intrinsic_result_t* r,
                                uint8_t* out_buf) {
    WR_U32(out_buf,  0, r->version);
    WR_U32(out_buf,  4, r->n_planes);
    WR_U32(out_buf,  8, r->n_points_total);
    WR_U32(out_buf, 12, r->n_points_assigned);
    WR_F64(out_buf, 16, r->completeness);
    WR_F64(out_buf, 24, r->residual_rms);
    WR_F64(out_buf, 32, r->residual_score);
    WR_F64(out_buf, 40, r->footprint_coverage);
    WR_F64(out_buf, 48, r->footprint_overshoot);
    WR_F64(out_buf, 56, r->plane_overlap);
    WR_F64(out_buf, 64, r->composite);
    return V7_SCORE_INTRINSIC_RECORD_BYTES;  // 72
}

int v7_score_intrinsic_unpack(const uint8_t* buf, size_t len,
                               v7_score_intrinsic_result_t* out) {
    if (!buf || !out || len < V7_SCORE_INTRINSIC_RECORD_BYTES) return -1;
    RD_U32(buf,  0, out->version);
    RD_U32(buf,  4, out->n_planes);
    RD_U32(buf,  8, out->n_points_total);
    RD_U32(buf, 12, out->n_points_assigned);
    RD_F64(buf, 16, out->completeness);
    RD_F64(buf, 24, out->residual_rms);
    RD_F64(buf, 32, out->residual_score);
    RD_F64(buf, 40, out->footprint_coverage);
    RD_F64(buf, 48, out->footprint_overshoot);
    RD_F64(buf, 56, out->plane_overlap);
    RD_F64(buf, 64, out->composite);
    return 0;
}
