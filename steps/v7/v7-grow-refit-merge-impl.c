// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-grow-refit-merge — see header.

#include "v7-grow-refit-merge-impl.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v7-plane-fit.h"
#include "v7-trace.h"

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

void v7_grm_params_default(v7_grm_params_t* p) {
    p->dist_threshold       = 0.12;
    p->max_degradation      = 0.04;
    p->max_iterations       = 5;        // v2 body's actual default (meta says 10 — v2 bug)
    p->stable_threshold     = 0.002;
    p->search_radius_scale  = 3.0;
    p->search_radius_max    = 1.0;
    p->density              = 25.0;
    p->merge_angle_deg      = 10.0;
    p->merge_wall_angle_deg = 15.0;
    p->merge_offset_scale   = 2.0;
    p->merge_min_shared_pts = 3;
    p->wall_tilt_deg        = 70.0;
    p->coherence_majority   = 0.70;
    p->coherence_max_rounds = 5;
    p->coherence_min_votes  = 3;
    p->redundancy_tau       = 0.5;      // dissolve planes a neighbour fits ~as well
    p->redundancy_far       = 1.0;
    p->up_axis              = 1;        // bench frame
}

void v7_grm_outputs_init(v7_grm_outputs_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

void v7_grm_outputs_free(v7_grm_outputs_t* out) {
    if (!out) return;
    free(out->owner);
    free(out->ridge_mask);
    free(out->residual);
    if (out->planes) {
        for (size_t i = 0; i < out->n_planes; i++) {
            free(out->planes[i].seed_indices);
            free(out->planes[i].void_ids);
        }
        free(out->planes);
    }
    if (out->log_lines) {
        for (size_t i = 0; i < out->n_log_lines; i++) free(out->log_lines[i]);
        free(out->log_lines);
    }
    memset(out, 0, sizeof(*out));
}

// ─── Per-plane claim set: ordered append-list + bitmap. ──
//   - .has(gi)        O(1) via bitmap
//   - .add(gi)        O(1) amortised (skips if already in)
//   - .delete(gi)     O(1) bitmap clear; list keeps stale entries
//   - iterate         walk list, bitmap-skip stale (matches JS Set's
//                      insertion-order iteration including post-delete
//                      gaps; spread `[...set]` produces the live entries
//                      in insertion order, exactly what we want)

typedef struct claim_set {
    uint32_t* members;          // unique entries (in first-add insertion order)
    size_t n_members;
    size_t cap;
    size_t n_active;            // = popcount(bitmap)
    uint8_t* bitmap;            // length (n_pts + 7) / 8 — currently live
    uint8_t* in_list;           // length (n_pts + 7) / 8 — ever in members[]
                                //   (so re-add after delete doesn't duplicate)
} claim_set_t;

static int claim_init(claim_set_t* c, size_t n_pts) {
    c->members = NULL; c->n_members = 0; c->cap = 0; c->n_active = 0;
    const size_t bm_bytes = (n_pts + 7) / 8;
    c->bitmap = (uint8_t*)calloc(bm_bytes, 1);
    c->in_list = (uint8_t*)calloc(bm_bytes, 1);
    return (c->bitmap && c->in_list) ? 0 : -1;
}

static void claim_free(claim_set_t* c) {
    free(c->members); free(c->bitmap); free(c->in_list);
    c->members = NULL; c->bitmap = NULL; c->in_list = NULL;
}

static inline int claim_has(const claim_set_t* c, uint32_t gi) {
    return (c->bitmap[gi >> 3] >> (gi & 7)) & 1;
}

static int claim_add(claim_set_t* c, uint32_t gi) {
    const size_t byte = gi >> 3;
    const uint8_t mask = (uint8_t)(1u << (gi & 7));
    if (c->bitmap[byte] & mask) return 0;     // already live
    c->bitmap[byte] |= mask;
    c->n_active++;
    // Append to members[] only the FIRST time we ever add this gi —
    // subsequent re-adds (after a delete) flip the bit back without
    // duplicating. Matches JS Set semantics for our usage (we never
    // care about post-readd insertion order shifts in this algo).
    if (c->in_list[byte] & mask) return 0;
    c->in_list[byte] |= mask;
    if (c->n_members == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 16;
        uint32_t* p = (uint32_t*)safe_realloc_array(c->members, nc, sizeof(uint32_t));
        if (!p) return -1;
        c->members = p; c->cap = nc;
    }
    c->members[c->n_members++] = gi;
    return 0;
}

static void claim_delete(claim_set_t* c, uint32_t gi) {
    const size_t byte = gi >> 3;
    const uint8_t mask = (uint8_t)(1u << (gi & 7));
    if (!(c->bitmap[byte] & mask)) return;
    c->bitmap[byte] &= (uint8_t)~mask;
    c->n_active--;
}

// Snapshot the live members into a freshly-allocated array (caller frees).
// Order = insertion order, skipping deleted entries — matches JS spread
// `[...claimSets[pi]]` semantics.
static uint32_t* claim_snapshot(const claim_set_t* c, size_t* out_n) {
    if (c->n_active == 0) { *out_n = 0; return NULL; }
    uint32_t* arr = (uint32_t*)safe_malloc_array(c->n_active, sizeof(uint32_t));
    if (!arr) { *out_n = 0; return NULL; }
    size_t k = 0;
    for (size_t i = 0; i < c->n_members; i++) {
        const uint32_t gi = c->members[i];
        if ((c->bitmap[gi >> 3] >> (gi & 7)) & 1) arr[k++] = gi;
    }
    *out_n = k;
    return arr;
}

// ─── Logging helpers ──

static int log_pushf(v7_grm_outputs_t* out, const char* fmt, ...) {
    if (!out || !fmt) return 0;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    char** p = (char**)realloc(out->log_lines, (out->n_log_lines + 1) * sizeof(char*));
    if (!p) return -1;
    out->log_lines = p;
    out->log_lines[out->n_log_lines] = (char*)malloc((size_t)n + 1);
    if (!out->log_lines[out->n_log_lines]) return -1;
    memcpy(out->log_lines[out->n_log_lines], buf, (size_t)n + 1);
    out->n_log_lines++;
    return 0;
}

// ─── Geometry helpers ──

static inline void query_pt(const double* positions, uint32_t gi, double q[3]) {
    q[0] = positions[gi * 3 + 0];
    q[1] = positions[gi * 3 + 1];
    q[2] = positions[gi * 3 + 2];
}

static inline double signed_dist(const double n[3], double d, const double* p) {
    return n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + d;
}

static inline double abs_d(double x) { return x < 0 ? -x : x; }

// Unweighted geometric RMS of a plane over a point-index list:
// sqrt(mean of squared PERPENDICULAR distances).
//
// The plane FIT is area-weighted (correct — it de-biases the non-uniform
// sampling when estimating the plane's tilt/offset). But the degradation /
// merge GATES must judge fit quality with a WEIGHT-FREE residual: the
// per-point weight is the Voronoi cell *area* a point covers (a tangential
// quantity) and has no bearing on how far the point sits *perpendicular* to
// the plane. Weighting the residual also up-weights sparse swath-edge points
// — the noisiest ones — biasing the contamination test by local density.
// And max_degradation (= 2·σ_z) is calibrated in real metres, which only the
// unweighted geometric RMS reports. (Audit 2026-06-01; see doc.json.)
static double geom_rms_plane(const double* positions,
                             const double normal[3], double d,
                             const uint32_t* idx, size_t n) {
    if (n == 0) return 0.0;
    double sumSq = 0.0;
    for (size_t i = 0; i < n; i++) {
        double q[3]; query_pt(positions, idx[i], q);
        const double s = signed_dist(normal, d, q);
        sumSq += s * s;
    }
    return sqrt(sumSq / (double)n);
}

// point_weights is REQUIRED at v7_grm_compute entry — no NULL check
// needed in this hot-path helper.
static double search_radius(const float* point_weights, uint32_t gi,
                             double scale, double max_r, double inv_sqrt_density) {
    // Connectivity radius = scale · max(local spacing, global spacing) —
    // adaptive `√w` floored at the global spacing `1/√density`. Dimensionally
    // [L] (the old `scale·√w·(1/√d)` was [L²], ~5× too small). The junk-sliver
    // problem this used to chase is now handled where it belongs — the GRM
    // redundancy prune (a spurious plane that a neighbour explains almost as
    // well is dissolved) — so the radius can stay the principled form rather
    // than being tuned to starve spurious seeds. See ts-weights audit journal.
    const double local_spacing = sqrt((double)point_weights[gi]);
    const double length = local_spacing > inv_sqrt_density ? local_spacing : inv_sqrt_density;
    const double r = scale * length;
    return r < max_r ? r : max_r;
}

// Run kd_radius_search; if the result count exceeds *cap (i.e. the buffer was
// too small to fit every neighbour), realloc-grow *nbrs to count+16 and retry.
// On alloc failure returns -1 — *nbrs is left valid (the realloc failure leaves
// the OLD pointer intact per C standard) so the caller's `free(*nbrs)` cleanup
// is always safe.
//
// kd_radius_search's own non-zero return (e.g. internal failure) is treated as
// benign here, matching the original inline-retry behaviour: we stop retrying
// and return 0 (the caller will see whatever *n_count was last written and
// proceed). Factored from two identical retry loops in the coherence and
// ridge-mask passes (task #72c, Family C self-dup).
static int radius_search_grow(const kd_tree_t* tree, const double q[3], double R,
                              kd_neighbor_t** nbrs, int* cap, int* n_count) {
    for (;;) {
        if (kd_radius_search(tree, q, R, *nbrs, *cap, n_count) != 0) return 0;
        if (*n_count <= *cap) return 0;
        const int new_cap = *n_count + 16;
        kd_neighbor_t* p = (kd_neighbor_t*)realloc(*nbrs, (size_t)new_cap * sizeof(kd_neighbor_t));
        if (!p) return -1;
        *nbrs = p;
        *cap  = new_cap;
    }
}

static double tilt_deg(const double n[3], int up_axis) {
    double v = abs_d(n[up_axis]);
    if (v > 1.0) v = 1.0;
    return acos(v) * (180.0 / V7_PI);
}

// ─── Per-PSID refit. JS map insertion-order = first-seen-PSID order.
//    We replicate by walking claimedArr once, recording unique PSIDs in
//    first-seen order. ──

typedef struct strip_bucket {
    uint16_t psid;            // U16 — matches LAZ point-source-id dtype
    uint32_t* members;
    size_t   n;
    size_t   cap;
} strip_bucket_t;

static int psid_refit_fit(const double* positions,
                           const float* point_weights,
                           const uint32_t* claimed, size_t n_claimed,
                           const uint16_t* psids,
                           int up_axis,
                           v7_plane_fit_t* out_fit) {
    // Bucket by PSID in first-seen order.
    strip_bucket_t* buckets = NULL;
    size_t n_buckets = 0;
    size_t buckets_cap = 0;
    for (size_t i = 0; i < n_claimed; i++) {
        const uint32_t gi = claimed[i];
        const float psid = psids[gi];
        // Linear search — n_buckets is small (number of strips, usually 1-3).
        // -Wfloat-equal flags the `==` on psid, but it's safe: PSIDs are
        // small integers stored as float (parcel/strip identifier upstream
        // uses int IDs that fit exactly in float mantissa, ≤ 2²³ values).
        // Float-equal between two integer-valued floats is exact.
        size_t found = (size_t)-1;
        for (size_t b = 0; b < n_buckets; b++) {
            if (buckets[b].psid == psid) { found = b; break; }
        }
        if (found == (size_t)-1) {
            if (n_buckets == buckets_cap) {
                const size_t nc = buckets_cap ? buckets_cap * 2 : 4;
                strip_bucket_t* nb = (strip_bucket_t*)realloc(buckets, nc * sizeof(*buckets));
                if (!nb) goto fail;
                buckets = nb; buckets_cap = nc;
            }
            buckets[n_buckets].psid = psid;
            buckets[n_buckets].members = NULL;
            buckets[n_buckets].n = 0;
            buckets[n_buckets].cap = 0;
            found = n_buckets++;
        }
        if (buckets[found].n == buckets[found].cap) {
            const size_t nc = buckets[found].cap ? buckets[found].cap * 2 : 16;
            uint32_t* nm = (uint32_t*)safe_realloc_array(buckets[found].members, nc, sizeof(uint32_t));
            if (!nm) goto fail;
            buckets[found].members = nm; buckets[found].cap = nc;
        }
        buckets[found].members[buckets[found].n++] = gi;
    }

    // Per-strip LSQ → equal-weight average.
    if (n_buckets >= 2) {
        // Buckets with < 3 pts are skipped.
        v7_plane_fit_t* fits = (v7_plane_fit_t*)safe_malloc_array(n_buckets, sizeof(v7_plane_fit_t));
        if (!fits) goto fail;
        size_t n_fits = 0;
        for (size_t b = 0; b < n_buckets; b++) {
            if (buckets[b].n < 3) continue;
            v7_plane_fit_t f;
            if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), buckets[b].members, buckets[b].n,
                                  up_axis, &f) != 0) continue;
            fits[n_fits++] = f;
        }
        if (n_fits >= 2) {
            // Sign-aligned average to first strip's normal.
            //
            // FP trap (audit #274): `sign = dot > 0 ? 1 : -1` is a coin flip
            // when |dot| ≈ 0 — i.e. when a strip's fit normal is nearly
            // perpendicular to ref. For co-planar strips that's pathological
            // (a near-orthogonal strip can't be the same plane as ref), so
            // reject the strip from the average rather than letting FP
            // rounding decide whether to add or subtract its normal.
            //
            // 0.5 ≈ 60° from ref. Anything below means the strip is
            // disagreeing wildly with ref's orientation; including it in
            // the average would corrupt the result. If too few strips
            // survive, fall through to single-LSQ on all claimed points.
            const double STRIP_ALIGN_MIN = 0.5;
            const double* ref = fits[0].normal;
            double nx = 0, ny = 0, nz = 0, dsum = 0;
            size_t n_aligned = 0;
            for (size_t k = 0; k < n_fits; k++) {
                const double dot = fits[k].normal[0] * ref[0]
                                 + fits[k].normal[1] * ref[1]
                                 + fits[k].normal[2] * ref[2];
                if (fabs(dot) < STRIP_ALIGN_MIN) continue;
                const double sign = dot > 0 ? 1.0 : -1.0;
                nx += sign * fits[k].normal[0];
                ny += sign * fits[k].normal[1];
                nz += sign * fits[k].normal[2];
                dsum += sign * fits[k].d;
                n_aligned++;
            }
            if (n_aligned < 2) {
                // Too few strips agree with ref — fall through to single-LSQ.
                free(fits); goto single_lsq_fallback;
            }
            const double len = sqrt(nx * nx + ny * ny + nz * nz);
            const double inv_len = (len > 0) ? 1.0 / len : 1.0;
            const double an[3] = { nx * inv_len, ny * inv_len, nz * inv_len };
            // Divide by n_aligned (strips actually included) so the average
            // matches its contributors. Using n_fits here would bias `d`
            // toward zero when some strips were rejected for poor alignment.
            const double ad = dsum / (double)n_aligned;
            // RMS over ALL claimed points against the averaged equation.
            double sumSq = 0;
            for (size_t i = 0; i < n_claimed; i++) {
                const double* p = positions + (size_t)claimed[i] * 3;
                const double s = an[0] * p[0] + an[1] * p[1] + an[2] * p[2] + ad;
                sumSq += s * s;
            }
            out_fit->normal[0] = an[0];
            out_fit->normal[1] = an[1];
            out_fit->normal[2] = an[2];
            out_fit->d = ad;
            out_fit->rms = sqrt(sumSq / (double)n_claimed);
            // centroid + eigenvalues are not consumed from the averaged fit.
            out_fit->centroid[0] = 0; out_fit->centroid[1] = 0; out_fit->centroid[2] = 0;
            out_fit->eigenvalues[0] = out_fit->eigenvalues[1] = out_fit->eigenvalues[2] = 0;
            free(fits);
            for (size_t b = 0; b < n_buckets; b++) free(buckets[b].members);
            free(buckets);
            return 0;
        }
        free(fits);
    }
    // Fallback: single LSQ on all claimed.
single_lsq_fallback:
    for (size_t b = 0; b < n_buckets; b++) free(buckets[b].members);
    free(buckets);
    return v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), claimed, n_claimed, up_axis, out_fit);

fail:
    if (buckets) {
        for (size_t b = 0; b < n_buckets; b++) free(buckets[b].members);
        free(buckets);
    }
    return -1;
}

// ─── Main compute ──

int v7_grm_compute(const double* positions,
                   const float*  point_weights,
                   const uint16_t* psids,
                   size_t n_pts,
                   const uint32_t* pool_indices, size_t n_pool,
                   const kd_tree_t* tree,
                   const v7_grm_input_seed_t* seeds, size_t K,
                   const v7_grm_params_t* params,
                   v7_grm_outputs_t* out) {
    if (!out || !positions || !pool_indices || !tree || !params) return -1;
    // point_weights is REQUIRED — see v7-plane-fit.h. No silent fallback.
    if (!point_weights) return -1;
    v7_grm_outputs_init(out);
    out->n_pts = n_pts;

    // Always-allocated outputs.
    out->owner       = (int16_t*)safe_malloc_array(n_pts, sizeof(int16_t));
    out->ridge_mask  = (uint8_t*)calloc(n_pts, 1);
    out->residual    = (float*) safe_malloc_array(n_pts, sizeof(float));
    if (!out->owner || !out->ridge_mask || !out->residual) goto oom;
    for (size_t i = 0; i < n_pts; i++) out->owner[i] = -1;
    for (size_t i = 0; i < n_pts; i++) out->residual[i] = INFINITY;
    out->distance_threshold = params->dist_threshold;

    if (n_pool == 0 || K == 0) {
        log_pushf(out, "grow-refit-merge: no building points or no seeds");
        return 0;
    }

    const double dist_threshold     = params->dist_threshold;
    const double max_degradation    = params->max_degradation;
    const int    max_iters          = params->max_iterations;
    const double stable_threshold   = params->stable_threshold;
    const double radius_scale       = params->search_radius_scale;
    const double radius_max         = params->search_radius_max;
    const double inv_sqrt_density   = 1.0 / sqrt(params->density);
    const double merge_angle_cos    = cos(params->merge_angle_deg     * V7_PI / 180.0);
    const double merge_wall_cos     = cos(params->merge_wall_angle_deg * V7_PI / 180.0);
    const double merge_offset_max   = params->merge_offset_scale * dist_threshold;
    const int    merge_min_shared   = params->merge_min_shared_pts;
    const double wall_tilt_deg_p    = params->wall_tilt_deg;
    const double coh_majority       = params->coherence_majority;
    const int    coh_max_rounds     = params->coherence_max_rounds;
    const int    coh_min_votes      = params->coherence_min_votes;
    const int    up_axis            = params->up_axis;

    // ── State arrays ──
    claim_set_t* claims = (claim_set_t*)safe_calloc_array(K, sizeof(claim_set_t));
    double (*plane_n)[3] = (double(*)[3])malloc(K * sizeof(double[3]));
    double*  plane_d   = (double*)safe_malloc_array(K, sizeof(double));
    double*  plane_rms = (double*)safe_calloc_array(K, sizeof(double));
    uint8_t* tomb      = (uint8_t*)calloc(K, 1);
    // Per-plane void_id sets — atom-of-truth for "this plane owns these
    // dv2 void ids". Initialised from seeds (one void_id per ghost seed),
    // unioned on merge, transferred from j→i when plane j is tombstoned.
    // Both arrays are length K. v_ids[pi] is owned (caller-frees if not
    // moved out into v7_grm_outputs).
    uint32_t** v_ids = (uint32_t**)calloc(K, sizeof(uint32_t*));
    uint32_t*  v_n   = (uint32_t*) calloc(K, sizeof(uint32_t));
    if (!claims || !plane_n || !plane_d || !plane_rms || !tomb || !v_ids || !v_n) {
        free(claims); free(plane_n); free(plane_d); free(plane_rms); free(tomb);
        free(v_ids); free(v_n);
        goto oom;
    }
    for (size_t pi = 0; pi < K; pi++) {
        if (claim_init(&claims[pi], n_pts) != 0) {
            for (size_t j = 0; j < pi; j++) claim_free(&claims[j]);
            free(claims); free(plane_n); free(plane_d); free(plane_rms); free(tomb);
            goto oom;
        }
    }

    // ── Init claims + initial RMS from seeds ──
    int n_ghost = 0;
    for (size_t pi = 0; pi < K; pi++) {
        plane_n[pi][0] = seeds[pi].normal[0];
        plane_n[pi][1] = seeds[pi].normal[1];
        plane_n[pi][2] = seeds[pi].normal[2];
        plane_d[pi]    = seeds[pi].d;
        if (seeds[pi].is_ghost) {
            n_ghost++;
            // Initial void-id set = [ghost_void_id]. Capacity 4 is enough
            // for the common case (1-3 voids per plane after merging); we
            // realloc on growth in the merge loop below.
            v_ids[pi] = (uint32_t*)malloc(4 * sizeof(uint32_t));
            if (!v_ids[pi]) {
                // OOM cleanup of partial v_ids — same shape as other oom paths.
                for (size_t j = 0; j < pi; j++) free(v_ids[j]);
                for (size_t j = 0; j < K; j++) claim_free(&claims[j]);
                free(claims); free(plane_n); free(plane_d); free(plane_rms); free(tomb);
                free(v_ids); free(v_n);
                goto oom;
            }
            v_ids[pi][0] = seeds[pi].ghost_void_id;
            v_n[pi] = 1;
        }
        for (uint32_t i = 0; i < seeds[pi].n_seeds; i++) {
            claim_add(&claims[pi], seeds[pi].seed_indices[i]);
        }
        if (claims[pi].n_active >= 3) {
            double sumSq = 0;
            for (size_t i = 0; i < claims[pi].n_members; i++) {
                const uint32_t gi = claims[pi].members[i];
                if (!claim_has(&claims[pi], gi)) continue;
                double q[3]; query_pt(positions, gi, q);
                const double s = signed_dist(plane_n[pi], plane_d[pi], q);
                sumSq += s * s;
            }
            plane_rms[pi] = sqrt(sumSq / (double)claims[pi].n_active);
        }
    }
    log_pushf(out,
              "grow-refit-merge: %zu seed planes (%zu normal + %d ghost), "
              "distThreshold=%.3fm, maxDegradation=%.3fm",
              K, K - (size_t)n_ghost, n_ghost, dist_threshold, max_degradation);

    // Reusable scratch buffers.
    int radius_cap = 64;
    kd_neighbor_t* nbrs = (kd_neighbor_t*)malloc((size_t)radius_cap * sizeof(kd_neighbor_t));
    if (!nbrs) goto oom_state;

    // Mini-resolve scratch, sized to global N.
    int32_t* best_owner = (int32_t*)safe_malloc_array(n_pts, sizeof(int32_t));
    float*   best_res   = (float*)  safe_malloc_array(n_pts, sizeof(float));
    if (!best_owner || !best_res) {
        free(nbrs); free(best_owner); free(best_res); goto oom_state;
    }

    // Frontier stack (LIFO — matches JS frontier.pop()).
    uint32_t* frontier = (uint32_t*)safe_malloc_array(n_pool, sizeof(uint32_t));
    if (!frontier) {
        free(nbrs); free(best_owner); free(best_res); goto oom_state;
    }

    int iter_done = 0;
    int iter = 0;
    for (; iter < max_iters; iter++) {
        size_t changes = 0;

        // ── Mini-resolve ──
        for (size_t i = 0; i < n_pts; i++) { best_owner[i] = -1; best_res[i] = INFINITY; }
        for (size_t pi = 0; pi < K; pi++) {
            if (tomb[pi]) continue;
            const double n0 = plane_n[pi][0], n1 = plane_n[pi][1], n2 = plane_n[pi][2];
            const double dd = plane_d[pi];
            for (size_t i = 0; i < claims[pi].n_members; i++) {
                const uint32_t gi = claims[pi].members[i];
                if (!claim_has(&claims[pi], gi)) continue;
                const double* p = positions + (size_t)gi * 3;
                double res = n0 * p[0] + n1 * p[1] + n2 * p[2] + dd;
                if (res < 0) res = -res;
                if ((float)res < best_res[gi]) {
                    best_res[gi] = (float)res;
                    best_owner[gi] = (int32_t)pi;
                }
            }
        }

        // ── Diffusion-limited grow ──
        for (size_t pi = 0; pi < K; pi++) {
            if (tomb[pi] || claims[pi].n_active == 0) continue;
            const double n0 = plane_n[pi][0], n1 = plane_n[pi][1], n2 = plane_n[pi][2];
            const double dd = plane_d[pi];
            // Build frontier: live claimed points where THIS plane is best owner.
            // Iteration order matches insertion order (JS Set spread).
            size_t f_top = 0;
            for (size_t i = 0; i < claims[pi].n_members; i++) {
                const uint32_t gi = claims[pi].members[i];
                if (!claim_has(&claims[pi], gi)) continue;
                if (best_owner[gi] == (int32_t)pi) frontier[f_top++] = gi;
            }
            // Visited tracker: snapshot current claimed set + each new addition.
            // Use a per-iteration bitmap so we don't realloc.
            const size_t bm_bytes = (n_pts + 7) / 8;
            uint8_t* visited = (uint8_t*)malloc(bm_bytes);
            if (!visited) {
                free(frontier); free(nbrs); free(best_owner); free(best_res);
                goto oom_state;
            }
            memcpy(visited, claims[pi].bitmap, bm_bytes);
            while (f_top > 0) {
                const uint32_t gi = frontier[--f_top];
                const double R = search_radius(point_weights, gi,
                                                radius_scale, radius_max, inv_sqrt_density);
                double q[3]; query_pt(positions, gi, q);
                int n_count = 0;
                for (;;) {
                    if (kd_radius_search(tree, q, R, nbrs, radius_cap, &n_count) != 0) break;
                    if (n_count <= radius_cap) break;
                    radius_cap = n_count + 16;
                    kd_neighbor_t* p = (kd_neighbor_t*)realloc(nbrs, (size_t)radius_cap * sizeof(kd_neighbor_t));
                    if (!p) { free(visited); free(frontier); free(nbrs); free(best_owner); free(best_res); goto oom_state; }
                    nbrs = p;
                }
                const int n_use = n_count < radius_cap ? n_count : radius_cap;
                for (int j = 0; j < n_use; j++) {
                    const uint32_t nb = (uint32_t)nbrs[j].idx;
                    const size_t byte = nb >> 3;
                    const uint8_t mask = (uint8_t)(1u << (nb & 7));
                    if (visited[byte] & mask) continue;
                    visited[byte] |= mask;
                    const double* np = positions + (size_t)nb * 3;
                    double s = n0 * np[0] + n1 * np[1] + n2 * np[2] + dd;
                    if (s < 0) s = -s;
                    if (s <= dist_threshold) {
                        if (!claim_has(&claims[pi], nb)) {
                            claim_add(&claims[pi], nb);
                            changes++;
                        }
                    }
                }
            }
            free(visited);
        }

        // ── Per-PSID refit + RMS revert ──
        size_t reverts = 0;
        for (size_t pi = 0; pi < K; pi++) {
            if (tomb[pi]) continue;
            if (claims[pi].n_active < 3) continue;
            size_t n_claimed = 0;
            uint32_t* claimed = claim_snapshot(&claims[pi], &n_claimed);
            if (!claimed || n_claimed < 3) { free(claimed); continue; }

            v7_plane_fit_t fit;
            int rc;
            if (psids) {
                rc = psid_refit_fit(positions, point_weights, claimed, n_claimed, psids, up_axis, &fit);
            } else {
                rc = v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), claimed, n_claimed, up_axis, &fit);
            }
            if (rc != 0) { free(claimed); continue; }

            // Judge the refit with a WEIGHT-FREE geometric residual — the fit
            // is area-weighted but the gate must not be (see geom_rms_plane).
            const double fit_grms = geom_rms_plane(positions, fit.normal, fit.d, claimed, n_claimed);
            const double rms_limit = plane_rms[pi] + max_degradation;
            if (fit_grms > rms_limit) {
                // Revert: eject points beyond distThreshold of the OLD equation.
                const double on0 = plane_n[pi][0], on1 = plane_n[pi][1], on2 = plane_n[pi][2];
                const double od = plane_d[pi];
                size_t ejected = 0;
                for (size_t i = 0; i < n_claimed; i++) {
                    const uint32_t gi = claimed[i];
                    const double* p = positions + (size_t)gi * 3;
                    double s = on0 * p[0] + on1 * p[1] + on2 * p[2] + od;
                    if (s < 0) s = -s;
                    if (s > dist_threshold) {
                        claim_delete(&claims[pi], gi);
                        ejected++;
                    }
                }
                // Refit on remainder.
                if (claims[pi].n_active >= 3) {
                    size_t n_remain = 0;
                    uint32_t* remain = claim_snapshot(&claims[pi], &n_remain);
                    if (remain) {
                        v7_plane_fit_t fit2;
                        if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), remain, n_remain, up_axis, &fit2) == 0) {
                            plane_n[pi][0] = fit2.normal[0];
                            plane_n[pi][1] = fit2.normal[1];
                            plane_n[pi][2] = fit2.normal[2];
                            plane_d[pi]    = fit2.d;
                            plane_rms[pi]  = geom_rms_plane(positions, fit2.normal, fit2.d, remain, n_remain);
                        }
                        free(remain);
                    }
                }
                changes += ejected;
                reverts++;
            } else {
                plane_n[pi][0] = fit.normal[0];
                plane_n[pi][1] = fit.normal[1];
                plane_n[pi][2] = fit.normal[2];
                plane_d[pi]    = fit.d;
                plane_rms[pi]  = fit_grms;
            }
            free(claimed);
        }

        // ── Coplanar merge ──
        size_t merges = 0;
        for (size_t i = 0; i < K; i++) {
            if (tomb[i] || claims[i].n_active == 0) continue;
            const double* nA = plane_n[i];
            for (size_t j = i + 1; j < K; j++) {
                if (tomb[j] || claims[j].n_active == 0) continue;
                const double* nB = plane_n[j];

                const double dot = abs_d(nA[0]*nB[0] + nA[1]*nB[1] + nA[2]*nB[2]);
                const double tA = tilt_deg(nA, up_axis);
                const double tB = tilt_deg(nB, up_axis);
                const double eff_cos = (tA > wall_tilt_deg_p && tB > wall_tilt_deg_p)
                    ? merge_wall_cos : merge_angle_cos;
                if (dot < eff_cos) continue;

                // Centroid of plane j.
                double cx = 0, cy = 0, cz = 0;
                size_t nj = 0;
                for (size_t k = 0; k < claims[j].n_members; k++) {
                    const uint32_t gi = claims[j].members[k];
                    if (!claim_has(&claims[j], gi)) continue;
                    const double* p = positions + (size_t)gi * 3;
                    cx += p[0]; cy += p[1]; cz += p[2];
                    nj++;
                }
                if (nj == 0) continue;
                cx /= (double)nj; cy /= (double)nj; cz /= (double)nj;
                const double offset = abs_d(nA[0]*cx + nA[1]*cy + nA[2]*cz + plane_d[i]);
                if (offset > merge_offset_max) continue;

                // Connexity: ≥ merge_min_shared of plane j's points have a plane i
                // member within their search radius. Iterate j's claimed in
                // insertion order (matches JS Set iteration).
                int shared = 0;
                for (size_t k = 0; k < claims[j].n_members && shared < merge_min_shared; k++) {
                    const uint32_t gj = claims[j].members[k];
                    if (!claim_has(&claims[j], gj)) continue;
                    const double R = search_radius(point_weights, gj,
                                                    radius_scale, radius_max, inv_sqrt_density);
                    double q[3]; query_pt(positions, gj, q);
                    int n_count = 0;
                    for (;;) {
                        if (kd_radius_search(tree, q, R, nbrs, radius_cap, &n_count) != 0) break;
                        if (n_count <= radius_cap) break;
                        radius_cap = n_count + 16;
                        kd_neighbor_t* p = (kd_neighbor_t*)realloc(nbrs, (size_t)radius_cap * sizeof(kd_neighbor_t));
                        if (!p) { free(frontier); free(nbrs); free(best_owner); free(best_res); goto oom_state; }
                        nbrs = p;
                    }
                    const int n_use = n_count < radius_cap ? n_count : radius_cap;
                    for (int q2 = 0; q2 < n_use; q2++) {
                        if (claim_has(&claims[i], (uint32_t)nbrs[q2].idx)) { shared++; break; }
                    }
                }
                if (shared < merge_min_shared) continue;

                // Try LSQ on union (i first in insertion order, then j).
                size_t nu = claims[i].n_active + claims[j].n_active;
                uint32_t* uarr = (uint32_t*)safe_malloc_array(nu, sizeof(uint32_t));
                if (!uarr) { free(frontier); free(nbrs); free(best_owner); free(best_res); goto oom_state; }
                size_t un = 0;
                // i's live members (in insertion order)
                for (size_t k = 0; k < claims[i].n_members; k++) {
                    const uint32_t gi = claims[i].members[k];
                    if (claim_has(&claims[i], gi)) uarr[un++] = gi;
                }
                // j's live members not already in i (in j's insertion order)
                for (size_t k = 0; k < claims[j].n_members; k++) {
                    const uint32_t gj = claims[j].members[k];
                    if (!claim_has(&claims[j], gj)) continue;
                    if (claim_has(&claims[i], gj)) continue;
                    uarr[un++] = gj;
                }
                v7_plane_fit_t ufit;
                if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), uarr, un, up_axis, &ufit) != 0) {
                    free(uarr); continue;
                }
                const double ufit_grms = geom_rms_plane(positions, ufit.normal, ufit.d, uarr, un);
                const double rms_limit_m = (plane_rms[i] > plane_rms[j] ? plane_rms[i] : plane_rms[j])
                                         + max_degradation;
                if (ufit_grms > rms_limit_m) { free(uarr); continue; }

                // Accept. Merge j → i.
                const size_t before = claims[i].n_active;
                for (size_t k = 0; k < claims[j].n_members; k++) {
                    const uint32_t gj = claims[j].members[k];
                    if (claim_has(&claims[j], gj)) claim_add(&claims[i], gj);
                }
                plane_n[i][0] = ufit.normal[0];
                plane_n[i][1] = ufit.normal[1];
                plane_n[i][2] = ufit.normal[2];
                plane_d[i]    = ufit.d;
                plane_rms[i]  = ufit_grms;
                // Void-id set union: append every j-side void not already
                // in i's set. Sorted-merge would be cheaper but n_voids
                // is tiny (≤ a few) so the O(n·m) scan is fine.
                if (v_n[j] > 0) {
                    uint32_t need = v_n[i] + v_n[j];
                    uint32_t* grown = (uint32_t*)realloc(v_ids[i], need * sizeof(uint32_t));
                    if (grown) {
                        v_ids[i] = grown;
                        for (uint32_t kj = 0; kj < v_n[j]; kj++) {
                            uint32_t vid = v_ids[j][kj];
                            bool dup = false;
                            for (uint32_t ki = 0; ki < v_n[i]; ki++) {
                                if (v_ids[i][ki] == vid) { dup = true; break; }
                            }
                            if (!dup) v_ids[i][v_n[i]++] = vid;
                        }
                    }
                    // Either way, j's voids are now "handed off". Drop j's array.
                    free(v_ids[j]); v_ids[j] = NULL; v_n[j] = 0;
                }
                tomb[j] = 1;
                changes += claims[i].n_active - before;
                log_pushf(out, "  Iter %d: merge #%zu -> #%zu (union %zu, rms=%.4f)",
                          iter + 1, j, i, claims[i].n_active, ufit.rms);
                merges++;
                free(uarr);
            }
        }

        // ── Redundant-plane prune ──────────────────────────────────────
        // Dissolve a plane a neighbour explains almost as well. For each of
        // its currently-owned points, compare residual to THIS plane vs the
        // next-best CLAIMING plane; redundancy = (alt_RMS − own_RMS)/
        // dist_threshold. Below tau ⇒ tombstone; its points re-resolve to
        // their next-best owner next iteration (same mechanism as merge).
        // Skip iter 0 — a real plane mid-growth can transiently look
        // redundant before it has claimed its exclusive points.
        size_t pruned = 0;
        if (iter >= 1 && params->redundancy_tau > 0.0) {
            for (size_t pi = 0; pi < K; pi++) {
                if (tomb[pi] || claims[pi].n_active == 0) continue;
                const double n0 = plane_n[pi][0], n1 = plane_n[pi][1], n2 = plane_n[pi][2], dd = plane_d[pi];
                double sum_own2 = 0.0, sum_alt2 = 0.0; size_t cnt = 0;
                for (size_t k = 0; k < claims[pi].n_members; k++) {
                    const uint32_t gi = claims[pi].members[k];
                    if (!claim_has(&claims[pi], gi)) continue;
                    if (best_owner[gi] != (int32_t)pi) continue;   // points pi currently owns
                    const double* p = positions + (size_t)gi * 3;
                    double own = n0 * p[0] + n1 * p[1] + n2 * p[2] + dd; if (own < 0) own = -own;
                    double alt = params->redundancy_far;           // stand-in if no competing claimer
                    for (size_t pj = 0; pj < K; pj++) {
                        if (pj == pi || tomb[pj] || !claim_has(&claims[pj], gi)) continue;
                        double s = plane_n[pj][0]*p[0] + plane_n[pj][1]*p[1] + plane_n[pj][2]*p[2] + plane_d[pj];
                        if (s < 0) s = -s;
                        if (s < alt) alt = s;
                    }
                    sum_own2 += own * own; sum_alt2 += alt * alt; cnt++;
                }
                if (cnt < 3) continue;
                const double own_rms = sqrt(sum_own2 / (double)cnt);
                const double alt_rms = sqrt(sum_alt2 / (double)cnt);
                const double redundancy = (alt_rms - own_rms) / dist_threshold;
                V7_TRACEF("GRM-RED", "iter=%d plane=%zu n=%zu own_rms=%.4f alt_rms=%.4f red=%.2f -> %s",
                          iter + 1, pi, cnt, own_rms, alt_rms, redundancy,
                          redundancy < params->redundancy_tau ? "PRUNE" : "keep");
                if (redundancy < params->redundancy_tau) {
                    tomb[pi] = 1;
                    changes += claims[pi].n_active;
                    pruned++;
                    log_pushf(out, "  Iter %d: redundancy-prune #%zu (own=%.4f alt=%.4f red=%.2f)",
                              iter + 1, pi, own_rms, alt_rms, redundancy);
                }
            }
        }

        // ── Iter end ──
        size_t alive = 0;
        for (size_t pi = 0; pi < K; pi++) if (!tomb[pi]) alive++;
        log_pushf(out,
                  "Iter %d: D=%zu (%zu refits reverted, %zu merges), %zu/%zu alive",
                  iter + 1, changes, reverts, merges, alive, K);
        const size_t stable_cap = (size_t)(stable_threshold * (double)n_pool);
        if (changes <= stable_cap && merges == 0 && reverts == 0 && pruned == 0) {
            log_pushf(out, "  stable (D=%zu < %zu)", changes, stable_cap + 1);
            iter_done = iter + 1;
            iter++;
            break;
        }
    }
    if (iter_done == 0) iter_done = iter;
    free(frontier);

    // ── Min-residual resolve ──
    for (size_t i = 0; i < n_pool; i++) {
        const uint32_t gi = pool_indices[i];
        int best_pi = -1;
        double best_r = INFINITY;
        for (size_t pi = 0; pi < K; pi++) {
            if (tomb[pi]) continue;
            if (!claim_has(&claims[pi], gi)) continue;
            const double* p = positions + (size_t)gi * 3;
            double s = plane_n[pi][0]*p[0] + plane_n[pi][1]*p[1] + plane_n[pi][2]*p[2] + plane_d[pi];
            if (s < 0) s = -s;
            if (s < best_r) { best_r = s; best_pi = (int)pi; }
        }
        if (best_pi >= 0) out->owner[gi] = (int16_t)best_pi;
    }

    // ── Spatial coherence ──
    {
        // Vote-tally scratch — shared across all neighbour visits in this
        // pass. Sized to current radius_cap; grown lazily.
        int  vote_cap = radius_cap;
        int* vote_planes = (int*)malloc((size_t)vote_cap * sizeof(int));
        int* vote_counts = (int*)malloc((size_t)vote_cap * sizeof(int));
        if (!vote_planes || !vote_counts) {
            free(vote_planes); free(vote_counts);
            free(nbrs); free(best_owner); free(best_res); goto oom_state;
        }
        size_t total_moves = 0;
        for (int round = 0; round < coh_max_rounds; round++) {
            size_t moved = 0;
            for (size_t i = 0; i < n_pool; i++) {
                const uint32_t gi = pool_indices[i];
                const int16_t my = out->owner[gi];
                if (my < 0) continue;
                const double R = search_radius(point_weights, gi,
                                                radius_scale, radius_max, inv_sqrt_density);
                double q[3]; query_pt(positions, gi, q);
                int n_count = 0;
                if (radius_search_grow(tree, q, R, &nbrs, &radius_cap, &n_count) != 0) {
                    free(nbrs); free(best_owner); free(best_res); goto oom_state;
                }
                const int n_use = n_count < radius_cap ? n_count : radius_cap;
                if (n_use > vote_cap) {
                    vote_cap = n_use + 16;
                    int* vp = (int*)realloc(vote_planes, (size_t)vote_cap * sizeof(int));
                    int* vc = (int*)realloc(vote_counts, (size_t)vote_cap * sizeof(int));
                    if (!vp || !vc) {
                        free(vp ? vp : vote_planes); free(vc ? vc : vote_counts);
                        free(nbrs); free(best_owner); free(best_res); goto oom_state;
                    }
                    vote_planes = vp; vote_counts = vc;
                }
                int n_votes = 0;
                int total_votes = 0;
                for (int k = 0; k < n_use; k++) {
                    const uint32_t nb = (uint32_t)nbrs[k].idx;
                    if (nb == gi) continue;
                    const int16_t nbo = out->owner[nb];
                    if (nbo < 0) continue;
                    int found = -1;
                    for (int v = 0; v < n_votes; v++) {
                        if (vote_planes[v] == nbo) { found = v; break; }
                    }
                    if (found >= 0) vote_counts[found]++;
                    else { vote_planes[n_votes] = nbo; vote_counts[n_votes] = 1; n_votes++; }
                    total_votes++;
                }
                if (total_votes < coh_min_votes) continue;

                int maj_pi = my, maj_count = 0;
                for (int v = 0; v < n_votes; v++) {
                    if (vote_planes[v] == my) maj_count = vote_counts[v];
                }
                for (int v = 0; v < n_votes; v++) {
                    if (vote_counts[v] > maj_count) {
                        maj_count = vote_counts[v];
                        maj_pi = vote_planes[v];
                    }
                }
                if (maj_pi == my) continue;
                if ((double)maj_count / (double)total_votes < coh_majority) continue;

                // Residual gate: majority plane must fit this point.
                const double* p = positions + (size_t)gi * 3;
                double s = plane_n[maj_pi][0]*p[0] + plane_n[maj_pi][1]*p[1]
                         + plane_n[maj_pi][2]*p[2] + plane_d[maj_pi];
                if (s < 0) s = -s;
                if (s > dist_threshold) continue;

                out->owner[gi] = (int16_t)maj_pi;
                moved++;
            }
            total_moves += moved;
            if (moved == 0) break;
            log_pushf(out, "  Coherence round %d: %zu moves", round + 1, moved);
        }
        if (total_moves > 0) {
            log_pushf(out, "Spatial coherence: %zu total reassignments", total_moves);
        }
        free(vote_planes); free(vote_counts);
    }

    // ── Final per-plane refit + build planes_out ──
    out->planes = (v7_grm_plane_t*)safe_calloc_array(K, sizeof(v7_grm_plane_t));
    if (!out->planes) { free(nbrs); free(best_owner); free(best_res); goto oom_state; }
    out->n_planes = K;

    int32_t* plane_count = (int32_t*)safe_calloc_array(K, sizeof(int32_t));
    if (!plane_count) { free(nbrs); free(best_owner); free(best_res); goto oom_state; }
    for (size_t i = 0; i < n_pool; i++) {
        const int16_t pi = out->owner[pool_indices[i]];
        if (pi >= 0) plane_count[pi]++;
    }

    int active = 0;
    int32_t total_assigned = 0;
    for (size_t pi = 0; pi < K; pi++) {
        v7_grm_plane_t* p = &out->planes[pi];
        p->hdr.normal[0] = plane_n[pi][0];
        p->hdr.normal[1] = plane_n[pi][1];
        p->hdr.normal[2] = plane_n[pi][2];
        p->hdr.d         = plane_d[pi];
        p->hdr.rms       = plane_rms[pi];
        p->hdr.tilt      = tilt_deg(plane_n[pi], up_axis);
        p->hdr.azimuth   = v7_azimuth_from_normal(plane_n[pi], up_axis);
        p->hdr.source_seed_index = (uint32_t)pi;
        p->hdr.is_ghost  = (uint8_t)(pi < K && seeds[pi].is_ghost);
        p->hdr.tombstoned = (uint8_t)tomb[pi];
        p->hdr.inlier_count = 0;
        // Transfer void_id ownership into the output plane. Tombstoned
        // planes have already had their voids handed off to the survivor
        // by the merge loop, so v_ids[pi]/v_n[pi] is NULL/0 for them.
        p->void_ids   = v_ids[pi];
        p->n_void_ids = v_n[pi];
        v_ids[pi] = NULL; v_n[pi] = 0;

        if (tomb[pi]) continue;
        const int32_t cnt = plane_count[pi];
        if (cnt < 3) continue;
        // Collect resolved-owner indices in pool order.
        uint32_t* idx = (uint32_t*)malloc((size_t)cnt * sizeof(uint32_t));
        if (!idx) { free(plane_count); free(nbrs); free(best_owner); free(best_res); goto oom_state; }
        size_t k = 0;
        for (size_t i = 0; i < n_pool; i++) {
            const uint32_t gi = pool_indices[i];
            if (out->owner[gi] == (int16_t)pi) idx[k++] = gi;
        }
        // Final refit; on failure keep the iteration's last plane equation.
        v7_plane_fit_t f;
        if (v7_plane_fit_lsq(positions, v7_fit_weights(point_weights), idx, k, up_axis, &f) == 0) {
            p->hdr.normal[0] = f.normal[0];
            p->hdr.normal[1] = f.normal[1];
            p->hdr.normal[2] = f.normal[2];
            p->hdr.d         = f.d;
            // Reported rms is the weight-free geometric residual (consistent
            // with the gates + the plane_rms baseline); the fit stays weighted.
            p->hdr.rms       = geom_rms_plane(positions, f.normal, f.d, idx, k);
            p->hdr.tilt      = tilt_deg(f.normal, up_axis);
            p->hdr.azimuth   = v7_azimuth_from_normal(f.normal, up_axis);
        }
        p->hdr.inlier_count = (uint32_t)k;
        p->seed_indices = idx;
        active++;
        total_assigned += (int32_t)k;
    }
    free(plane_count);

    log_pushf(out, "grow-refit-merge: %d/%zu active planes, %d/%zu assigned (%d%%), %d iterations",
              active, K, total_assigned, n_pool,
              (int)((100.0 * (double)total_assigned / (double)n_pool) + 0.5),
              iter_done);

    // ── Ridge mask ──
    for (size_t i = 0; i < n_pool; i++) {
        const uint32_t gi = pool_indices[i];
        const int16_t my = out->owner[gi];
        if (my < 0) continue;
        const double R = search_radius(point_weights, gi,
                                        radius_scale, radius_max, inv_sqrt_density);
        double q[3]; query_pt(positions, gi, q);
        int n_count = 0;
        if (radius_search_grow(tree, q, R, &nbrs, &radius_cap, &n_count) != 0) {
            free(nbrs); free(best_owner); free(best_res); goto oom_state;
        }
        const int n_use = n_count < radius_cap ? n_count : radius_cap;
        for (int j = 0; j < n_use; j++) {
            const uint32_t nb = (uint32_t)nbrs[j].idx;
            if (nb == gi) continue;
            const int16_t nbo = out->owner[nb];
            if (nbo >= 0 && nbo != my) { out->ridge_mask[gi] = 1; break; }
        }
    }

    // ── Per-point residual ──
    for (size_t i = 0; i < n_pool; i++) {
        const uint32_t gi = pool_indices[i];
        const int16_t pi = out->owner[gi];
        if (pi < 0) continue;
        const double* p = positions + (size_t)gi * 3;
        double s = out->planes[pi].hdr.normal[0] * p[0]
                 + out->planes[pi].hdr.normal[1] * p[1]
                 + out->planes[pi].hdr.normal[2] * p[2]
                 + out->planes[pi].hdr.d;
        if (s < 0) s = -s;
        out->residual[gi] = (float)s;
    }

    free(nbrs); free(best_owner); free(best_res);

    for (size_t pi = 0; pi < K; pi++) claim_free(&claims[pi]);
    free(claims); free(plane_n); free(plane_d); free(plane_rms); free(tomb);
    // v_ids/v_n entries that were moved out into out->planes[].void_ids
    // are NULL/0; any leftover (shouldn't happen on the success path) is
    // freed defensively.
    for (size_t pi = 0; pi < K; pi++) free(v_ids[pi]);
    free(v_ids); free(v_n);
    return 0;

oom_state:
    for (size_t pi = 0; pi < K; pi++) claim_free(&claims[pi]);
    free(claims); free(plane_n); free(plane_d); free(plane_rms); free(tomb);
    for (size_t pi = 0; pi < K; pi++) free(v_ids[pi]);
    free(v_ids); free(v_n);
oom:
    v7_grm_outputs_free(out);
    return -1;
}
