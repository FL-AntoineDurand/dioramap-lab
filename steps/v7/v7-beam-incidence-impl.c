// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-beam-incidence — see header.

#include "v7-beam-incidence-impl.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

void v7_beam_params_default(v7_beam_params_t* p) {
    p->min_points_per_strip  = 30;
    p->min_band_elongation   = 3.0;
    p->mr_min_pairs          = 5;
    p->mr_max_angle_jitter   = 0.1;
    p->mr_min_pair_dist      = 0.05;
    p->up_axis               = 1;     // bench frame
}

// ─── Per-strip records (Map<psid, …> equivalent, first-seen order) ──

typedef struct strip {
    uint16_t psid;
    uint32_t* members;        // member point indices (in buildIdx-order)
    size_t n_members;
    size_t cap;
    // Phase 2a result
    int has_fit;              // 0 → strip is too small / degenerate
    double ct_h0;             // cross-track in (h0, h1) plane
    double ct_h1;
    // Phase 2b result
    int has_correction;
    double corr_h0;
    double corr_h1;
} strip_t;

static int strip_push(strip_t* s, uint32_t gi) {
    if (s->n_members == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        uint32_t* p = (uint32_t*)realloc(s->members, nc * sizeof(uint32_t));
        if (!p) return -1;
        s->members = p; s->cap = nc;
    }
    s->members[s->n_members++] = gi;
    return 0;
}

// Sort key (angle, member-index) → stable plain qsort. Avoids the
// non-portable qsort_r dance.
typedef struct angle_key { float angle; uint32_t gi; } angle_key_t;

static int angle_key_cmp(const void* a, const void* b) {
    const angle_key_t* x = (const angle_key_t*)a;
    const angle_key_t* y = (const angle_key_t*)b;
    if (x->angle < y->angle) return -1;
    if (x->angle > y->angle) return  1;
    if (x->gi < y->gi) return -1;
    if (x->gi > y->gi) return  1;
    return 0;
}

// ─── Helpers ──

static inline void horiz_axes(int up_axis, int* h0, int* h1) {
    if (up_axis == 1)      { *h0 = 0; *h1 = 2; }
    else if (up_axis == 2) { *h0 = 0; *h1 = 1; }
    else                   { *h0 = 1; *h1 = 2; }
}

// ─── Phase 2a: per-strip PCA on iso-angle bands → flight direction ──

static int strip_fit_flight_direction(strip_t* s,
                                       const double* positions,
                                       const float* scan_angles,
                                       int h0, int h1,
                                       double min_elongation) {
    if ((int32_t)s->n_members < 30) return 0;  // caller already gates,
                                                 // but defensive

    // Sort members by scan angle (ascending) — used for swath edges + median
    // sign check.
    angle_key_t* keys = (angle_key_t*)malloc(s->n_members * sizeof(angle_key_t));
    if (!keys) return -1;
    for (size_t i = 0; i < s->n_members; i++) {
        keys[i].gi = s->members[i];
        keys[i].angle = scan_angles[s->members[i]];
    }
    qsort(keys, s->n_members, sizeof(angle_key_t), angle_key_cmp);
    for (size_t i = 0; i < s->n_members; i++) s->members[i] = keys[i].gi;
    free(keys);

    const float min_angle = scan_angles[s->members[0]];
    const float max_angle = scan_angles[s->members[s->n_members - 1]];
    const double angle_span = (double)(max_angle - min_angle);

    const int n_bands_iso = 7;
    // Frama-C audit finding BEAM-1: cap n_members's contribution to fit
    // signed int. The 0.03 factor gives 7e7 at INT_MAX which is safe
    // through to ~2.4G member points, but the cast itself is UB for
    // larger inputs. Clamp at the cast boundary.
    double edge_count_d = (double)s->n_members * 0.03 + 0.5;
    if (edge_count_d > (double)INT_MAX) edge_count_d = (double)INT_MAX;
    const int edge_count = (int)edge_count_d;
    const int edge_count_clamped = edge_count > 20 ? edge_count : 20;
    const double half_width = angle_span / (n_bands_iso * 2);

    double flight_h0 = 0, flight_h1 = 0;
    int n_bands_used = 0;

    // Generic per-band PCA on the (h0, h1) horizontal plane.
    // `band_indices`: indices INTO s->members (NOT into positions).
    // We'll use a scratch buffer of N members, populated per band.
    uint32_t* band = (uint32_t*)malloc(s->n_members * sizeof(uint32_t));
    if (!band) return -1;

    // Bands 0..6: iso-angle bands. Band 7: low-edge swath. Band 8: high-edge swath.
    for (int b = 0; b < n_bands_iso + 2; b++) {
        size_t band_n = 0;
        if (b < n_bands_iso) {
            const double target = (double)min_angle
                + angle_span * (double)b / (double)(n_bands_iso - 1);
            // Linear scan (members aren't grouped by angle anymore — sorted
            // but that doesn't help for arbitrary windows, so just scan).
            // Cost: O(n_strip) per band, 9 bands → ≤ 10× n_strip.
            for (size_t i = 0; i < s->n_members; i++) {
                const uint32_t gi = s->members[i];
                const double a = (double)scan_angles[gi];
                if (a >= target - half_width && a <= target + half_width) {
                    band[band_n++] = gi;
                }
            }
            if (band_n < 10) continue;
        } else if (b == n_bands_iso) {
            // Low edge: first edge_count_clamped members in sort order.
            const size_t take = (size_t)edge_count_clamped;
            band_n = take < s->n_members ? take : s->n_members;
            memcpy(band, s->members, band_n * sizeof(uint32_t));
        } else {
            const size_t take = (size_t)edge_count_clamped;
            band_n = take < s->n_members ? take : s->n_members;
            const size_t start = s->n_members - band_n;
            memcpy(band, s->members + start, band_n * sizeof(uint32_t));
        }
        if (band_n < 10) continue;

        // 2×2 PCA on (h0, h1) projection.
        double mh0 = 0, mh1 = 0;
        for (size_t k = 0; k < band_n; k++) {
            const double* p = positions + (size_t)band[k] * 3;
            mh0 += p[h0]; mh1 += p[h1];
        }
        mh0 /= (double)band_n; mh1 /= (double)band_n;

        double cxx = 0, czz = 0, cxz = 0;
        for (size_t k = 0; k < band_n; k++) {
            const double* p = positions + (size_t)band[k] * 3;
            const double dh0 = p[h0] - mh0;
            const double dh1 = p[h1] - mh1;
            cxx += dh0 * dh0;
            czz += dh1 * dh1;
            cxz += dh0 * dh1;
        }

        const double tr  = cxx + czz;
        const double det = cxx * czz - cxz * cxz;
        const double disc = tr * tr / 4 - det;
        const double sq = sqrt(disc > 0 ? disc : 0);
        const double e1 = tr / 2 + sq;
        const double e2 = tr / 2 - sq;
        if (e2 < 1e-10) continue;
        if (e1 / e2 < min_elongation) continue;

        double evx, evz;
        if (cxz > 1e-10 || cxz < -1e-10) {
            evx = e1 - czz; evz = cxz;
        } else {
            evx = (cxx > czz) ? 1.0 : 0.0;
            evz = (cxx > czz) ? 0.0 : 1.0;
        }
        const double evl = sqrt(evx * evx + evz * evz);
        evx /= evl; evz /= evl;

        // Sign-align with accumulated flight direction.
        if (n_bands_used > 0
            && (evx * flight_h0 + evz * flight_h1) < 0) {
            evx = -evx; evz = -evz;
        }
        flight_h0 += evx;
        flight_h1 += evz;
        n_bands_used++;
    }
    free(band);

    const double flen = sqrt(flight_h0 * flight_h0 + flight_h1 * flight_h1);
    if (flen < 1e-8) return 0;
    flight_h0 /= flen; flight_h1 /= flen;

    // Cross-track = perpendicular(-flight_h1, +flight_h0). Sign-align so
    // points with higher-than-median scan angle project FURTHER along
    // ct than below-median ones.
    double ct_h0 = -flight_h1;
    double ct_h1 =  flight_h0;
    const float med_angle = scan_angles[s->members[s->n_members / 2]];
    double hi_sum = 0, lo_sum = 0;
    // Frama-C audit finding BEAM-2: hi_n / lo_n are `int` but the loop is
    // bounded by `size_t s->n_members`. Widen to size_t so a strip with
    // > INT_MAX members can't wrap the counter — currently safe (LIDAR
    // point counts are in the millions per strip) but the type was
    // inconsistent with the loop bound.
    size_t hi_n = 0, lo_n = 0;
    for (size_t i = 0; i < s->n_members; i++) {
        const uint32_t gi = s->members[i];
        const double* p = positions + (size_t)gi * 3;
        const double proj = p[h0] * ct_h0 + p[h1] * ct_h1;
        if (scan_angles[gi] > med_angle) { hi_sum += proj; hi_n++; }
        else                              { lo_sum += proj; lo_n++; }
    }
    if (hi_n > 0 && lo_n > 0
        && (hi_sum / (double)hi_n) < (lo_sum / (double)lo_n)) {
        ct_h0 = -ct_h0; ct_h1 = -ct_h1;
    }

    s->ct_h0 = ct_h0;
    s->ct_h1 = ct_h1;
    s->has_fit = 1;
    return 1;
}

// ─── Phase 2b: per-strip multi-return correction ──
// Consecutive returns from the same beam come in as (gi, gi+1) where
// both share PSID + numberOfReturns and have nearly-identical scan
// angles. The (Δh0, Δh1, Δup) vector between them = TRUE beam
// direction. Subtract the model beam (sin(α)·ct) → horizontal
// residual = correction (corr.h0, corr.h1).

static void apply_mr_correction(strip_t* strips, size_t n_strips,
                                 const double* positions,
                                 const float* scan_angles,
                                 const uint16_t* psids,
                                 const uint8_t* return_numbers,
                                 const uint8_t* number_of_returns,
                                 size_t n_pts,
                                 const v7_beam_params_t* params) {
    if (!return_numbers || !number_of_returns) return;
    int h0, h1; horiz_axes(params->up_axis, &h0, &h1);

    // Per-strip accumulator.
    double* sum_h0 = (double*)calloc(n_strips, sizeof(double));
    double* sum_h1 = (double*)calloc(n_strips, sizeof(double));
    // Frama-C audit finding BEAM-3: each n_pairs[s] is incremented once per
    // point assigned to strip s in the loop below; per-strip counts can
    // reach point-count scale (millions). Was `int`; widen to size_t so
    // the increment can't wrap. Comparison `< params->mr_min_pairs` is
    // size_t-vs-int — well-defined for non-negative mr_min_pairs.
    size_t* n_pairs = (size_t*)calloc(n_strips, sizeof(size_t));
    if (!sum_h0 || !sum_h1 || !n_pairs) {
        free(sum_h0); free(sum_h1); free(n_pairs); return;
    }

    // Build a quick PSID → strip-index lookup (linear ok for small n_strips).
    for (size_t gi = 0; gi + 1 < n_pts; gi++) {
        if (number_of_returns[gi] < 2) continue;
        if (psids[gi] != psids[gi + 1]) continue;
        if (number_of_returns[gi] != number_of_returns[gi + 1]) continue;
        if (return_numbers[gi] != 1 || return_numbers[gi + 1] != 2) continue;
        const float a1 = scan_angles[gi];
        const float a2 = scan_angles[gi + 1];
        const double da = (a1 > a2 ? (double)(a1 - a2) : (double)(a2 - a1));
        if (da > params->mr_max_angle_jitter) continue;

        const double* p1 = positions + (size_t)gi * 3;
        const double* p2 = positions + (size_t)(gi + 1) * 3;
        const double dh0 = p2[h0] - p1[h0];
        const double dh1 = p2[h1] - p1[h1];
        const double dup = p2[params->up_axis] - p1[params->up_axis];
        const double len = sqrt(dh0 * dh0 + dh1 * dh1 + dup * dup);
        if (len < params->mr_min_pair_dist) continue;
        const double bx = dh0 / len, bz = dh1 / len;

        // Find this PSID's strip.
        const uint16_t psid = psids[gi];
        int s_idx = -1;
        for (size_t s = 0; s < n_strips; s++) {
            if (strips[s].psid == psid) { s_idx = (int)s; break; }
        }
        if (s_idx < 0 || !strips[s_idx].has_fit) continue;

        // Model beam horizontal component for this point.
        const double a_rad = (double)scan_angles[gi] * V7_PI / 180.0;
        const double sin_a = sin(a_rad);
        const double model_bx = sin_a * strips[s_idx].ct_h0;
        const double model_bz = sin_a * strips[s_idx].ct_h1;

        sum_h0[s_idx] += bx - model_bx;
        sum_h1[s_idx] += bz - model_bz;
        n_pairs[s_idx]++;
    }

    for (size_t s = 0; s < n_strips; s++) {
        if (n_pairs[s] < (size_t)params->mr_min_pairs) continue;
        strips[s].corr_h0 = sum_h0[s] / (double)n_pairs[s];
        strips[s].corr_h1 = sum_h1[s] / (double)n_pairs[s];
        strips[s].has_correction = 1;
    }

    free(sum_h0); free(sum_h1); free(n_pairs);
}

// ─── Phase 3b: per-point beam direction array ──

static void build_beam_dirs(const strip_t* strips, size_t n_strips,
                             const float* scan_angles,
                             const uint16_t* psids,
                             size_t n_pts,
                             int up_axis,
                             float* out_beam_dirs) {
    int h0, h1; horiz_axes(up_axis, &h0, &h1);

    // Default = straight down along up-axis.
    for (size_t i = 0; i < n_pts; i++) {
        out_beam_dirs[i * 3 + 0] = 0.0f;
        out_beam_dirs[i * 3 + 1] = 0.0f;
        out_beam_dirs[i * 3 + 2] = 0.0f;
        out_beam_dirs[i * 3 + (size_t)up_axis] = -1.0f;
    }

    // For each strip with a fit, paint per-member beam direction.
    for (size_t s = 0; s < n_strips; s++) {
        if (!strips[s].has_fit) continue;
        const double ct_h0 = strips[s].ct_h0;
        const double ct_h1 = strips[s].ct_h1;
        const double corr_h0 = strips[s].has_correction ? strips[s].corr_h0 : 0.0;
        const double corr_h1 = strips[s].has_correction ? strips[s].corr_h1 : 0.0;
        for (size_t k = 0; k < strips[s].n_members; k++) {
            const uint32_t gi = strips[s].members[k];
            const double a_rad = (double)scan_angles[gi] * V7_PI / 180.0;
            const double sin_a = sin(a_rad);
            const double cos_a = cos(a_rad);
            const double bh0 = sin_a * ct_h0 + corr_h0;
            const double b_up = -cos_a;
            const double bh1 = sin_a * ct_h1 + corr_h1;
            const double len = sqrt(bh0 * bh0 + b_up * b_up + bh1 * bh1);
            const double inv_len = (len > 0) ? 1.0 / len : 1.0;
            const size_t o = (size_t)gi * 3;
            // Reset to 0 then write the chosen components — avoids the
            // need to know default-vs-painted separately.
            out_beam_dirs[o + 0] = 0.0f;
            out_beam_dirs[o + 1] = 0.0f;
            out_beam_dirs[o + 2] = 0.0f;
            out_beam_dirs[o + (size_t)h0]      = (float)(bh0 * inv_len);
            out_beam_dirs[o + (size_t)h1]      = (float)(bh1 * inv_len);
            out_beam_dirs[o + (size_t)up_axis] = (float)(b_up * inv_len);
            (void)psids;  // not needed at this stage — strip member list already filters
        }
    }
}

// ─── Main entry ──

int v7_beam_compute(const double* positions,
                    const float*  scan_angles,
                    const uint16_t* psids,
                    const uint8_t*  return_numbers,
                    const uint8_t*  number_of_returns,
                    size_t n_pts,
                    const uint32_t* building_indices,
                    size_t n_bldg,
                    const v7_beam_params_t* params,
                    float* out_beam_dirs) {
    if (!positions || !scan_angles || !psids || !out_beam_dirs || !params) return -1;

    int h0, h1; horiz_axes(params->up_axis, &h0, &h1);
    (void)h0; (void)h1;  // used inside phase fns

    // ── Phase 1: group buildIdx points by PSID (first-seen order) ──
    strip_t* strips = NULL;
    size_t n_strips = 0;
    size_t strips_cap = 0;

    for (size_t i = 0; i < n_bldg; i++) {
        const uint32_t gi = building_indices[i];
        if (gi >= n_pts) continue;
        const uint16_t psid = psids[gi];
        size_t found = (size_t)-1;
        for (size_t s = 0; s < n_strips; s++) {
            if (strips[s].psid == psid) { found = s; break; }
        }
        if (found == (size_t)-1) {
            if (n_strips == strips_cap) {
                size_t nc = strips_cap ? strips_cap * 2 : 8;
                strip_t* p = (strip_t*)realloc(strips, nc * sizeof(strip_t));
                if (!p) goto oom;
                strips = p; strips_cap = nc;
            }
            memset(&strips[n_strips], 0, sizeof(strip_t));
            strips[n_strips].psid = psid;
            found = n_strips++;
        }
        if (strip_push(&strips[found], gi) != 0) goto oom;
    }

    // Default-fill out_beam_dirs (Phase 3b later overwrites painted points).
    // Doing it here so OOM exits with a valid output buffer.
    {
        const int up = params->up_axis;
        for (size_t i = 0; i < n_pts; i++) {
            out_beam_dirs[i * 3 + 0] = 0.0f;
            out_beam_dirs[i * 3 + 1] = 0.0f;
            out_beam_dirs[i * 3 + 2] = 0.0f;
            out_beam_dirs[i * 3 + (size_t)up] = -1.0f;
        }
    }

    // ── Phase 2a: per-strip flight direction ──
    for (size_t s = 0; s < n_strips; s++) {
        if ((int32_t)strips[s].n_members < params->min_points_per_strip) continue;
        if (strip_fit_flight_direction(&strips[s], positions, scan_angles,
                                        h0, h1, params->min_band_elongation) < 0)
            goto oom;
    }

    // ── Phase 2b: per-strip multi-return correction ──
    apply_mr_correction(strips, n_strips, positions, scan_angles, psids,
                         return_numbers, number_of_returns, n_pts, params);

    // ── Phase 3b: per-point beam direction array ──
    build_beam_dirs(strips, n_strips, scan_angles, psids, n_pts,
                     params->up_axis, out_beam_dirs);

    for (size_t s = 0; s < n_strips; s++) free(strips[s].members);
    free(strips);
    return 0;

oom:
    for (size_t s = 0; s < n_strips; s++) free(strips[s].members);
    free(strips);
    return -1;
}
