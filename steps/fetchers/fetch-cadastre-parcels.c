// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-cadastre-parcels
// Reads:  canonical-location — JSON with bbox_l93
// Params: { "max_count": int (default 2000) }
// Writes: cadastre-parcels-geojson — raw GeoJSON bytes (AK_OPAQUE_JSON).
//
// IGN WFS → FeatureCollection of polygon/multipolygon geometries in
// EPSG:2154 (Lambert-93 metres). Downstream steps parse the bytes
// themselves; the fetcher stays format-agnostic.

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/fetch.h"
#include "v3/log.h"
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
    json_t* j_bbox = json_object_get(input, "bbox_l93");
    if (!json_is_array(j_bbox) || json_array_size(j_bbox) != 4) {
        json_decref(input);
        return -1;
    }
    double bbox[4];
    for (size_t i = 0; i < 4; i++) {
        bbox[i] = json_number_value(json_array_get(j_bbox, i));
    }
    json_decref(input);

    int max_count = 2000;
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j = json_object_get(params, "max_count");
            if (json_int_or_default(j, 1, 100000, "max_count", &max_count) != 0) {
                json_decref(params); return -1;
            }
            json_decref(params);
        }
    }

    char url[2048];
    int n = snprintf(url, sizeof(url),
                     "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0"
                     "&REQUEST=GetFeature"
                     // IGN retired CADASTRALPARCELS.PARCELLAIRE_EXPRESS in
                     // 2025; cadastral parcels now live under the BDPARCELLAIRE
                     // namespace. The old name returned HTTP 400 "Feature type
                     // unknown" for every request after the migration.
                     "&TYPENAMES=BDPARCELLAIRE-VECTEUR_WLD_BDD_WGS84G:parcelle"
                     "&COUNT=%d&BBOX=%.3f,%.3f,%.3f,%.3f,EPSG:2154"
                     "&SRSNAME=EPSG:2154&OUTPUTFORMAT=application/json",
                     max_count, bbox[0], bbox[1], bbox[2], bbox[3]);
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    fetch_client_t* client = fetchers_shared_client();
    if (!client)
        return -1;
    fetch_response_t resp;
    if (fetch_get(client, url, &resp) != 0) {
        // Log the upstream failure with enough context to debug from logs
        // alone — without this the runner only emits a bare step_failed,
        // see commit history for the IGN layer-rename surprise.
        char snippet[257] = {0};
        if (resp.body && resp.body_len > 0) {
            size_t copy_len = resp.body_len < sizeof(snippet) - 1
                              ? resp.body_len
                              : sizeof(snippet) - 1;
            memcpy(snippet, resp.body, copy_len);
        }
        LOG_EVENT(SUB_FETCH, LOG_ERROR, EV_FETCH_FAILED,
                  LF_STEP("fetch-cadastre-parcels"),
                  LF_STR("url", url),
                  LF_UINT("status", resp.status_code),
                  LF_INT("error_code", (int)resp.error),
                  LF_STR("body_snippet", snippet));
        free(resp.body);
        return -1;
    }
    int rc = step_write_output(ctx, "cadastre-parcels-geojson", resp.body, resp.body_len);
    free(resp.body);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    {.name = "cadastre-parcels-geojson", .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fetch_cadastre_parcels_def = {
    .name = "fetch-cadastre-parcels",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
