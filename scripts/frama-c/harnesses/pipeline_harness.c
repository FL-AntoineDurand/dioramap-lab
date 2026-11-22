// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_pipeline`.
// Pipeline parses JSON via jansson (opaque). We focus on the public
// builder API and the validators.

#include "v3/pipeline.h"

#include <__fc_builtin.h>
#include <stddef.h>

int main(void) {
    // Builder path
    pipeline_t* p = pipeline_new("test");
    if (p) {
        (void)pipeline_add_step(p, "s1", "{}");
        (void)pipeline_add_step(p, "s2", NULL);
        (void)pipeline_add_step(p, NULL, "{}");           // NULL name
        (void)pipeline_add_step(NULL, "s3", "{}");        // NULL pipeline
        (void)pipeline_add_attribution(p, "a", "u");
        (void)pipeline_add_attribution(p, NULL, NULL);

        (void)pipeline_validate(p);
        (void)pipeline_name(p);
        (void)pipeline_step_count(p);

        for (size_t i = 0; i < pipeline_step_count(p); i++) {
            (void)pipeline_step_def_at(p, i);
            (void)pipeline_step_params_at(p, i);
        }
        (void)pipeline_step_def_at(p, 999); // out-of-range
        (void)pipeline_step_params_at(p, 999);

        for (size_t i = 0; i < pipeline_attribution_count(p); i++) {
            (void)pipeline_attribution_at(p, i);
        }
        pipeline_free(p);
    }

    // JSON loader paths
    char err[256];
    pipeline_t* p2 = pipeline_load_json_string("{\"name\":\"x\",\"steps\":[]}", 23, err, sizeof(err));
    if (p2) pipeline_free(p2);

    // Malformed
    p2 = pipeline_load_json_string("not json", 8, err, sizeof(err));
    if (p2) pipeline_free(p2);

    // NULL inputs
    p2 = pipeline_load_json_string(NULL, 0, err, sizeof(err));
    if (p2) pipeline_free(p2);
    p2 = pipeline_load_json_string("{}", 2, NULL, 0);
    if (p2) pipeline_free(p2);

    // File path — opaque
    p2 = pipeline_load_json_file("/tmp/nope.json", err, sizeof(err));
    if (p2) pipeline_free(p2);

    pipeline_free(NULL);
    return 0;
}
