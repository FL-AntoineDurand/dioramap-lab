// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tests for B6 classification steps:
//   build-tile-ndvi, compute-normal-scatter, compute-near-building-mask,
//   classify-vegetation.
//
// Synthetic scenarios verify each step's core behaviour plus the end-to-end
// chain on a canopy-like configuration (horizontal normals + high points +
// elevated NDVI → canopy flags).

#define _POSIX_C_SOURCE 200809L

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "classification-steps.h"
#include "raster_format.h"

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
// Helpers
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_classification_test_XXXXXX");
    if (mkdtemp(out) == NULL) exit(1);
}

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

// Encode a solid-colour RGB JPEG in memory.
static void encode_solid_jpeg(uint8_t R, uint8_t G, uint8_t B, int w, int h, uint8_t** out_bytes,
                              size_t* out_len) {
    uint8_t* rgb = malloc(w * h * 3);
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = R;
        rgb[i * 3 + 1] = G;
        rgb[i * 3 + 2] = B;
    }
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* mem = NULL;
    unsigned long mem_len = 0;
    jpeg_mem_dest(&cinfo, &mem, &mem_len);
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    JSAMPROW row[1];
    for (int y = 0; y < h; y++) {
        row[0] = (JSAMPROW)(rgb + (size_t)y * w * 3);
        jpeg_write_scanlines(&cinfo, row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(rgb);
    *out_bytes = mem;
    *out_len = (size_t)mem_len;
}

// Fixture globals
static const char* g_canonical = "{\"bbox_l93\":[0,0,10,10]}";
static uint8_t* g_rgb_jpeg = NULL;
static size_t g_rgb_len = 0;
static uint8_t* g_irc_jpeg = NULL;
static size_t g_irc_len = 0;
static const double* g_positions = NULL;
static size_t g_n_points = 0;
static const float* g_normals = NULL;
static uint8_t* g_bmask_bytes = NULL;
static size_t g_bmask_len = 0;
static const char* g_geojson = NULL;
static size_t g_geojson_len = 0;
static const char* g_cadastre_fit_json = NULL;
static size_t g_cadastre_fit_len = 0;

static int load_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "canonical-location", g_canonical, strlen(g_canonical)) != 0)
        return -1;
    if (step_write_output(ctx, "ortho-rgb-jpeg", g_rgb_jpeg, g_rgb_len) != 0) return -1;
    if (step_write_output(ctx, "ortho-irc-jpeg", g_irc_jpeg, g_irc_len) != 0) return -1;
    if (step_write_output(ctx, "lidar-positions", g_positions,
                          g_n_points * 3 * sizeof(double)) != 0)
        return -1;
    if (step_write_output(ctx, "lidar-normals", g_normals,
                          g_n_points * 3 * sizeof(float)) != 0)
        return -1;
    if (step_write_output(ctx, "lidar-building-mask", g_bmask_bytes, g_bmask_len) != 0)
        return -1;
    if (step_write_output(ctx, "cadastre-buildings-geojson", g_geojson, g_geojson_len) != 0)
        return -1;
    return step_write_output(ctx, "cadastre-fit", g_cadastre_fit_json, g_cadastre_fit_len);
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-rgb-jpeg", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-irc-jpeg", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-positions", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-normals", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-building-mask", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-buildings-geojson", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-fit", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-classification-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 8,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

typedef struct cap {
    hash_t hashes[4];
    const char* names[4];
    int n;
} cap_t;
static void cap_cb(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    cap_t* c = ctx;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->names[i], name) == 0) {
            c->hashes[i] = *h;
            return;
        }
    }
}

static void register_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_DEF) == 0);
    CHECK(classification_register_steps() == 0);
}

// Build a solid-zero lidar building mask at 0.5m (for steps that need it).
static uint8_t* make_zero_mask_05m(void) {
    uint8_t* buf = raster_alloc(20, 20, 1);
    float* p = raster_channel_ptr(buf, 20, 20, 0);
    memset(p, 0, 20 * 20 * sizeof(float));
    return buf;
}

// ----------------------------------------------------------------------------
// build-tile-ndvi
// ----------------------------------------------------------------------------

static void test_tile_ndvi_vegetation(void) {
    register_test_steps();

    // RGB: solid green (0, 255, 0). IRC: solid (255, 0, 0) → NIR=255.
    // Raw NDVI = (1 - 0) / (1 + 0 + 1e-6) ≈ 1.0. Uniform → boost no-op.
    encode_solid_jpeg(0, 255, 0, 256, 256, &g_rgb_jpeg, &g_rgb_len);
    encode_solid_jpeg(255, 0, 0, 256, 256, &g_irc_jpeg, &g_irc_len);

    // Placeholder unused inputs.
    static const double unused_pos[3] = {0};
    static const float unused_nrm[3] = {0};
    g_positions = unused_pos;
    g_n_points = 1;
    g_normals = unused_nrm;
    g_bmask_bytes = make_zero_mask_05m();
    g_bmask_len = raster_total_bytes(20, 20, 1);
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_cadastre_fit_json = "{\"offsetX\":0,\"offsetY\":0}";
    g_cadastre_fit_len = strlen(g_cadastre_fit_json);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 16 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("ndvi-test");
    pipeline_add_step(p, "load-classification-fixture", NULL);
    pipeline_add_step(p, "build-tile-ndvi", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"tile-ndvi"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"tile-ndvi"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    CHECK(store_lookup(store, &cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == 10 && v.rows == 10 && v.n_channels == 1);
    // Ignore the 1-cell border (bilinear resample leaks a touch from
    // background). Interior cells should be ~1.0.
    for (uint32_t r2 = 1; r2 < v.rows - 1; r2++) {
        for (uint32_t c2 = 1; c2 < v.cols - 1; c2++) {
            CHECK_NEAR(v.channels[0][r2 * v.cols + c2], 1.0f, 0.05);
        }
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_rgb_jpeg); free(g_irc_jpeg);
    free(g_bmask_bytes);
    g_rgb_jpeg = NULL; g_irc_jpeg = NULL; g_bmask_bytes = NULL;
}

// ----------------------------------------------------------------------------
// compute-normal-scatter: flat plane vs jittered orientations
// ----------------------------------------------------------------------------

static void test_normal_scatter_flat(void) {
    register_test_steps();

    // 100 points in a single 1m cell, all with normal (0, 0, 1) → scatter ≈ 0.
    const int N = 100;
    static double pos[300];
    static float nrm[300];
    for (int i = 0; i < N; i++) {
        pos[i * 3 + 0] = 5.0 + 0.001 * i;
        pos[i * 3 + 1] = 5.0 + 0.0005 * i;
        pos[i * 3 + 2] = 10.0 + 0.01 * (i % 5);
        nrm[i * 3 + 0] = 0.0f;
        nrm[i * 3 + 1] = 0.0f;
        nrm[i * 3 + 2] = 1.0f;
    }
    g_positions = pos; g_n_points = N; g_normals = nrm;
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_rgb_jpeg, &g_rgb_len);
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_irc_jpeg, &g_irc_len);
    g_bmask_bytes = make_zero_mask_05m();
    g_bmask_len = raster_total_bytes(20, 20, 1);
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_cadastre_fit_json = "{\"offsetX\":0,\"offsetY\":0}";
    g_cadastre_fit_len = strlen(g_cadastre_fit_json);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 16 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("scatter-flat");
    pipeline_add_step(p, "load-classification-fixture", NULL);
    pipeline_add_step(p, "compute-normal-scatter", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"normal-scatter"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"normal-scatter"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    CHECK(v.n_channels == 2);
    // Cell at (5, 5) = index (5, 5) in the 10×10 grid. All 100 normals
    // agree perfectly → scatter = 0 degrees.
    size_t k = (size_t)5 * v.cols + 5;
    CHECK_NEAR(v.channels[0][k], 0.0f, 1e-4);
    // Min-Z = 10.0 (plus small jitter).
    CHECK_NEAR(v.channels[1][k], 10.0f, 0.1);
    // Empty cells: scatter 0, min_z = INFINITY.
    CHECK(v.channels[0][0] == 0.0f);
    CHECK(isinf(v.channels[1][0]));
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_rgb_jpeg); free(g_irc_jpeg);
    free(g_bmask_bytes);
    g_rgb_jpeg = NULL; g_irc_jpeg = NULL; g_bmask_bytes = NULL;
}

static void test_normal_scatter_jitter(void) {
    register_test_steps();

    // 100 points in one cell, normals uniformly sampled on a cone 20° off
    // vertical → mean normal ≈ (0, 0, 1); scatter should land around ~17°.
    const int N = 100;
    static double pos[300];
    static float nrm[300];
    double deg20 = 20.0 * 3.14159265358979323846 / 180.0;
    for (int i = 0; i < N; i++) {
        pos[i * 3 + 0] = 5.0 + 0.001 * i;
        pos[i * 3 + 1] = 5.0;
        pos[i * 3 + 2] = 10.0;
        double phi = 2.0 * 3.14159265358979323846 * (double)i / (double)N;
        nrm[i * 3 + 0] = (float)(sin(deg20) * cos(phi));
        nrm[i * 3 + 1] = (float)(sin(deg20) * sin(phi));
        nrm[i * 3 + 2] = (float)cos(deg20);
    }
    g_positions = pos; g_n_points = N; g_normals = nrm;
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_rgb_jpeg, &g_rgb_len);
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_irc_jpeg, &g_irc_len);
    g_bmask_bytes = make_zero_mask_05m();
    g_bmask_len = raster_total_bytes(20, 20, 1);
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_cadastre_fit_json = "{\"offsetX\":0,\"offsetY\":0}";
    g_cadastre_fit_len = strlen(g_cadastre_fit_json);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 16 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("scatter-cone");
    pipeline_add_step(p, "load-classification-fixture", NULL);
    pipeline_add_step(p, "compute-normal-scatter", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"normal-scatter"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"normal-scatter"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    size_t k = (size_t)5 * v.cols + 5;
    // All points are exactly 20° from the mean normal → scatter = 20°.
    CHECK_NEAR(v.channels[0][k], 20.0f, 1.0);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_rgb_jpeg); free(g_irc_jpeg);
    free(g_bmask_bytes);
    g_rgb_jpeg = NULL; g_irc_jpeg = NULL; g_bmask_bytes = NULL;
}

// ----------------------------------------------------------------------------
// compute-near-building-mask: lidar mask + dilation
// ----------------------------------------------------------------------------

static void test_near_building_dilation(void) {
    register_test_steps();

    // LiDAR building mask at 0.5m grid: single cell at (10, 10) set.
    uint8_t* bmask = raster_alloc(20, 20, 1);
    float* bm_p = raster_channel_ptr(bmask, 20, 20, 0);
    memset(bm_p, 0, 400 * sizeof(float));
    bm_p[10 * 20 + 10] = 1.0f; // centre of tile (corresponds to L93 (5, 5))

    g_bmask_bytes = bmask;
    g_bmask_len = raster_total_bytes(20, 20, 1);
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_cadastre_fit_json = "{\"offsetX\":0,\"offsetY\":0}";
    g_cadastre_fit_len = strlen(g_cadastre_fit_json);
    static const double unused_pos[3] = {0};
    static const float unused_nrm[3] = {0};
    g_positions = unused_pos; g_n_points = 1; g_normals = unused_nrm;
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_rgb_jpeg, &g_rgb_len);
    encode_solid_jpeg(128, 128, 128, 64, 64, &g_irc_jpeg, &g_irc_len);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 16 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("near-build-test");
    pipeline_add_step(p, "load-classification-fixture", NULL);
    pipeline_add_step(p, "compute-near-building-mask",
                      "{\"cell_size_m\":1.0,\"dilate_r\":3}");
    cap_t cap = {.n = 1, .names = {"lidar-near-building-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"lidar-near-building-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    CHECK(v.cols == 10 && v.rows == 10);
    // Centre cell (5, 5) and all cells within a (2R+1)² = 7×7 window must be 1.
    size_t on_count = 0;
    for (size_t i = 0; i < 100; i++) {
        if (v.channels[0][i] == 1.0f) on_count++;
    }
    // A single building cell dilated by R=3: 7×7 = 49 on-cells centred at (5,5).
    CHECK(on_count == 49);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_rgb_jpeg); free(g_irc_jpeg);
    free(g_bmask_bytes);
    g_rgb_jpeg = NULL; g_irc_jpeg = NULL; g_bmask_bytes = NULL;
}

// ----------------------------------------------------------------------------
// classify-vegetation end-to-end
// ----------------------------------------------------------------------------

static void test_classify_vegetation_canopy_points(void) {
    register_test_steps();

    // Setup: single 1m cell at (5, 5).
    //   - Ground point at z=10, normal (0,0,1)
    //   - 20 canopy points at z=15 (height=5m > CANOPY_HEIGHT_MIN),
    //     normals with |nz| > 0.3 but scattered.
    //   - Full ortho is green (NDVI ≈ 1)
    //   - No buildings near.
    // Expected: at least the canopy-like points get flagged canopy.
    const int N = 21;
    static double pos[63];
    static float nrm[63];
    pos[0] = 5.0; pos[1] = 5.0; pos[2] = 10.0;
    nrm[0] = 0.0f; nrm[1] = 0.0f; nrm[2] = 1.0f;
    double deg = 30.0 * 3.14159265358979323846 / 180.0;
    for (int i = 1; i < N; i++) {
        pos[i * 3 + 0] = 5.0 + 0.01 * i;
        pos[i * 3 + 1] = 5.0;
        pos[i * 3 + 2] = 15.0;
        double phi = 2.0 * 3.14159265358979323846 * i / (N - 1);
        nrm[i * 3 + 0] = (float)(sin(deg) * cos(phi));
        nrm[i * 3 + 1] = (float)(sin(deg) * sin(phi));
        nrm[i * 3 + 2] = (float)cos(deg);
    }
    g_positions = pos; g_n_points = N; g_normals = nrm;

    encode_solid_jpeg(0, 255, 0, 256, 256, &g_rgb_jpeg, &g_rgb_len); // green
    encode_solid_jpeg(255, 0, 0, 256, 256, &g_irc_jpeg, &g_irc_len); // NIR high

    g_bmask_bytes = make_zero_mask_05m();
    g_bmask_len = raster_total_bytes(20, 20, 1);
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_cadastre_fit_json = "{\"offsetX\":0,\"offsetY\":0}";
    g_cadastre_fit_len = strlen(g_cadastre_fit_json);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 16 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("classify-e2e");
    pipeline_add_step(p, "load-classification-fixture", NULL);
    pipeline_add_step(p, "build-tile-ndvi", "{\"cell_size_m\":1.0}");
    pipeline_add_step(p, "compute-normal-scatter", "{\"cell_size_m\":1.0}");
    pipeline_add_step(p, "compute-near-building-mask",
                      "{\"cell_size_m\":1.0,\"dilate_r\":3}");
    pipeline_add_step(p, "classify-vegetation", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 2, .names = {"canopy-flags", "grass-flags"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"canopy-flags", "grass-flags"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 2, &cb) == 0);

    store_handle_t hc;
    CHECK(store_lookup(store, &cap.hashes[0], &hc) == 0);
    CHECK(hc.len == (size_t)N);
    int n_canopy = 0;
    for (size_t i = 0; i < hc.len; i++) {
        if (hc.bytes[i]) n_canopy++;
    }
    fprintf(stderr, "canopy: %d/%d\n", n_canopy, N);
    // At least the 20 high-normal-scattered points should be canopy.
    CHECK(n_canopy >= 15);
    // The ground point (z=10) should NOT be canopy (height < min).
    CHECK(hc.bytes[0] == 0);
    store_release(store, &hc);

    store_handle_t hg;
    CHECK(store_lookup(store, &cap.hashes[1], &hg) == 0);
    CHECK(hg.len == (size_t)N);
    int n_grass = 0;
    for (size_t i = 0; i < hg.len; i++) {
        if (hg.bytes[i]) n_grass++;
    }
    fprintf(stderr, "grass: %d/%d\n", n_grass, N);
    // The ground point has NDVI > threshold and isn't canopy → grass.
    CHECK(hg.bytes[0] == 1);
    store_release(store, &hg);

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_rgb_jpeg); free(g_irc_jpeg);
    free(g_bmask_bytes);
    g_rgb_jpeg = NULL; g_irc_jpeg = NULL; g_bmask_bytes = NULL;
}

// ----------------------------------------------------------------------------

int main(void) {
    test_tile_ndvi_vegetation();
    test_normal_scatter_flat();
    test_normal_scatter_jitter();
    test_near_building_dilation();
    test_classify_vegetation_canopy_points();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
