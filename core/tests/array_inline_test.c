// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Round-trip test for the ARRAY_INLINE layout (Phase 5.1d).
//
// Writes a few records via `step_write_array_inline`, reads them back via
// `step_array_read_bytes`, asserts:
//   1. Item count + per-record bytes round-trip identically.
//   2. Each variable-length record's payload pointer is 8-byte aligned —
//      the bug 5.1d fixes. A pointer-cast read of a `double` field
//      through the borrowed buffer must not trap on UBSan.
//
// This is the test the original v7-tile-signals UBSan crash would have
// surfaced on first run, had it existed.

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/runner.h"
#include "v3/store.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ── Test step: writes a 3-record variable-length ARRAY_INLINE ────────

// Each record is `[u32 ne][padding to 8][f64 vals[ne]][optional trailing
// f32]`. Sizes are chosen so at least one record's TOTAL length is NOT a
// multiple of 8 — that's the case the runner's trailing-pad logic exists
// for, and it must fire on at least one record for the test to be meaningful.
//   record 0: 4 + 4 + 3*8           = 32  (8-aligned, no trailing pad)
//   record 1: 4 + 4 + 5*8 + 4       = 52  (4 trailing bytes pad needed)
//   record 2: 4 + 4 + 1*8           = 16  (8-aligned)
// The 4-byte trailer in record 1 is a `float` — its alignment is 4, but
// the runner doesn't care; it just rounds the record's total length up to
// 8 so the next record's header lands aligned.

static int writer_exec(step_context_t* ctx) {
    enum { N_REC = 3 };
    static const size_t DOUBLES[N_REC] = {3, 5, 1};
    static const int    HAS_FLOAT[N_REC] = {0, 1, 0}; // record 1 carries a trailing f32

    // Build the input blob via the shared blob_push helper — same
    // framing the runner expects (`{u32 rec_len, u32 pad, payload,
    // pad-to-8}` per record). Record 1 deliberately has a non-8-multiple
    // payload length so the trailing-pad path is exercised.
    blob_t b = {0};
    int rc = 0;
    for (size_t k = 0; k < N_REC && rc == 0; k++) {
        size_t payload_len = 8 /* per-record header */
                             + DOUBLES[k] * sizeof(double)
                             + (HAS_FLOAT[k] ? sizeof(float) : 0);
        uint8_t* rec = (uint8_t*)calloc(1, payload_len);
        if (!rec) { rc = -1; break; }
        uint32_t n_elems = (uint32_t)DOUBLES[k];
        memcpy(rec + 0, &n_elems, 4);
        // rec[4..8) is the inner per-record header pad.
        size_t cur = 8;
        for (size_t j = 0; j < DOUBLES[k]; j++) {
            double v = (double)(k * 100 + j) + 0.5;
            memcpy(rec + cur, &v, sizeof(double));
            cur += sizeof(double);
        }
        if (HAS_FLOAT[k]) {
            float f = (float)(k * 1000) + 0.25f;
            memcpy(rec + cur, &f, sizeof(float));
        }
        rc = blob_push(&b, rec, payload_len);
        free(rec);
    }
    if (rc == 0) rc = step_write_array_inline(ctx, "test-array",
                                              b.bytes, b.len, b.n_records, 0);
    blob_free(&b);
    return rc;
}

static const step_read_t writer_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writer_writes[] = {
    {.name = "test-array", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG,
     .array = true},
};
static const step_def_t WRITER_DEF = {
    .name = "test-array-writer",
    .version = 1,
    .reads = writer_reads,
    .n_reads = 1,
    .writes = writer_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = writer_exec,
};

// ── Test step: reads the array back, asserts alignment + round-trip ──

static int reader_exec(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "test-array");
    if (n != 3) return -1;

    const uintptr_t expected_doubles[3] = {3, 5, 1};
    const int       expected_f32[3]     = {0, 1, 0};
    for (size_t k = 0; k < n; k++) {
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "test-array", k, &rec, &rec_len) != 0)
            return -1;
        // Alignment assertion — the whole point of 5.1d. Both the record
        // payload pointer AND the implied next-record header offset must
        // be 8-aligned.
        if (((uintptr_t)rec & 7u) != 0) {
            fprintf(stderr, "record %zu payload misaligned: %p\n", k, (const void*)rec);
            return -1;
        }
        uint32_t n_elems;
        memcpy(&n_elems, rec + 0, 4);
        if (n_elems != expected_doubles[k]) return -1;
        size_t expected_len = 8 + n_elems * sizeof(double)
                              + (expected_f32[k] ? sizeof(float) : 0);
        if (rec_len != expected_len) {
            fprintf(stderr, "record %zu rec_len=%zu expected=%zu\n",
                    k, rec_len, expected_len);
            return -1;
        }

        // Pointer-cast read of the doubles — the read that would have
        // UBSan-trapped pre-5.1d.
        const double* vals = (const double*)(rec + 8);
        for (uint32_t j = 0; j < n_elems; j++) {
            double expect = (double)(k * 100 + j) + 0.5;
            if (vals[j] != expect) {
                fprintf(stderr, "record %zu val[%u]: got %g expected %g\n",
                        k, j, vals[j], expect);
                return -1;
            }
        }
        if (expected_f32[k]) {
            float fv;
            memcpy(&fv, rec + 8 + n_elems * sizeof(double), sizeof(float));
            float expect = (float)(k * 1000) + 0.25f;
            if (fv != expect) {
                fprintf(stderr, "record %zu trailing f32: got %g expected %g\n",
                        k, (double)fv, (double)expect);
                return -1;
            }
        }
    }
    return 0;
}

static const step_read_t reader_reads[] = {{.name = "test-array", .array = true}};
static const step_def_t READER_DEF = {
    .name = "test-array-reader",
    .version = 1,
    .reads = reader_reads,
    .n_reads = 1,
    .writes = NULL,
    .n_writes = 0,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = reader_exec,
};

// ── Driver ───────────────────────────────────────────────────────────

static void rm_rf(const char* p) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
    (void)!system(cmd);
}

static void run_round_trip(void) {
    char root[256] = "/tmp/dioramap_arrinline_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "mkdtemp failed\n");
        g_failed++;
        return;
    }
    store_t* store = store_open(&(store_config_t){
        .root_dir = root, .hot_budget_bytes = 16 * 1024 * 1024});
    char im_path[512];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", root);
    intent_map_t* im = intent_map_open(im_path);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    step_registry_reset();
    CHECK(step_register(&WRITER_DEF) == 0);
    CHECK(step_register(&READER_DEF) == 0);

    pipeline_t* p = pipeline_new("array-inline-roundtrip");
    pipeline_add_step(p, "test-array-writer", NULL);
    pipeline_add_step(p, "test-array-reader", NULL);

    int dummy = 0;
    const char* subs[] = {"test-array"};
    int rc = runner_run(r, p, &dummy, sizeof(dummy), subs, 1, NULL);
    CHECK(rc == 0);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
}

int main(void) {
    run_round_trip();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
