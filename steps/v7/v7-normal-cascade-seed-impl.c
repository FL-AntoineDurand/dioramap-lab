// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-normal-cascade-seed — see header.

#include "v7-normal-cascade-seed-impl.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v7-plane-fit.h"
#include "v7-trace.h"

void v7_seed_params_default(v7_seed_params_t* p) {
    // V7 specifically: tile-signals hardcodes σ_z=0.02 m
    // (lib/pipeline/steps/tile-signals.js:189), overriding the
    // 0.03 default that lives in extract-denoise (which v7 never
    // runs). distThreshold = max(0.05, 4.5×0.02) = 0.09 — matches
    // the seed-dist-threshold.json value in v2 dumps. The
    // pre-port audit listed 0.135 because it traced extract-denoise;
    // the actual live value in v7 is 0.09.
    p->dist_threshold = 0.09;
    // Curvature gate threshold. As of 2026-06-02 the gate compares the
    // MEDIAN point curvature of a segment (border-robust), not the
    // area-weighted mean. The mean was inflated by a small plane's
    // high-curvature perimeter (border/inner ratio grows as planes shrink),
    // over-rejecting legit small roofs; the area-weighting made it worse.
    // 0.055 separates labelled roofs/walls from ridge/valley segments on
    // the dahouet building (median curv of genuine ridges ≥ 0.07).
    p->max_seed_curvature = 0.055;
    p->wall_tilt_deg = 70.0;
    p->up_axis = 1;          // bench-v2 default; wrapper overrides to 2 for LAZ
}

void v7_seed_outputs_init(v7_seed_outputs_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

void v7_seed_outputs_free(v7_seed_outputs_t* out) {
    if (!out) return;
    if (out->planes) {
        for (size_t i = 0; i < out->n_planes; i++) free(out->planes[i].seed_indices);
        free(out->planes);
    }
    free(out->owner);
    memset(out, 0, sizeof(*out));
}

// ── Sort key — (sum_weight DESC, source_segment_id ASC for stability) ──

typedef struct seg_sort_key {
    double sum_weight;          // sort key (area sans /density — constant scale)
    uint32_t input_index;       // index into the input segments[] array
    uint32_t source_segment_id; // tiebreaker
} seg_sort_key_t;

static int cmp_float_asc(const void* a, const void* b) {
    const float fa = *(const float*)a, fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

static int seg_sort_cmp(const void* a, const void* b) {
    const seg_sort_key_t* sa = (const seg_sort_key_t*)a;
    const seg_sort_key_t* sb = (const seg_sort_key_t*)b;
    if (sa->sum_weight > sb->sum_weight) return -1;
    if (sa->sum_weight < sb->sum_weight) return  1;
    if (sa->source_segment_id < sb->source_segment_id) return -1;
    if (sa->source_segment_id > sb->source_segment_id) return  1;
    return 0;
}

// ── Compute ──

int v7_seed_compute(const double* positions,
                    const float* curvatures,
                    const float* point_weights,
                    size_t n_pts,
                    const v7_seed_input_segment_t* segments,
                    size_t n_segments,
                    const v7_seed_params_t* params,
                    v7_seed_outputs_t* out) {
    if (!out || !positions || !segments || !params) return -1;
    // point_weights is REQUIRED — see v7-plane-fit.h. No silent fallback.
    // v2's "ctx.hasArtifact" silent-NULL behaviour is forbidden here; all
    // callers MUST pass an explicit uniform-1.0 array if they want
    // unweighted semantics.
    if (!point_weights) return -1;
    v7_seed_outputs_init(out);
    out->n_pts = n_pts;

    out->owner = (int16_t*)safe_malloc_array(n_pts, sizeof(int16_t));
    if (!out->owner) return -1;
    for (size_t i = 0; i < n_pts; i++) out->owner[i] = -1;

    if (n_segments == 0) return 0;

    const double dist_threshold = params->dist_threshold;
    const double max_seed_curv  = params->max_seed_curvature;
    const double wall_tilt_deg  = params->wall_tilt_deg;
    const int    up_axis        = params->up_axis;

    // Trace gate: V7_SEED_TRACE=<per-building local point index> traces the
    // one segment that contains that point through every accept/reject
    // decision (segment pt_indices here are building-local, NOT global tile
    // indices). The env read is compiled out entirely unless -DDIORAMAP_V7_TRACE.
#if V7_TRACE_ENABLED
    const char* trace_env = getenv("V7_SEED_TRACE");
    const long  trace_gidx = trace_env ? atol(trace_env) : -1;
#endif

    // ── Sort segments by area (sum-of-weights) DESC ──
    seg_sort_key_t* keys = (seg_sort_key_t*)safe_malloc_array(n_segments, sizeof(seg_sort_key_t));
    if (!keys) return -1;
    for (size_t i = 0; i < n_segments; i++) {
        // point_weights is non-NULL by guard above; uniform-1.0 yields the
        // legacy `s = n_pts` semantics, but the call site must opt in.
        double s = 0.0;
        for (uint32_t j = 0; j < segments[i].n_pts; j++) {
            s += (double)point_weights[segments[i].pt_indices[j]];
        }
        keys[i].sum_weight = s;
        keys[i].input_index = (uint32_t)i;
        keys[i].source_segment_id = segments[i].source_segment_id;
    }
    qsort(keys, n_segments, sizeof(seg_sort_key_t), seg_sort_cmp);

    // ── Per-segment loop ──
    v7_seed_plane_t* planes = (v7_seed_plane_t*)safe_malloc_array(n_segments, sizeof(v7_seed_plane_t));
    if (!planes) { free(keys); return -1; }
    size_t n_planes = 0;

    // Scratch buffer for the "clean" inlier sublist after outlier
    // rejection — sized to the largest segment.
    uint32_t max_seg_pts = 0;
    for (size_t i = 0; i < n_segments; i++) {
        if (segments[i].n_pts > max_seg_pts) max_seg_pts = segments[i].n_pts;
    }
    uint32_t* clean = (uint32_t*)malloc((size_t)max_seg_pts * sizeof(uint32_t));
    if (!clean && max_seg_pts > 0) { free(planes); free(keys); return -1; }
    // Scratch for the curvature-gate median (per-segment curvature copy → sort).
    float* curv_scratch = (float*)malloc((size_t)max_seg_pts * sizeof(float));
    if (!curv_scratch && max_seg_pts > 0) { free(clean); free(planes); free(keys); return -1; }

    for (size_t k = 0; k < n_segments; k++) {
        const v7_seed_input_segment_t* seg = &segments[keys[k].input_index];
        const uint32_t* gidx = seg->pt_indices;
        const uint32_t n_seg = seg->n_pts;

        // Trace gate: is the traced point in this segment?
        int trace = 0;
#if V7_TRACE_ENABLED
        if (trace_gidx >= 0) {
            for (uint32_t i = 0; i < n_seg; i++) {
                if (gidx[i] == (uint32_t)trace_gidx) { trace = 1; break; }
            }
        }
#endif
        (void)trace;  // unused when V7_TRACE is compiled out
        V7_TRACE_IF(trace, "SEED", "seg src_id=%u n_pts=%u sum_weight=%.4f rank=%zu/%zu",
                    seg->source_segment_id, n_seg, keys[k].sum_weight, k, n_segments);

        if (n_seg < 3) {
            V7_TRACE_IF(trace, "SEED", "  REJECT: n_seg=%u < 3", n_seg);
            continue;
        }

        // S2: LSQ plane fit (also serves as the curvature-gate fit
        // since v7 hardcodes excludeRidges=false → identical inputs
        // for both v2 fits; reuse).
        v7_plane_fit_t fit;
        if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), gidx, n_seg, up_axis, &fit) != 0) {
            V7_TRACE_IF(trace, "SEED", "  REJECT: plane_fit_lsq failed (n_seg=%u)", n_seg);
            continue;
        }

        // S1: curvature gate (after fit so we have tilt). Exempt walls
        // (tilt > wall_tilt_deg). For the rest, skip when the MEDIAN point
        // curvature exceeds the threshold. The median is border-robust: a
        // small/legit plane's high-curvature perimeter inflates the mean but
        // not the median, so the gate no longer over-rejects small planes.
        // (Replaced the area-weighted mean 2026-06-02 — see journal.)
        // point_weights is intentionally NOT used here.
        if (curvatures && max_seed_curv > 0.0) {
            const double tilt = v7_tilt_from_normal(fit.normal, up_axis);
            if (tilt < wall_tilt_deg) {
                for (uint32_t i = 0; i < n_seg; i++)
                    curv_scratch[i] = curvatures[gidx[i]];
                qsort(curv_scratch, n_seg, sizeof(float), cmp_float_asc);
                const double median = (n_seg & 1u)
                    ? (double)curv_scratch[n_seg / 2]
                    : 0.5 * ((double)curv_scratch[n_seg / 2 - 1] +
                             (double)curv_scratch[n_seg / 2]);
                V7_TRACE_IF(trace, "SEED",
                            "  curvature gate: median_curv=%.5f max=%.5f tilt=%.1f wall=%.1f -> %s",
                            median, max_seed_curv, tilt, wall_tilt_deg,
                            (median > max_seed_curv) ? "REJECT" : "pass");
                if (median > max_seed_curv) continue;
            } else {
                V7_TRACE_IF(trace, "SEED",
                            "  curvature gate: EXEMPT (wall, tilt=%.1f >= %.1f)",
                            tilt, wall_tilt_deg);
            }
        }

        // S3: outlier rejection.
        uint32_t n_clean = 0;
        for (uint32_t i = 0; i < n_seg; i++) {
            const uint32_t gi = gidx[i];
            const double* p = positions + (size_t)gi * 3;
            double dist = v7_signed_distance_to_plane(fit.normal, fit.d, p);
            if (dist < 0) dist = -dist;
            if (dist <= dist_threshold) clean[n_clean++] = gi;
        }
        V7_TRACE_IF(trace, "SEED", "  outlier reject: n_clean=%u / n_seg=%u (dist_thr=%.3f)",
                    n_clean, n_seg, dist_threshold);
        if (n_clean < 3) {
            V7_TRACE_IF(trace, "SEED", "  REJECT: n_clean=%u < 3", n_clean);
            continue;
        }

        // S4: refit on clean if outliers were removed.
        v7_plane_fit_t final = fit;
        if (n_clean < n_seg) {
            v7_plane_fit_t refit;
            if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), clean, n_clean, up_axis, &refit) == 0) {
                final = refit;
            }
            // v2 leaves `tilt` from the FIRST fit (computed before
            // outlier-rejection); we need to do the same for byte-id'l
            // tilt values. The refit's normal is used for d/normal;
            // tilt is from the pre-refit normal.
        }
        const double tilt_pre = v7_tilt_from_normal(fit.normal, up_axis);

        V7_TRACE_IF(trace, "SEED",
                    "  ACCEPT: plane idx=%zu n_clean=%u rms=%.4f tilt=%.1f",
                    n_planes, n_clean, final.rms, tilt_pre);

        // Persist the plane.
        v7_seed_plane_t* sp = &planes[n_planes];
        sp->hdr.normal[0] = final.normal[0];
        sp->hdr.normal[1] = final.normal[1];
        sp->hdr.normal[2] = final.normal[2];
        sp->hdr.d         = final.d;
        sp->hdr.tilt      = tilt_pre;       // matches v2: tilt from initial fit
        sp->hdr.azimuth   = v7_azimuth_from_normal(final.normal, up_axis);
        sp->hdr.rms       = final.rms;
        sp->hdr.inlier_count = n_clean;
        sp->hdr.source_segment_id = seg->source_segment_id;
        sp->seed_indices = (uint32_t*)malloc((size_t)n_clean * sizeof(uint32_t));
        if (!sp->seed_indices) {
            // Best-effort cleanup of what we've built so far.
            for (size_t j = 0; j < n_planes; j++) free(planes[j].seed_indices);
            free(planes); free(clean); free(curv_scratch); free(keys);
            return -1;
        }
        memcpy(sp->seed_indices, clean, (size_t)n_clean * sizeof(uint32_t));
        n_planes++;
    }

    free(clean);
    free(curv_scratch);
    free(keys);

    // Trim the planes array to the actual count.
    if (n_planes < n_segments) {
        v7_seed_plane_t* trimmed = (v7_seed_plane_t*)realloc(
            planes, n_planes ? n_planes * sizeof(v7_seed_plane_t) : sizeof(v7_seed_plane_t));
        if (trimmed) planes = trimmed;
    }
    out->planes = planes;
    out->n_planes = n_planes;

    // Build owner from final plane assignments.
    for (size_t pi = 0; pi < n_planes; pi++) {
        const v7_seed_plane_t* sp = &planes[pi];
        const int16_t pid = (int16_t)pi;
        for (uint32_t i = 0; i < sp->hdr.inlier_count; i++) {
            const uint32_t gi = sp->seed_indices[i];
            out->owner[gi] = pid;
        }
    }

    return 0;
}
