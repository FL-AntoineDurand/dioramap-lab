// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step_bbox_params.c — see header for rationale.
#include "v3/step_bbox_params.h"

#include <jansson.h>


int step_read_bbox_l93(step_context_t* ctx, double bbox[4]) {
    size_t len = 0;
    const char* bytes = (const char*)step_read(ctx, "canonical-location", &len);
    if (!bytes) return -1;
    json_error_t je;
    json_t* j = json_loadb(bytes, len, 0, &je);
    if (!j) return -1;
    json_t* jb = json_object_get(j, "bbox_l93");
    if (!json_is_array(jb) || json_array_size(jb) != 4) {
        json_decref(j);
        return -1;
    }
    for (int i = 0; i < 4; i++) bbox[i] = json_number_value(json_array_get(jb, i));
    json_decref(j);
    return 0;
}

double step_parse_cell_size_m(const char* params_json, double default_m) {
    if (!params_json) return default_m;
    json_error_t je;
    json_t* p = json_loads(params_json, 0, &je);
    if (!p) return default_m;
    double v = default_m;
    json_t* j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double cand = json_number_value(j);
        if (cand > 1e-6 && cand < 100.0) v = cand;
    }
    json_decref(p);
    return v;
}
