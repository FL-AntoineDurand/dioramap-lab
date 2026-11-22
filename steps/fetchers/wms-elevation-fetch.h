// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// wms-elevation-fetch.h — shared body for the two IGN WMS-BIL
// elevation fetcher steps (fetch-elevation and fetch-elevation-surround).
//
// Extracted in task #72c Family G from two ~50-line exec_fn bodies
// that differed in (1) bbox source (bbox_l93 vs center±radius), (2)
// default grid_size (512 vs 256), (3) extra radius_m param on surround,
// and (4) output artifact name. PMD CPD flagged the cluster at 275
// tokens / 41 lines.
//
// The bbox computation stays in each step (different input shapes); the
// helper takes the resolved bbox and handles the rest (params parsing,
// URL build, fetch, raster_format header wrap, write).
#ifndef V3_STEPS_FETCHERS_WMS_ELEVATION_FETCH_H
#define V3_STEPS_FETCHERS_WMS_ELEVATION_FETCH_H

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetches a WMS-BIL F32 grid for `bbox`, wraps it in the v3
// raster_format header (u32 cols, u32 rows, u32 n_channels=1), and
// writes it to `output_artifact`. Honors the step's grid_size +
// layer params (falling back to default_grid_size + default_layer).
// Returns 0 on success, -1 on any failure.
int wms_elevation_fetch_exec(step_context_t* ctx,
                             const double bbox[4],
                             int default_grid_size,
                             const char* default_layer,
                             const char* output_artifact);

#ifdef __cplusplus
}
#endif
#endif
