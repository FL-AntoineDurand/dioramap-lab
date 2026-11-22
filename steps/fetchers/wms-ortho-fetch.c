// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// wms-ortho-fetch.c — see header for rationale.
#include "wms-ortho-fetch.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/fetch.h"
#include "v3/json_int.h"
#include "v3/step_bbox_params.h"

#include "fetchers.h"

extern fetch_client_t* fetchers_shared_client(void);

int wms_ortho_fetch_exec(step_context_t* ctx,
                         const char* default_layer,
                         const char* output_artifact)
{
    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;

    int size_px = 2048;
    const char* layer = default_layer;
    char layer_buf[128] = {0};
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_error_t je;
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_size = json_object_get(params, "size_px");
            if (json_int_or_default(j_size, 1, 16384, "size_px", &size_px) != 0) {
                json_decref(params); return -1;
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
                     "&BBOX=%.3f,%.3f,%.3f,%.3f&WIDTH=%d&HEIGHT=%d&FORMAT=image/jpeg",
                     layer, bbox[0], bbox[1], bbox[2], bbox[3], size_px, size_px);
    if (n < 0 || (size_t)n >= sizeof(url)) return -1;

    fetch_client_t* client = fetchers_shared_client();
    if (!client) return -1;
    fetch_response_t resp;
    if (fetch_get(client, url, &resp) != 0) {
        free(resp.body);
        return -1;
    }
    int rc = step_write_output(ctx, output_artifact, resp.body, resp.body_len);
    free(resp.body);
    return rc;
}
