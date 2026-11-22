// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-ortho-rgb
// Reads:  canonical-location — JSON with bbox_l93
// Params: {"size_px": int, "layer": string}
//           defaults: size_px=2048, layer="ORTHOIMAGERY.ORTHOPHOTOS"
// Writes: ortho-rgb-jpeg — raw JPEG bytes (kind AK_IMAGE_JPEG).
//
// The URL is fully determined by canonical-location + params, which IS the
// step identity, so plain `fetch_get` + `step_write_output` is the right
// choice (PLAN-FETCHERS F4b): same identity → runner-level intent-map hit
// → no HTTP. No need for sub-scope `fetch_and_cache`.
//
// Body is in wms-ortho-fetch.c (#72c Family F — shared with fetch-ortho-irc).

#include "v3/artifact.h"
#include "v3/store.h"

#include "fetchers.h"
#include "wms-ortho-fetch.h"

static int exec_fn(step_context_t* ctx) {
    return wms_ortho_fetch_exec(ctx, "ORTHOIMAGERY.ORTHOPHOTOS", "ortho-rgb-jpeg");
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    {.name = "ortho-rgb-jpeg",
     .kind = AK_IMAGE_JPEG,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fetch_ortho_rgb_def = {
    .name = "fetch-ortho-rgb",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
