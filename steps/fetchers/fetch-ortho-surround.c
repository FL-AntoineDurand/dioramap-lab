// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-ortho-surround
// Reads:  canonical-location — JSON with center_l93_{x,y}
// Params: {"size_px": int, "radius_m": number, "layer": string}
//           defaults: size_px=1024, radius_m=600,
//                     layer="ORTHOIMAGERY.ORTHOPHOTOS"
// Writes: ortho-surround-jpeg — raw JPEG bytes (kind AK_IMAGE_JPEG).
//
// The "surround" ortho is a wider-radius context tile (default 600m vs the
// ~50-100m core tile that drives coregistration). It's used by the lab UI
// as a ground-texture backdrop so the core scene has context; it's never
// read by algorithmic steps. Sits in the DAG as a leaf that only fires
// when a subscriber asks for `ortho-surround-jpeg` — see PLAN-VIEWER-PORT
// §4.2 note on subscription-gated surround.
//
// URL construction differs from fetch-ortho-rgb: the bbox here is derived
// from `center_l93_{x,y} ± radius_m`, NOT from `bbox_l93` (which is the
// core's radius). Both step identities stay distinct because they read
// the same canonical-location but carry different params.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/fetch.h"
#include "v3/store.h"
#include "v3/json_int.h"

#include "fetchers.h"

extern fetch_client_t* fetchers_shared_client(void);

static int exec_fn(step_context_t* ctx) {
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, "canonical-location", &in_len);
    if (!in_bytes || in_len == 0)
        return -1;
    json_error_t je;
    json_t* input = json_loadb(in_bytes, in_len, 0, &je);
    if (!input)
        return -1;
    json_t* j_cx = json_object_get(input, "center_l93_x");
    json_t* j_cy = json_object_get(input, "center_l93_y");
    if (!json_is_number(j_cx) || !json_is_number(j_cy)) {
        json_decref(input);
        return -1;
    }
    double cx = json_number_value(j_cx);
    double cy = json_number_value(j_cy);
    json_decref(input);

    int size_px = 1024;
    double radius_m = 600.0;
    const char* layer = "ORTHOIMAGERY.ORTHOPHOTOS";
    char layer_buf[128] = {0};
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_size = json_object_get(params, "size_px");
            if (json_int_or_default(j_size, 1, 16384, "size_px", &size_px) != 0) {
                json_decref(params); return -1;
            }
            json_t* j_radius = json_object_get(params, "radius_m");
            if (json_is_number(j_radius)) {
                double v = json_number_value(j_radius);
                if (v > 0 && v < 100000) radius_m = v;
            }
            json_t* j_layer = json_object_get(params, "layer");
            if (json_is_string(j_layer)) {
                snprintf(layer_buf, sizeof(layer_buf), "%s", json_string_value(j_layer));
                layer = layer_buf;
            }
            json_decref(params);
        }
    }

    double bbox[4] = {cx - radius_m, cy - radius_m, cx + radius_m, cy + radius_m};
    char url[2048];
    int n = snprintf(url, sizeof(url),
                     "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0"
                     "&REQUEST=GetMap&LAYERS=%s&STYLES=&CRS=EPSG:2154"
                     "&BBOX=%.3f,%.3f,%.3f,%.3f&WIDTH=%d&HEIGHT=%d&FORMAT=image/jpeg",
                     layer, bbox[0], bbox[1], bbox[2], bbox[3], size_px, size_px);
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    fetch_client_t* client = fetchers_shared_client();
    if (!client)
        return -1;
    fetch_response_t resp;
    if (fetch_get(client, url, &resp) != 0) {
        free(resp.body);
        return -1;
    }
    int rc = step_write_output(ctx, "ortho-surround-jpeg", resp.body, resp.body_len);
    free(resp.body);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    {.name = "ortho-surround-jpeg",
     .kind = AK_IMAGE_JPEG,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fetch_ortho_surround_def = {
    .name = "fetch-ortho-surround",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
