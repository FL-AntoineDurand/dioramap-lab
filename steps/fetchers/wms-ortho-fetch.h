// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// wms-ortho-fetch.h — shared body for the two IGN WMS ortho-image
// fetcher steps (fetch-ortho-rgb and fetch-ortho-irc).
//
// Extracted in task #72c Family F from two ~50-line exec_fn bodies
// that differed only in (1) default WMS layer string and (2) output
// artifact name. PMD CPD flagged the cluster at 258 tokens / 37 lines.
//
// Reads:  canonical-location (JSON, bbox_l93 — via step_read_bbox_l93)
// Params: size_px (int, default 2048), layer (string, default = arg)
// Writes: `output_artifact` (JPEG bytes, kind AK_IMAGE_JPEG — declared
//         by each step's step_def writes_arr).
#ifndef V3_STEPS_FETCHERS_WMS_ORTHO_FETCH_H
#define V3_STEPS_FETCHERS_WMS_ORTHO_FETCH_H

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pulls bbox_l93 + params from ctx, fetches the WMS ortho JPEG, writes
// it to `output_artifact`. Returns 0 on success, -1 on any failure
// (input/params malformed, URL too long, fetch error, write error).
int wms_ortho_fetch_exec(step_context_t* ctx,
                         const char* default_layer,
                         const char* output_artifact);

#ifdef __cplusplus
}
#endif
#endif
