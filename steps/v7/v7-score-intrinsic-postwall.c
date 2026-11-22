// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-score-intrinsic-postwall — step wrapper.
//
// Per-building intrinsic (no-GT) model-quality scoring AFTER v7-wall-
// detect has run. Reads the post-wall-detect roof-graph TLV (which
// carries ALL plane equations in its PLANES section — both the upstream
// roof planes passed through unchanged and the new wall planes appended
// by wall-detect) + wd-owner (the mutated grm-owner with wall-grabbed
// points reassigned to their wall plane id), then computes exactly the
// same intrinsic metrics as v7-score-intrinsic but over the FULL post-
// wall-detect model. The score it emits accounts for wall-reassigned
// points, which v7-score-intrinsic on grm-* would skip.
//
// Implementation is a thin wrapper over v7_score_intrinsic_compute —
// the math is identical; the only difference is where the planes +
// owner come from. We:
//
//   1. Parse the per-bldg TLV via the rg_reader API in
//      v7-roof-graph-format.h.
//   2. Walk RG_SECT_PLANES, pulling each rg_plane_record_t's id,
//      normal[3], and d. Find max_id along the way.
//   3. Allocate planes[max_id+1] + valid[max_id+1]; fill the planes
//      slots that exist in the TLV. wd-owner values are the plane ids,
//      so they index directly into this array.
//   4. Defensive sweep: any wd-owner value pointing at a slot we didn't
//      fill (defensive — wall-detect's id namespace is dense, so this
//      shouldn't fire in practice) is rewritten to -1 (treated as
//      unassigned) so the compute doesn't read uninitialised planes.
//   5. Call v7_score_intrinsic_compute() on the local positions +
//      sanitised owner + planes table.
//   6. Pack the 72-byte result; emit one record per building into
//      model-score-postwall.
//
// Output artifact name is DIFFERENT from v7-score-intrinsic's
// (model-score-postwall vs model-score) so both can coexist in the
// same pipeline run — handy: the delta tells you what wall-detect
// changed (and at what cost).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"
#include "v7-roof-graph-format.h"
#include "v7-score-intrinsic-impl.h"

// rg_plane_record_t field offsets (matches the pragma-packed struct in
// v7-roof-graph-format.h:207 — kept as named constants here so a future
// schema change makes the lockstep with this step explicit).
#define RG_PLANE_OFF_ID      0
#define RG_PLANE_OFF_NORMAL  32
#define RG_PLANE_OFF_D       56

// ── Extract (id → normal, d) from the per-bldg roof-graph TLV. ─────
//
// Allocates two arrays sized to max_id+1 (so wd-owner indexes directly):
//   *out_planes [v7_score_plane_t]  — filled where valid[i]==true
//   *out_valid  [uint8_t]           — 1 where slot is filled, else 0
//
// On success: writes both arrays + *out_n_slots (= max_id+1). Caller
// owns the buffers via free(). On empty / missing PLANES section,
// returns 0 with both pointers NULL and *out_n_slots = 0. Returns -1
// on TLV corruption or allocation failure.
static int extract_planes_from_rgraph(const uint8_t* tlv_bytes, size_t tlv_len,
                                       v7_score_plane_t** out_planes,
                                       uint8_t** out_valid,
                                       size_t* out_n_slots) {
    *out_planes = NULL;
    *out_valid = NULL;
    *out_n_slots = 0;
    if (!tlv_bytes || tlv_len == 0) return 0;

    rg_reader_t r;
    if (rg_reader_open(&r, tlv_bytes, tlv_len) != 0) return -1;

    const uint8_t* sect_bytes = NULL;
    size_t sect_len = 0;
    int found = rg_reader_section(&r, RG_SECT_PLANES, &sect_bytes, &sect_len);
    if (found != 0) {
        // 1 = not present (no PLANES section in this bldg). -1 = corrupt.
        return (found == 1) ? 0 : -1;
    }

    uint32_t n_records = 0, stride = 0;
    if (rg_section_header(sect_bytes, sect_len, &n_records, &stride) != 0) return -1;
    if (n_records == 0) return 0;
    // We rely on the field offsets above; require stride >= the last
    // field we read (d at offset 56, 8 bytes) so a trimmed record schema
    // doesn't silently read past the record.
    if (stride < RG_PLANE_OFF_D + sizeof(double)) return -1;

    // Pass 1: find max_id (so we can size planes[] / valid[] correctly).
    uint32_t max_id = 0;
    {
        rg_record_iter_t it;
        if (rg_record_iter_init(&it, sect_bytes, sect_len) != 0) return -1;
        const uint8_t* rec; uint32_t rl;
        while (rg_record_iter_next(&it, &rec, &rl) == 1) {
            if (rl < RG_PLANE_OFF_D + sizeof(double)) continue;
            uint32_t id;
            memcpy(&id, rec + RG_PLANE_OFF_ID, sizeof(uint32_t));
            if (id > max_id) max_id = id;
        }
    }

    // Sanity cap — if max_id is wildly large we'd allocate a huge sparse
    // table for nothing useful. wd-owner is int16, so any id ≥ 32768
    // can't be referenced anyway; defensively bail.
    if (max_id >= (uint32_t)INT16_MAX) return -1;

    size_t n_slots = (size_t)max_id + 1;
    v7_score_plane_t* planes = (v7_score_plane_t*)safe_calloc_array(n_slots, sizeof(v7_score_plane_t));
    uint8_t* valid = (uint8_t*)safe_calloc_array(n_slots, sizeof(uint8_t));
    if (!planes || !valid) { free(planes); free(valid); return -1; }

    // Pass 2: fill the slots that exist.
    {
        rg_record_iter_t it;
        if (rg_record_iter_init(&it, sect_bytes, sect_len) != 0) {
            free(planes); free(valid); return -1;
        }
        const uint8_t* rec; uint32_t rl;
        while (rg_record_iter_next(&it, &rec, &rl) == 1) {
            if (rl < RG_PLANE_OFF_D + sizeof(double)) continue;
            uint32_t id;
            memcpy(&id, rec + RG_PLANE_OFF_ID, sizeof(uint32_t));
            if ((size_t)id >= n_slots) continue;  // can't happen post pass 1, but be safe
            memcpy(planes[id].normal, rec + RG_PLANE_OFF_NORMAL, 3 * sizeof(double));
            memcpy(&planes[id].d,    rec + RG_PLANE_OFF_D,           sizeof(double));
            valid[id] = 1;
        }
    }

    *out_planes = planes;
    *out_valid = valid;
    *out_n_slots = n_slots;
    return 0;
}

// ── exec ──────────────────────────────────────────────────────────

static int v7_score_intrinsic_postwall_exec(step_context_t* ctx) {
    (void)step_get_params(ctx);  // no tunable params yet — see v7-score-intrinsic

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    size_t n_owner_arr = step_array_len(ctx, "wd-owner");
    size_t n_rg_arr    = step_array_len(ctx, "wall-detect-roof-graph");
    if (n_owner_arr != n_buildings || n_rg_arr != n_buildings) {
        fprintf(stderr,
                "[v7-score-intrinsic-postwall] count mismatch: bldgs=%zu "
                "wd-owner=%zu wall-detect-roof-graph=%zu\n",
                n_buildings, n_owner_arr, n_rg_arr);
        return -1;
    }

    blob_t b_score = {0};

    int rc = 0;
    double*  local_pos = NULL;
    size_t   local_pos_cap = 0;
    int16_t* sanit_owner = NULL;
    size_t   sanit_owner_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // ── per-building-records: gidx + n_pts ──
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 68) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts,   rec + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec + idx_off);

        // ── wd-owner: n_pts × int16 ──
        const uint8_t* owner_bytes = NULL; size_t owner_len = 0;
        if (step_array_read_bytes(ctx, "wd-owner", bi, &owner_bytes, &owner_len) != 0
            || owner_len != (size_t)n_pts * sizeof(int16_t)) {
            blob_push(&b_score, NULL, 0);
            continue;
        }

        // ── post-wall-detect roof-graph TLV → planes[max_id+1] ──
        const uint8_t* tlv_bytes = NULL; size_t tlv_len = 0;
        if (step_array_read_bytes(ctx, "wall-detect-roof-graph", bi, &tlv_bytes, &tlv_len) != 0) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        v7_score_plane_t* planes = NULL; uint8_t* valid = NULL; size_t n_slots = 0;
        if (extract_planes_from_rgraph(tlv_bytes, tlv_len, &planes, &valid, &n_slots) != 0) {
            rc = -1; break;
        }

        // ── Sanitise owner: any int16 ≥ n_slots, or pointing at an
        //    invalid (unfilled) slot, becomes -1 (unassigned). Defensive
        //    — wall-detect's id namespace is dense in practice. ──
        if ((size_t)n_pts > sanit_owner_cap) {
            int16_t* p = (int16_t*)realloc(sanit_owner, (size_t)n_pts * sizeof(int16_t));
            if (!p) { free(planes); free(valid); rc = -1; break; }
            sanit_owner = p;
            sanit_owner_cap = (size_t)n_pts;
        }
        memcpy(sanit_owner, owner_bytes, (size_t)n_pts * sizeof(int16_t));
        for (uint32_t i = 0; i < n_pts; i++) {
            int16_t pid = sanit_owner[i];
            if (pid < 0) continue;
            if ((size_t)pid >= n_slots || !valid[pid]) sanit_owner[i] = -1;
        }

        // ── Build per-bldg local positions[n_pts*3] from gidx ──
        if ((size_t)n_pts * 3 > local_pos_cap) {
            double* p64 = (double*)realloc(local_pos, (size_t)n_pts * 3 * sizeof(double));
            if (!p64) { free(planes); free(valid); rc = -1; break; }
            local_pos = p64;
            local_pos_cap = (size_t)n_pts * 3;
        }
        for (uint32_t i = 0; i < n_pts; i++) {
            uint32_t g = gidx[i];
            if ((size_t)g >= total_points) {
                local_pos[3*i+0] = local_pos[3*i+1] = local_pos[3*i+2] = 0.0;
            } else {
                local_pos[3*i+0] = positions[3*g+0];
                local_pos[3*i+1] = positions[3*g+1];
                local_pos[3*i+2] = positions[3*g+2];
            }
        }

        // ── Score ──
        v7_score_intrinsic_result_t r;
        int crc = v7_score_intrinsic_compute(local_pos, sanit_owner, (size_t)n_pts,
                                              planes, n_slots, &r);
        free(planes);
        free(valid);
        if (crc != 0) { rc = -1; break; }

        uint8_t buf[V7_SCORE_INTRINSIC_RECORD_BYTES];
        v7_score_intrinsic_pack(&r, buf);
        if (blob_push(&b_score, buf, sizeof(buf)) != 0) { rc = -1; break; }
    }

    free(local_pos);
    free(sanit_owner);

    if (rc == 0) rc |= emit_blob(ctx, "model-score-postwall", &b_score);
    blob_free(&b_score);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_score_intrinsic_postwall_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",       .array = true},
    {.name = "wd-owner",                   .array = true},
    {.name = "wall-detect-roof-graph",     .array = true},
};
static const step_write_t v7_score_intrinsic_postwall_writes[] = {
    {.name = "model-score-postwall",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC,
     .array = true},
};

const step_def_t v7_score_intrinsic_postwall_def = {
    .name = "v7-score-intrinsic-postwall",
    .version = 1,
    .reads = v7_score_intrinsic_postwall_reads,
    .n_reads = sizeof(v7_score_intrinsic_postwall_reads)  / sizeof(v7_score_intrinsic_postwall_reads[0]),
    .writes = v7_score_intrinsic_postwall_writes,
    .n_writes = sizeof(v7_score_intrinsic_postwall_writes) / sizeof(v7_score_intrinsic_postwall_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_score_intrinsic_postwall_exec,
};
