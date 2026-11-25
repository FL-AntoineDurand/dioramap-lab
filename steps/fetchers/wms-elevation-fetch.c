// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// wms-elevation-fetch.c — see header for rationale.
#include "wms-elevation-fetch.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/fetch.h"
#include "v3/json_int.h"

#include "fetchers.h"

extern fetch_client_t* fetchers_shared_client(void);

int wms_elevation_fetch_exec(step_context_t* ctx,
                             const double bbox[4],
                             int default_grid_size,
                             const char* default_layer,
                             const char* output_artifact)
{
    int grid_size = default_grid_size;
    const char* layer = default_layer;
    char layer_buf[128] = {0};
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_error_t je;
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_grid = json_object_get(params, "grid_size");
            if (json_int_or_default(j_grid, 1, 16384, "grid_size", &grid_size) != 0) {
                json_decref(params);
                return step_fail(ctx, V3_ERR_INVALID_INPUT, "param grid_size must be an integer in [1, 16384]");
            }
            json_t* j_layer = json_object_get(params, "layer");
            if (json_is_string(j_layer)) {
                snprintf(layer_buf, sizeof(layer_buf), "%s", json_string_value(j_layer));
                layer = layer_buf;
            }
            json_decref(params);
        }
    }

    char url[2048];
    int n = snprintf(url, sizeof(url),
                     "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0"
                     "&REQUEST=GetMap&LAYERS=%s&STYLES=&CRS=EPSG:2154"
                     "&BBOX=%.3f,%.3f,%.3f,%.3f&WIDTH=%d&HEIGHT=%d"
                     "&FORMAT=image/x-bil;bits=32",
                     layer, bbox[0], bbox[1], bbox[2], bbox[3], grid_size, grid_size);
    if (n < 0 || (size_t)n >= sizeof(url))
        return step_fail(ctx, V3_ERR_INTERNAL, "request URL does not fit in %zu bytes", sizeof(url));

    fetch_client_t* client = fetchers_shared_client();
    if (!client)
        return step_fail(ctx, V3_ERR_INTERNAL, "HTTP client not initialised");
    fetch_response_t resp;
    if (fetch_get(client, url, &resp) != 0) {
        free(resp.body);
        return step_fail(ctx, V3_ERR_UPSTREAM_HTTP, "IGN elevation WMS request (layer %s) failed",
                         layer);
    }
    // The IGN WMS-BIL response is raw F32 grid_size × grid_size cells.
    // Wrap in the v3 raster_format header (u32 cols, u32 rows, u32
    // n_channels=1) so consumers can decode as AK_RASTER_F32 without
    // a special case. Expected payload size: grid_size² × 4 bytes.
    const size_t expected_bil = (size_t)grid_size * (size_t)grid_size * 4;
    if (resp.body_len != expected_bil) {
        char what[192], expected[96];
        snprintf(what, sizeof(what), "IGN elevation WMS (layer %s)", layer);
        snprintf(expected, sizeof(expected), "%zu bytes of float32 BIL (%dx%d)", expected_bil,
                 grid_size, grid_size);
        int frc = fetchers_fail_bad_body(ctx, what, url, resp.body, resp.body_len, expected);
        free(resp.body);
        return frc;
    }
    const size_t out_len = 12 + resp.body_len;
    uint8_t* out = malloc(out_len);
    if (!out) {
        free(resp.body);
        return step_fail(ctx, V3_ERR_INTERNAL, "out of memory");
    }
    const uint32_t cols_u32 = (uint32_t)grid_size;
    const uint32_t rows_u32 = (uint32_t)grid_size;
    const uint32_t n_ch_u32 = 1;
    memcpy(out + 0, &cols_u32, 4);
    memcpy(out + 4, &rows_u32, 4);
    memcpy(out + 8, &n_ch_u32, 4);
    memcpy(out + 12, resp.body, resp.body_len);
    int rc = step_write_output_owned(ctx, output_artifact, out, out_len);
    free(resp.body);
    return rc;
}
