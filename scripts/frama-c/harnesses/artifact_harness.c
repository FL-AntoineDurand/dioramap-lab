// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_artifact`.
//
// Exercises both write side (builder) and read side (view).
// av_open's bounds-check loop is the main thing we want RTE to validate.

#include "v3/artifact.h"
#include "v3/hash.h"

#include <__fc_builtin.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SZ 1024

int main(void) {
    // ---- writer side ----
    artifact_builder_t* ab = ab_new(AK_POINTCLOUD_SOA, 100);
    if (!ab) return 0;

    uint8_t col_data[256];
    Frama_C_make_unknown((char*)col_data, sizeof(col_data));

    (void)ab_add_column(ab, "x", DT_F32, col_data, sizeof(col_data));
    (void)ab_add_column(ab, "y", DT_F32, col_data, sizeof(col_data));
    (void)ab_add_column(ab, NULL, DT_U8, col_data, sizeof(col_data));  // NULL name
    (void)ab_set_metadata_json(ab, "{\"k\":1}", 7);
    (void)ab_set_metadata_json(ab, NULL, 0);                            // NULL json

    uint8_t* out_bytes = NULL;
    size_t out_len = 0;
    hash_t out_hash;
    int rc = ab_finalize(ab, &out_bytes, &out_len, &out_hash);
    ab_free(ab);

    // ---- reader side ----
    if (rc == 0 && out_bytes && out_len > 0) {
        artifact_view_t v;
        if (av_open(out_bytes, out_len, &v) == 0) {
            size_t b;
            (void)av_column(&v, "x", &b);
            (void)av_column(&v, "nope", &b);
            (void)av_column_dtype(&v, "y");
            (void)av_metadata_json(&v, &b);
            (void)av_hash(&v);
        }
        free(out_bytes);
    }

    // ---- corrupt-input reader ----
    uint8_t fuzz[BUF_SZ];
    Frama_C_make_unknown((char*)fuzz, BUF_SZ);
    size_t len = Frama_C_unsigned_int_interval(0, BUF_SZ);
    artifact_view_t v2;
    (void)av_open(fuzz, len, &v2);

    // helpers
    (void)artifact_kind_name(AK_POINTCLOUD_SOA);
    (void)dtype_size(DT_F32);
    (void)dtype_name(DT_F64);
    (void)artifact_column_name_hash("test");
    (void)artifact_column_name_hash(NULL);
    return 0;
}
