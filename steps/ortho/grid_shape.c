// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "grid_shape.h"

#include <jansson.h>
#include <math.h>
#include <string.h>

int grid_shape_resolve(step_context_t* ctx, grid_shape_t* out) {
    if (!ctx || !out) {
        return -1;
    }
    size_t loc_len = 0;
    const char* loc = (const char*)step_read(ctx, "canonical-location", &loc_len);
    if (!loc) {
        return -1;
    }
    json_error_t je;
    json_t* j = json_loadb(loc, loc_len, 0, &je);
    if (!j) {
        return -1;
    }
    json_t* j_bbox = json_object_get(j, "bbox_l93");
    if (!json_is_array(j_bbox) || json_array_size(j_bbox) != 4) {
        json_decref(j);
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        out->bbox[i] = json_number_value(json_array_get(j_bbox, i));
    }
    json_decref(j);

    out->cell_size = 0.5;
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_cs = json_object_get(params, "cell_size_m");
            if (json_is_number(j_cs)) {
                double cand = json_number_value(j_cs);
                if (cand > 1e-6 && cand < 100.0) {
                    out->cell_size = cand;
                }
            }
            json_decref(params);
        }
    }

    double width_m = out->bbox[2] - out->bbox[0];
    double height_m = out->bbox[3] - out->bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) {
        return -1;
    }
    int64_t cols_i = (int64_t)floor(width_m / out->cell_size);
    int64_t rows_i = (int64_t)floor(height_m / out->cell_size);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) {
        return -1;
    }
    out->cols = (uint32_t)cols_i;
    out->rows = (uint32_t)rows_i;
    return 0;
}
