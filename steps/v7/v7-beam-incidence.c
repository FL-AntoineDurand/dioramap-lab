// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-beam-incidence — step wrapper.
//
// Scope split (recurring v3 pattern, beam-incidence is the cleanest
// example):
//
//   COMPUTE SCOPE   = building points only.
//                     We only compute beams for points that belong
//                     to an extracted building. Ground / vegetation
//                     points don't get a beam direction — the per-
//                     strip PCA wants reasonably planar surfaces
//                     (roofs) to land a clean flight-direction fit;
//                     mixing in ground / canopy adds noise.
//                     Source: per-building-records' globalIndices,
//                     concatenated into a flat tile-wide list.
//
//   STORAGE SCOPE   = tile-wide F32×3·N.
//                     Output has one slot per global point. Slots
//                     for non-building points stay at the default
//                     (0, -1, 0) = straight down. Why this shape:
//                     boundary-signals indexes the array by global
//                     gi pulled from other tile-wide arrays
//                     (boundary-candidates-all, classifications,
//                     etc.), so every gi must have a slot even if
//                     many carry the default. Cost: a few hundred
//                     kB of "wasted" zeros — negligible.
//
// In short: WE CALCULATE BEAM ONLY FOR BUILDING POINTS BUT STORE
// THE RESULT IN A TILE-WIDE FORMAT.
//
// Reads:
//   lidar-positions      (F64×3·N tile-wide)
//   per-building-records (per-building — read only to harvest the
//                          flat globalIndices list of building points;
//                          we never iterate per-building inside the
//                          impl)
//   scan-angles          (F32×N tile-wide)
//   point-source-ids     (U16×N tile-wide)
//   return-numbers       (U8×N tile-wide, optional — gates Phase 2b
//                          multi-return correction; absent → no MR
//                          correction, same as v2's behaviour)
//   number-of-returns    (U8×N tile-wide, optional, paired with above)
//
// Writes:
//   beam-directions      (F32×3·N tile-wide, AK_POINT_COLUMN)
//
// Per the audit, beam-incidence's other v2 outputs (beam-incidence
// edges, beam-psid-colors, beam-incidence-meta, sun beams) are viz-
// only and not on the V7 boundary-regularize path; dropped.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/step.h"
#include "v7-beam-incidence-impl.h"

static int parse_params(const char* params_str, v7_beam_params_t* p) {
    v7_beam_params_default(p);
    p->up_axis = 2;  // production v3 LAZ z-up
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    j = json_object_get(obj, "minPointsPerStrip");
    if (json_int32_or_default(j, 1, 1000000, "minPointsPerStrip", &p->min_points_per_strip) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "minBandElongation")) && json_is_number(j))
        p->min_band_elongation = json_number_value(j);
    j = json_object_get(obj, "mrMinPairs");
    if (json_int32_or_default(j, 1, 1000000, "mrMinPairs", &p->mr_min_pairs) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "mrMaxAngleJitter")) && json_is_number(j))
        p->mr_max_angle_jitter = json_number_value(j);
    if ((j = json_object_get(obj, "mrMinPairDist")) && json_is_number(j))
        p->mr_min_pair_dist = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// Concatenate every per-bldg-records global index into one flat tile-
// wide buildIndices array. Caller frees.
static int collect_building_indices(step_context_t* ctx, size_t n_buildings,
                                     uint32_t** out, size_t* out_n) {
    *out = NULL; *out_n = 0;
    size_t total = 0;
    // Pass 1: total count.
    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 12) continue;
        uint32_t n_pts;
        memcpy(&n_pts, rec + 8, 4);
        total += n_pts;
    }
    if (total == 0) return 0;
    uint32_t* arr = (uint32_t*)safe_malloc_array(total, sizeof(uint32_t));
    if (!arr) return -1;
    size_t k = 0;
    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 12) continue;
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts,   rec + 8, 4);
        if (schema != 1) continue;
        const size_t idx_off = 68 + (size_t)n_verts * 24;
        if (idx_off + (size_t)n_pts * 4 > rec_len) continue;
        memcpy(arr + k, rec + idx_off, (size_t)n_pts * 4);
        k += n_pts;
    }
    *out = arr;
    *out_n = k;
    return 0;
}

static int v7_beam_incidence_exec(step_context_t* ctx) {
    v7_beam_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t n_pts = pos_bytes / (3 * sizeof(double));

    size_t sa_bytes = 0;
    const float* scan_angles = (const float*)step_read(ctx, "scan-angles", &sa_bytes);
    if (!scan_angles || sa_bytes != n_pts * sizeof(float)) {
        fprintf(stderr, "[v7-beam-incidence] missing scan-angles, skipping\n");
        return 0;  // produce nothing — boundary-signals falls back to vertical
    }

    size_t psid_bytes = 0;
    const uint16_t* psids = (const uint16_t*)step_read(ctx, "point-source-ids", &psid_bytes);
    if (!psids || psid_bytes != n_pts * sizeof(uint16_t)) {
        fprintf(stderr, "[v7-beam-incidence] missing point-source-ids, skipping\n");
        return 0;
    }

    // Optional MR inputs — present in TCBD when flag bits 2 & 3 are set.
    size_t rn_bytes = 0, nor_bytes = 0;
    const uint8_t* rn  = (const uint8_t*)step_read(ctx, "return-numbers",    &rn_bytes);
    const uint8_t* nor = (const uint8_t*)step_read(ctx, "number-of-returns", &nor_bytes);
    if (rn  && rn_bytes  != n_pts) rn = NULL;
    if (nor && nor_bytes != n_pts) nor = NULL;

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    uint32_t* bldg_indices = NULL;
    size_t n_bldg = 0;
    if (collect_building_indices(ctx, n_buildings, &bldg_indices, &n_bldg) != 0) return -1;

    float* beam_dirs = (float*)malloc(n_pts * 3 * sizeof(float));
    if (!beam_dirs) { free(bldg_indices); return -1; }

    int rc = v7_beam_compute(positions, scan_angles, psids, rn, nor,
                              n_pts, bldg_indices, n_bldg, &params, beam_dirs);
    free(bldg_indices);
    if (rc != 0) { free(beam_dirs); return -1; }

    rc = step_write_output(ctx, "bi-directions",
                            beam_dirs, n_pts * 3 * sizeof(float));
    free(beam_dirs);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_beam_incidence_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records", .array = true},
    {.name = "scan-angles"},
    {.name = "point-source-ids"},
    {.name = "return-numbers"},
    {.name = "number-of-returns"},
};
static const step_write_t v7_beam_incidence_writes[] = {
    {.name = "bi-directions", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t v7_beam_incidence_def = {
    .name = "v7-beam-incidence",
    .version = 1,
    .reads = v7_beam_incidence_reads,
    .n_reads = sizeof(v7_beam_incidence_reads) / sizeof(v7_beam_incidence_reads[0]),
    .writes = v7_beam_incidence_writes,
    .n_writes = sizeof(v7_beam_incidence_writes) / sizeof(v7_beam_incidence_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_beam_incidence_exec,
};
