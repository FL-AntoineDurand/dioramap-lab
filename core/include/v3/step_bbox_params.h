// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step_bbox_params.h — boilerplate JSON parsers shared by raster-step
// wrappers. Both helpers preserve the existing silent-fallback behavior
// of the static per-file copies they replace (canonical-location missing,
// malformed JSON, wrong type → step returns -1 OR caller's default).
//
// Extracted in task #72c Family E from the 7 step wrappers that hand-
// rolled identical read_bbox_l93 and the 3 that hand-rolled
// parse_cell_size (see commit message for the site list).
#ifndef V3_STEP_BBOX_PARAMS_H
#define V3_STEP_BBOX_PARAMS_H

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

// Read the "canonical-location" artifact bound on `ctx`, parse it as
// JSON, and write its "bbox_l93" 4-element array (xmin, ymin, xmax,
// ymax in Lambert-93) into `bbox`.
//
// Returns 0 on success, -1 on any failure (artifact missing, JSON parse
// error, bbox_l93 absent / wrong type / wrong length). On failure the
// contents of `bbox` are unspecified.
int step_read_bbox_l93(step_context_t* ctx, double bbox[4]);

// Parse the step's `params_json` (caller passes step_get_params(ctx)),
// extract "cell_size_m" as a positive double in (1e-6, 100.0), and
// return it. Returns `default_m` on any failure (params NULL,
// JSON parse error, cell_size_m absent, wrong type, or out-of-range).
double step_parse_cell_size_m(const char* params_json, double default_m);

#ifdef __cplusplus
}
#endif
#endif
