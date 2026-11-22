// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Unit + integration tests for the B5a registration steps:
//   build-lidar-rasters, compute-ndsm, compute-building-mask.
//
// Synthetic geometry covers exact-known per-cell stats. End-to-end via the
// runner verifies the chain composes correctly through the artifact map.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/runner.h"
#include "v3/safe_alloc.h"
#include "v3/store.h"

#include "raster_format.h"
#include "registration-steps.h"

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

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        g_checks++;                                                                                \
        double _diff = fabs((double)(a) - (double)(b));                                            \
        if (_diff > (tol)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: |%.6g - %.6g| = %.6g > %.6g\n", __FILE__, __LINE__,       \
                    (double)(a), (double)(b), _diff, (double)(tol));                               \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ----------------------------------------------------------------------------
// raster_format unit tests
// ----------------------------------------------------------------------------

static void test_raster_alloc_open_roundtrip(void) {
    uint8_t* buf = raster_alloc(4, 5, 3);
    CHECK(buf != NULL);
    // Fill with deterministic values.
    for (uint32_t ch = 0; ch < 3; ch++) {
        float* p = raster_channel_ptr(buf, 4, 5, ch);
        for (int i = 0; i < 20; i++) {
            p[i] = (float)(ch * 100 + i);
        }
    }
    size_t total = raster_total_bytes(4, 5, 3);
    raster_view_t v;
    CHECK(raster_view_open(buf, total, &v) == 0);
    CHECK(v.cols == 4);
    CHECK(v.rows == 5);
    CHECK(v.n_channels == 3);
    for (uint32_t ch = 0; ch < 3; ch++) {
        for (int i = 0; i < 20; i++) {
            CHECK(v.channels[ch][i] == (float)(ch * 100 + i));
        }
    }
    free(buf);
}

static void test_raster_view_rejects_bad(void) {
    raster_view_t v;
    uint8_t junk[4] = {0};
    CHECK(raster_view_open(junk, 4, &v) == -1); // too short
    uint8_t* buf = raster_alloc(2, 2, 1);
    CHECK(raster_view_open(buf, raster_total_bytes(2, 2, 1) - 1, &v) == -1); // truncated
    CHECK(raster_view_open(buf, raster_total_bytes(2, 2, 1) + 1, &v) == -1); // too long
    free(buf);
}

// Regression for Frama-C audit finding REG-1: malicious (cols, rows, n_channels)
// headers that overflow `cols * rows * n_channels * sizeof(float)` must be
// rejected. Before the fix, a 12-byte buffer with these values would slip
// past the `len == raster_total_bytes(...)` consistency check because both
// sides of the comparison overflow to the same small value, then the
// per-channel pointer arithmetic would index anywhere in memory.
static void test_raster_view_rejects_overflow_dimensions(void) {
    raster_view_t v;
    // 12-byte header + body. Pick (cols, rows, n_channels) so that the size_t
    // multiplication wraps. UINT32_MAX * UINT32_MAX overflows on 64-bit
    // when promoted to size_t (since size_t is u64 → max representable is
    // u64_max; the product is ~2^64 → wraps to a small value).
    uint8_t hdr[12];
    hdr[0]  = 0xff; hdr[1]  = 0xff; hdr[2]  = 0xff; hdr[3]  = 0xff; // cols = UINT32_MAX
    hdr[4]  = 0xff; hdr[5]  = 0xff; hdr[6]  = 0xff; hdr[7]  = 0xff; // rows = UINT32_MAX
    hdr[8]  = 16;   hdr[9]  = 0;    hdr[10] = 0;    hdr[11] = 0;    // n_channels = 16
    // The buffer length doesn't matter — the overflow check should fire first.
    CHECK(raster_view_open(hdr, sizeof(hdr), &v) == -1);

    // Also test: cells (cols*rows) overflows even with n_channels = 1.
    hdr[8] = 1;
    CHECK(raster_view_open(hdr, sizeof(hdr), &v) == -1);

    // Ridge case: cells = SIZE_MAX exactly should also be rejected because
    // RASTER_HEADER_BYTES + cells * sizeof(float) overflows.
    hdr[0]  = 0x00; hdr[1]  = 0x00; hdr[2]  = 0x00; hdr[3]  = 0x80; // cols = 2^31
    hdr[4]  = 0x00; hdr[5]  = 0x00; hdr[6]  = 0x00; hdr[7]  = 0x80; // rows = 2^31
    hdr[8]  = 1;                                                     // n_channels = 1
    CHECK(raster_view_open(hdr, sizeof(hdr), &v) == -1);
}

// ----------------------------------------------------------------------------
// End-to-end via the runner
// ----------------------------------------------------------------------------

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_registration_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        exit(1);
    }
}

// "load-fixture": single-step that writes a fixed set of artifacts the
// downstream registration steps consume.
static const double* g_positions = NULL;
static size_t g_n_points = 0;
static const uint16_t* g_intensities = NULL;
static const uint8_t* g_num_returns = NULL;
static const char* g_canonical_json = NULL;

static int load_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "lidar-positions", g_positions,
                          g_n_points * 3 * sizeof(double)) != 0) {
        return -1;
    }
    if (step_write_output(ctx, "lidar-intensities", g_intensities,
                          g_n_points * sizeof(uint16_t)) != 0) {
        return -1;
    }
    if (step_write_output(ctx, "lidar-num-returns", g_num_returns,
                          g_n_points * sizeof(uint8_t)) != 0) {
        return -1;
    }
    if (step_write_output(ctx, "canonical-location", g_canonical_json,
                          strlen(g_canonical_json)) != 0) {
        return -1;
    }
    return 0;
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "lidar-positions", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-intensities", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-num-returns", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 4,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

static void register_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_DEF) == 0);
    CHECK(registration_register_steps() == 0);
}

typedef struct cap {
    hash_t hashes[8];
    const char* names[8];
    int n;
} cap_t;
static void cap_artifact(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    cap_t* c = ctx;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->names[i], name) == 0) {
            c->hashes[i] = *h;
            return;
        }
    }
}

// Run a 4-step pipeline (load + 3 registration steps) and capture the named
// artifact hashes. Returns store + cap so caller can inspect the rasters.
typedef struct fixture_run {
    char root[256];
    store_t* store;
    intent_map_t* im;
    runner_t* runner;
    cap_t cap;
} fixture_run_t;

static int run_chain(fixture_run_t* f, const char* params_brl, const char* params_ndsm) {
    make_tmpdir(f->root, sizeof(f->root));
    f->store = store_open(&(store_config_t){.root_dir = f->root,
                                              .hot_budget_bytes = 16 * 1024 * 1024});
    f->im = open_test_im(f->root);
    f->runner = runner_new(&(runner_config_t){.store = f->store, .intent_map = f->im});

    pipeline_t* p = pipeline_new("reg-test");
    pipeline_add_step(p, "load-fixture", NULL);
    pipeline_add_step(p, "build-lidar-rasters", params_brl);
    pipeline_add_step(p, "compute-ndsm", params_ndsm);
    pipeline_add_step(p, "compute-building-mask", NULL);

    f->cap.n = 3;
    f->cap.names[0] = "lidar-rasters";
    f->cap.names[1] = "lidar-ndsm";
    f->cap.names[2] = "lidar-building-mask";
    runner_callbacks_t cb = {.ctx = &f->cap, .on_artifact_ready = cap_artifact};

    int dummy = 0;
    const char* subs[] = {"lidar-building-mask"};
    int rc = runner_run(f->runner, p, &dummy, sizeof(dummy), subs, 1, &cb);
    pipeline_free(p);
    return rc;
}

static void cleanup_run(fixture_run_t* f) {
    runner_free(f->runner);
    intent_map_close(f->im);
    store_close(f->store);
    rm_rf(f->root);
}

// ----------------------------------------------------------------------------
// Synthetic geometry tests
// ----------------------------------------------------------------------------

static void test_horizontal_grid_rasters(void) {
    register_test_steps();

    // 20×20 grid of points at z=10, spaced 1 m. bbox = (0,0,20,20), cell=1m.
    // Expected: each cell gets exactly 1 point, maxZ=minZ=meanZ=10, density=1,
    // intensity=42, roughness=0, multiReturn=0.
    const int side = 20;
    const size_t N = side * side;
    double* pos = malloc(N * 3 * sizeof(double));
    uint16_t* intens = safe_malloc_array(N, sizeof(uint16_t));
    uint8_t* nr = safe_malloc_array(N, sizeof(uint8_t));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            pos[i * 3 + 0] = x + 0.5; // centre of cell
            pos[i * 3 + 1] = y + 0.5;
            pos[i * 3 + 2] = 10.0;
            intens[i] = 42;
            nr[i] = 1;
            i++;
        }
    }
    g_positions = pos; g_n_points = N; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,20,20]}";

    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":1.0}", NULL) == 0);

    // Pull back the lidar-rasters artifact.
    store_handle_t h;
    CHECK(store_lookup(f.store, &f.cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == 20 && v.rows == 20);
    CHECK(v.n_channels == LIDAR_RC__COUNT);

    for (size_t k = 0; k < (size_t)v.cols * v.rows; k++) {
        CHECK(v.channels[LIDAR_RC_DENSITY][k] == 1.0f);
        CHECK_NEAR(v.channels[LIDAR_RC_MAX_Z][k], 10.0f, 1e-5);
        CHECK_NEAR(v.channels[LIDAR_RC_MIN_Z][k], 10.0f, 1e-5);
        CHECK_NEAR(v.channels[LIDAR_RC_MEAN_Z][k], 10.0f, 1e-5);
        CHECK_NEAR(v.channels[LIDAR_RC_INTENSITY][k], 42.0f, 1e-5);
        CHECK(v.channels[LIDAR_RC_ROUGHNESS][k] == 0.0f);
        CHECK(v.channels[LIDAR_RC_MULTI_RETURN][k] == 0.0f);
    }
    store_release(f.store, &h);
    free(pos); free(intens); free(nr);
    cleanup_run(&f);
}

static void test_empty_cells_get_nan(void) {
    register_test_steps();

    // 2 points clustered in one corner; the other 99 cells of a 10×10 grid
    // get no point, must end up NaN (except density which is 0).
    double pos[6] = {0.5, 0.5, 5.0, 0.6, 0.6, 5.0};
    uint16_t intens[2] = {100, 200};
    uint8_t nr[2] = {1, 1};
    g_positions = pos; g_n_points = 2; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,10,10]}";

    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":1.0}", NULL) == 0);
    store_handle_t h;
    CHECK(store_lookup(f.store, &f.cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == 10 && v.rows == 10);

    // Cell (0,0) populated.
    size_t k0 = 0;
    CHECK(v.channels[LIDAR_RC_DENSITY][k0] == 2.0f);
    CHECK_NEAR(v.channels[LIDAR_RC_MEAN_Z][k0], 5.0f, 1e-5);

    // Other cells: density=0, all other channels NaN.
    size_t empty_count = 0;
    for (size_t k = 1; k < 100; k++) {
        if (v.channels[LIDAR_RC_DENSITY][k] == 0.0f &&
            isnan(v.channels[LIDAR_RC_MAX_Z][k]) &&
            isnan(v.channels[LIDAR_RC_MEAN_Z][k]) &&
            isnan(v.channels[LIDAR_RC_INTENSITY][k]) &&
            isnan(v.channels[LIDAR_RC_ROUGHNESS][k]) &&
            isnan(v.channels[LIDAR_RC_MULTI_RETURN][k])) {
            empty_count++;
        }
    }
    CHECK(empty_count == 99);
    store_release(f.store, &h);
    cleanup_run(&f);
}

static void test_roughness_under_z_jitter(void) {
    register_test_steps();
    // Single cell, 100 points at known z values → roughness = std-dev.
    const int N = 100;
    double pos[300];
    uint16_t intens[100] = {0};
    uint8_t nr[100] = {0};
    for (int i = 0; i < N; i++) {
        pos[i * 3 + 0] = 0.5;
        pos[i * 3 + 1] = 0.5;
        pos[i * 3 + 2] = (i % 2 == 0) ? 9.0 : 11.0; // alternating; std = 1
        intens[i] = 0;
        nr[i] = 1;
    }
    g_positions = pos; g_n_points = N; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,2,2]}";
    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":2.0}", NULL) == 0);
    store_handle_t h;
    store_lookup(f.store, &f.cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    CHECK_NEAR(v.channels[LIDAR_RC_ROUGHNESS][0], 1.0f, 1e-3);
    CHECK_NEAR(v.channels[LIDAR_RC_MAX_Z][0], 11.0f, 1e-5);
    CHECK_NEAR(v.channels[LIDAR_RC_MIN_Z][0], 9.0f, 1e-5);
    CHECK_NEAR(v.channels[LIDAR_RC_MEAN_Z][0], 10.0f, 1e-5);
    store_release(f.store, &h);
    cleanup_run(&f);
}

static void test_multi_return_fraction(void) {
    register_test_steps();
    // 4 points in one cell; 3 of them have num_returns > 1 → multiReturn = 0.75.
    double pos[12] = {0.5, 0.5, 1, 0.5, 0.5, 1, 0.5, 0.5, 1, 0.5, 0.5, 1};
    uint16_t intens[4] = {0, 0, 0, 0};
    uint8_t nr[4] = {2, 3, 1, 2};
    g_positions = pos; g_n_points = 4; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,2,2]}";
    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":2.0}", NULL) == 0);
    store_handle_t h;
    store_lookup(f.store, &f.cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    CHECK_NEAR(v.channels[LIDAR_RC_MULTI_RETURN][0], 0.75f, 1e-5);
    store_release(f.store, &h);
    cleanup_run(&f);
}

static void test_ndsm_step_function(void) {
    register_test_steps();
    // 10×10 grid, points at z=5 in a 5×5 bottom-left block, z=20 in the rest
    // (a 15 m "building"). Local-min window k=3 reaches across both regions
    // for boundary cells → NDSM should be ~0 in the ground area, ~15 in the
    // far interior of the building, and intermediate values along the seam.
    const int side = 10;
    const size_t N = side * side;
    double* pos = malloc(N * 3 * sizeof(double));
    uint16_t* intens = safe_malloc_array(N, sizeof(uint16_t));
    uint8_t* nr = safe_malloc_array(N, sizeof(uint8_t));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            pos[i * 3 + 0] = x + 0.5;
            pos[i * 3 + 1] = y + 0.5;
            pos[i * 3 + 2] = (x < 5 && y < 5) ? 5.0 : 20.0;
            intens[i] = 0;
            nr[i] = 1;
            i++;
        }
    }
    g_positions = pos; g_n_points = N; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,10,10]}";
    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":1.0}", "{\"local_min_window\":3}") == 0);

    store_handle_t h;
    store_lookup(f.store, &f.cap.hashes[1], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);

    // Far interior of the ground area (cell 0,0): self=5, local min=5 → ndsm=0.
    CHECK_NEAR(v.channels[0][0 * 10 + 0], 0.0f, 1e-5);
    // Far interior of the building (cell 9,9): self=20, local min=20 → ndsm=0.
    CHECK_NEAR(v.channels[0][9 * 10 + 9], 0.0f, 1e-5);
    // Building-side cell that can reach into the ground region (e.g. 5,5):
    // self=20, local min=5 → ndsm=15.
    CHECK_NEAR(v.channels[0][5 * 10 + 5], 15.0f, 1e-5);
    store_release(f.store, &h);
    free(pos); free(intens); free(nr);
    cleanup_run(&f);
}

static void test_building_mask_thresholds(void) {
    register_test_steps();
    // Same step-function geometry; but now feed roughness < 0.4 by keeping z
    // constant within each region (so per-cell roughness=0). Building region
    // should mask to 1; ground to 0.
    const int side = 10;
    const size_t N = side * side;
    double* pos = malloc(N * 3 * sizeof(double));
    uint16_t* intens = safe_malloc_array(N, sizeof(uint16_t));
    uint8_t* nr = safe_malloc_array(N, sizeof(uint8_t));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            pos[i * 3 + 0] = x + 0.5;
            pos[i * 3 + 1] = y + 0.5;
            pos[i * 3 + 2] = (x < 5 && y < 5) ? 5.0 : 20.0;
            intens[i] = 0;
            nr[i] = 1; // single-return → multiReturn=0 → satisfies < 0.15
            i++;
        }
    }
    g_positions = pos; g_n_points = N; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,10,10]}";
    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":1.0}", "{\"local_min_window\":3}") == 0);

    store_handle_t h;
    store_lookup(f.store, &f.cap.hashes[2], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);

    // Far interior of building (well away from ground edge): mask=1.
    CHECK(v.channels[0][9 * 10 + 9] == 0.0f); // ndsm=0 here, NOT building
    // 5,5 has ndsm=15 with roughness=0 and mr=0 → mask=1.
    CHECK(v.channels[0][5 * 10 + 5] == 1.0f);
    // 0,0 ground: ndsm=0 → mask=0.
    CHECK(v.channels[0][0] == 0.0f);
    store_release(f.store, &h);
    free(pos); free(intens); free(nr);
    cleanup_run(&f);
}

static void test_chain_caches_on_rerun(void) {
    register_test_steps();
    // Run twice on the same input; second run should be a full intent-map
    // hit (zero body executions). The runner_test suite already proves
    // this for the smoke pipeline; here we confirm the registration steps'
    // identities are stable across runs (no hidden non-determinism).
    const int side = 5;
    const size_t N = side * side;
    static double pos[75];
    static uint16_t intens[25] = {0};
    static uint8_t nr[25] = {1};
    for (size_t i = 0; i < 25; i++) { nr[i] = 1; }
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            pos[i * 3 + 0] = x + 0.5;
            pos[i * 3 + 1] = y + 0.5;
            pos[i * 3 + 2] = 7.0;
            i++;
        }
    }
    g_positions = pos; g_n_points = N; g_intensities = intens; g_num_returns = nr;
    g_canonical_json = "{\"bbox_l93\":[0,0,5,5]}";

    fixture_run_t f;
    CHECK(run_chain(&f, "{\"cell_size_m\":1.0}", NULL) == 0);
    hash_t first_brl = f.cap.hashes[0];

    // Re-run on the same intent map + store.
    pipeline_t* p2 = pipeline_new("reg-rerun");
    pipeline_add_step(p2, "load-fixture", NULL);
    pipeline_add_step(p2, "build-lidar-rasters", "{\"cell_size_m\":1.0}");
    pipeline_add_step(p2, "compute-ndsm", NULL);
    pipeline_add_step(p2, "compute-building-mask", NULL);
    cap_t cap2 = {.n = 3,
                  .names = {"lidar-rasters", "lidar-ndsm", "lidar-building-mask"}};
    runner_callbacks_t cb = {.ctx = &cap2, .on_artifact_ready = cap_artifact};
    int dummy = 0;
    const char* subs[] = {"lidar-building-mask"};
    int rc = runner_run(f.runner, p2, &dummy, sizeof(dummy), subs, 1, &cb);
    pipeline_free(p2);
    CHECK(rc == 0);

    // Same content hash → confirms determinism + cache convergence.
    CHECK(hash_eq(&first_brl, &cap2.hashes[0]));
    cleanup_run(&f);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_raster_alloc_open_roundtrip();
    test_raster_view_rejects_bad();
    test_raster_view_rejects_overflow_dimensions();
    test_horizontal_grid_rasters();
    test_empty_cells_get_nan();
    test_roughness_under_z_jitter();
    test_multi_return_fraction();
    test_ndsm_step_function();
    test_building_mask_thresholds();
    test_chain_caches_on_rerun();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
