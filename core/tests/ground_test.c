// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tests for B7 ground steps:
//   build-lidar-ground, build-parcel-lines.
//
// build-lidar-ground: bin ASPRS class-2 (ground) points → per-cell min Z
// + BFS-fill gaps. build-parcel-lines: drape cadastre polygon edges onto
// the ground grid + emit scene-local F32 line segments.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "ground-steps.h"
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

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_ground_test_XXXXXX");
    if (mkdtemp(out) == NULL) exit(1);
}

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

// Fixture globals.
static const char* g_canonical = NULL;
static const double* g_positions = NULL;
static size_t g_n_points = 0;
static const uint8_t* g_classes = NULL;
static const char* g_geojson = NULL;
static size_t g_geojson_len = 0;
static uint8_t* g_elev_bytes = NULL;
static size_t g_elev_len = 0;
static const char* g_origin = NULL;
static const char* g_cadfit = NULL;
static uint8_t* g_lidar_ground_bytes = NULL;
static size_t g_lidar_ground_len = 0;

// Two fixture variants. The "base" version writes everything EXCEPT the
// lidar-ground-grid raster; build-lidar-ground tests use this so the step
// itself produces the grid (no duplicate writer). The "with-grid" variant
// also writes a placeholder grid; build-parcel-lines tests use it so they
// can exercise the parcel step without first running build-lidar-ground.
static int load_base_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "canonical-location", g_canonical, strlen(g_canonical)) != 0)
        return -1;
    if (step_write_output(ctx, "lidar-positions", g_positions,
                          g_n_points * 3 * sizeof(double)) != 0)
        return -1;
    if (step_write_output(ctx, "lidar-classifications", g_classes, g_n_points) != 0)
        return -1;
    if (step_write_output(ctx, "cadastre-parcels-geojson", g_geojson, g_geojson_len) != 0)
        return -1;
    if (step_write_output(ctx, "elevation-f32", g_elev_bytes, g_elev_len) != 0) return -1;
    if (step_write_output(ctx, "scene-origin", g_origin, strlen(g_origin)) != 0) return -1;
    return step_write_output(ctx, "cadastre-fit", g_cadfit, strlen(g_cadfit));
}
static int load_with_ground_exec(step_context_t* ctx) {
    if (load_base_exec(ctx) != 0) return -1;
    return step_write_output(ctx, "lidar-ground-grid", g_lidar_ground_bytes,
                             g_lidar_ground_len);
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_base_writes[] = {
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-positions", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-classifications", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-parcels-geojson", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "elevation-f32", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "scene-origin", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-fit", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_write_t load_with_ground_writes[] = {
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-positions", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-classifications", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-parcels-geojson", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "elevation-f32", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "scene-origin", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-fit", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-ground-grid", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_BASE_DEF = {
    .name = "load-ground-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_base_writes,
    .n_writes = 7,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_base_exec,
};
static const step_def_t LOAD_WITH_GROUND_DEF = {
    .name = "load-ground-fixture-with-grid",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_with_ground_writes,
    .n_writes = 8,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_with_ground_exec,
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
    CHECK(step_register(&LOAD_BASE_DEF) == 0);
    CHECK(step_register(&LOAD_WITH_GROUND_DEF) == 0);
    CHECK(ground_register_steps() == 0);
}

// Make a placeholder lidar-ground-grid raster (1m grid, 10×10) used by
// build-parcel-lines tests that don't want to run build-lidar-ground first.
static uint8_t* make_const_ground(uint32_t cols, uint32_t rows, float z) {
    uint8_t* buf = raster_alloc(cols, rows, 1);
    float* p = raster_channel_ptr(buf, cols, rows, 0);
    for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = z;
    return buf;
}

// Make a placeholder elevation-f32 artifact: 12-byte raster header
// (cols=rows=side, n_channels=1) followed by side*side F32 cells.
// build-parcel-lines (v3) skips the header before reading the grid, so a
// raw F32 array without the header parses to a non-square count and the
// step rejects it.
static uint8_t* make_const_elev(uint32_t side, float z) {
    uint8_t* buf = raster_alloc(side, side, 1);
    float* p = raster_channel_ptr(buf, side, side, 0);
    for (size_t i = 0; i < (size_t)side * side; i++) p[i] = z;
    return buf;
}

// ----------------------------------------------------------------------------
// build-lidar-ground
// ----------------------------------------------------------------------------

static void test_ground_keeps_only_class2_min(void) {
    register_test_steps();

    // Bbox 10×10, cell 1m → 10×10 grid.
    g_canonical = "{\"bbox_l93\":[0,0,10,10],\"center_l93_x\":5,\"center_l93_y\":5,"
                  "\"radius_m\":5}";
    // 4 points in cell (5, 5):
    //   - 2 ASPRS class-2 (ground) points at z = 5.2 and z = 4.7
    //   - 1 high-veg (class 5) at z = 9.5
    //   - 1 building (class 6) at z = 8.2
    // Expected min-Z for that cell = 4.7 (only class-2 considered).
    static const double pos[12] = {
        5.5, 5.5, 5.2,
        5.5, 5.5, 4.7,
        5.5, 5.5, 9.5,
        5.5, 5.5, 8.2,
    };
    static const uint8_t classes[4] = {2, 2, 5, 6};
    g_positions = pos; g_n_points = 4; g_classes = classes;
    // Unused inputs (build-lidar-ground only uses positions + classes + canon):
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_elev_bytes = make_const_elev(4, 0.0f);
    g_elev_len = raster_total_bytes(4, 4, 1);
    g_origin = "{\"x\":0,\"y\":0,\"z\":0,\"min_z\":0,\"max_z\":0}";
    g_cadfit = "{\"offsetX\":0,\"offsetY\":0}";
    g_lidar_ground_bytes = make_const_ground(10, 10, 0.0f);
    g_lidar_ground_len = raster_total_bytes(10, 10, 1);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("ground-keep-class2");
    pipeline_add_step(p, "load-ground-fixture", NULL);
    pipeline_add_step(p, "build-lidar-ground", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"lidar-ground-grid"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"lidar-ground-grid"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    CHECK(store_lookup(store, &cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == 10 && v.rows == 10);
    // Cell (5, 5) → row 5 col 5 → index 55. Min Z over non-canopy = 4.7.
    CHECK_NEAR(v.channels[0][5 * 10 + 5], 4.7f, 1e-5);
    // Other cells got filled by BFS from cell (5,5) → all should equal 4.7.
    for (size_t k = 0; k < 100; k++) {
        CHECK_NEAR(v.channels[0][k], 4.7f, 1e-5);
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_ground_bytes);
    g_lidar_ground_bytes = NULL;
    free(g_elev_bytes);
    g_elev_bytes = NULL;
}

static void test_ground_bfs_fills_gaps(void) {
    register_test_steps();

    // Two ground points in opposite corners of a 10×10 grid; BFS should
    // propagate their Z values without leaving any cell empty.
    g_canonical = "{\"bbox_l93\":[0,0,10,10],\"center_l93_x\":5,\"center_l93_y\":5,"
                  "\"radius_m\":5}";
    static const double pos[6] = {
        0.5, 0.5, 10.0,
        9.5, 9.5, 20.0,
    };
    static const uint8_t classes[2] = {2, 2};
    g_positions = pos; g_n_points = 2; g_classes = classes;
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    g_geojson_len = strlen(g_geojson);
    g_elev_bytes = make_const_elev(4, 0.0f);
    g_elev_len = raster_total_bytes(4, 4, 1);
    g_origin = "{\"x\":0,\"y\":0,\"z\":0,\"min_z\":0,\"max_z\":0}";
    g_cadfit = "{\"offsetX\":0,\"offsetY\":0}";
    g_lidar_ground_bytes = make_const_ground(10, 10, 0.0f);
    g_lidar_ground_len = raster_total_bytes(10, 10, 1);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("ground-bfs");
    pipeline_add_step(p, "load-ground-fixture", NULL);
    pipeline_add_step(p, "build-lidar-ground", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"lidar-ground-grid"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"lidar-ground-grid"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    // Every cell should be either 10 or 20 (BFS-filled from the two seeds).
    int n_ten = 0, n_twenty = 0;
    for (size_t k = 0; k < 100; k++) {
        if (v.channels[0][k] == 10.0f) n_ten++;
        else if (v.channels[0][k] == 20.0f) n_twenty++;
        else { fprintf(stderr, "unexpected ground value at %zu: %g\n", k, v.channels[0][k]); }
    }
    CHECK(n_ten + n_twenty == 100);
    CHECK(n_ten > 0 && n_twenty > 0);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_ground_bytes);
    g_lidar_ground_bytes = NULL;
    free(g_elev_bytes);
    g_elev_bytes = NULL;
}

// ----------------------------------------------------------------------------
// build-parcel-lines
// ----------------------------------------------------------------------------

static void test_parcel_lines_simple_polygon(void) {
    register_test_steps();

    // bbox 20×20, single rectangular parcel from (5, 5) to (10, 10).
    // origin at (0, 0, 0), height_offset_m=0, subdivide_m=2, no clip.
    // Polygon perimeter = 20m. With subdivide=2 we get 10 sub-segments
    // around the polygon. Cell size 1m, scene origin (0,0,0).
    //
    // The radius=20 makes the disc bigger than the polygon, so no clipping.
    g_canonical = "{\"bbox_l93\":[0,0,20,20],\"center_l93_x\":10,\"center_l93_y\":10,"
                  "\"radius_m\":20}";
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[{"
                "\"type\":\"Feature\",\"properties\":{},"
                "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[["
                "[5,5],[10,5],[10,10],[5,10],[5,5]"
                "]]}}]}";
    g_geojson_len = strlen(g_geojson);
    static const double unused_pos[3] = {0};
    static const uint8_t unused_cls[1] = {0};
    g_positions = unused_pos; g_n_points = 1; g_classes = unused_cls;
    // Lidar ground at constant z=5 m → every parcel vertex gets z=5.
    g_lidar_ground_bytes = make_const_ground(20, 20, 5.0f);
    g_lidar_ground_len = raster_total_bytes(20, 20, 1);
    // Elevation grid: 4×4 squared (sqrt(16)=4), all zero.
    g_elev_bytes = make_const_elev(4, 0.0f);
    g_elev_len = raster_total_bytes(4, 4, 1);
    g_origin = "{\"x\":0,\"y\":0,\"z\":0,\"min_z\":0,\"max_z\":0}";
    g_cadfit = "{\"offsetX\":0,\"offsetY\":0}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("parcel-simple");
    pipeline_add_step(p, "load-ground-fixture-with-grid", NULL);
    pipeline_add_step(p, "build-parcel-lines",
                      "{\"subdivide_m\":2,\"ground_cell_size_m\":1.0,"
                      "\"height_offset_m\":0,\"clip_to_disc_radius\":false}");
    cap_t cap = {.n = 1, .names = {"parcel-lines"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"parcel-lines"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    CHECK(store_lookup(store, &cap.hashes[0], &h) == 0);
    // GeoJSON closes rings with a duplicate vertex (first == last), so the
    // 5-vertex polygon has 5 edges total — 4 real ones + 1 degenerate
    // closing edge. Real edges produce ceil(5/2) = 3 sub-segments each;
    // the degenerate closing edge contributes 1 zero-length sub-segment.
    // Total: 4*3 + 1 = 13.
    size_t seg_count = h.len / (6 * sizeof(float));
    fprintf(stderr, "parcel: %zu segments (expected 13)\n", seg_count);
    CHECK(seg_count == 13);
    // Every Z coordinate (z component, world up — v3 Z-up convention)
    // should equal 5.0 (lidar ground z, after recenter by scene-origin
    // which is z=0 in this fixture). Was y-component pre-v2 (Y-up legacy).
    const float* segs = (const float*)h.bytes;
    for (size_t s = 0; s < seg_count; s++) {
        for (int v_i = 0; v_i < 2; v_i++) {
            float pz = segs[s * 6 + v_i * 3 + 2];
            CHECK_NEAR(pz, 5.0f, 1e-3);
        }
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_ground_bytes);
    g_lidar_ground_bytes = NULL;
    free(g_elev_bytes);
    g_elev_bytes = NULL;
}

static void test_parcel_lines_disc_clip(void) {
    register_test_steps();

    // Parcel covers the entire 20×20 bbox. Disc radius is 5m around (10, 10).
    // Most edges sit OUTSIDE the disc and should be clipped out.
    g_canonical = "{\"bbox_l93\":[0,0,20,20],\"center_l93_x\":10,\"center_l93_y\":10,"
                  "\"radius_m\":5}";
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[{"
                "\"type\":\"Feature\",\"properties\":{},"
                "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[["
                "[0,0],[20,0],[20,20],[0,20],[0,0]"
                "]]}}]}";
    g_geojson_len = strlen(g_geojson);
    static const double unused_pos[3] = {0};
    static const uint8_t unused_cls[1] = {0};
    g_positions = unused_pos; g_n_points = 1; g_classes = unused_cls;
    g_lidar_ground_bytes = make_const_ground(20, 20, 0.0f);
    g_lidar_ground_len = raster_total_bytes(20, 20, 1);
    g_elev_bytes = make_const_elev(4, 0.0f);
    g_elev_len = raster_total_bytes(4, 4, 1);
    g_origin = "{\"x\":10,\"y\":10,\"z\":0,\"min_z\":0,\"max_z\":0}";
    g_cadfit = "{\"offsetX\":0,\"offsetY\":0}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("parcel-clip");
    pipeline_add_step(p, "load-ground-fixture-with-grid", NULL);
    pipeline_add_step(p, "build-parcel-lines",
                      "{\"subdivide_m\":2,\"ground_cell_size_m\":1.0,"
                      "\"clip_to_disc_radius\":true}");
    cap_t cap = {.n = 1, .names = {"parcel-lines"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"parcel-lines"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    size_t seg_count = h.len / (6 * sizeof(float));
    fprintf(stderr, "clipped parcel: %zu segments (corners outside, expect 0)\n", seg_count);
    // All four polygon corners are at distance ~14m from centre (10, 10),
    // outside the 5m radius → no segment should survive (both endpoints
    // outside).
    CHECK(seg_count == 0);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_ground_bytes);
    g_lidar_ground_bytes = NULL;
    free(g_elev_bytes);
    g_elev_bytes = NULL;
}

static void test_parcel_lines_cadastre_offset_applied(void) {
    register_test_steps();

    // Same square parcel as test_parcel_lines_simple_polygon, but cadastre
    // offset = (+10, +20). Origin at (15, 25, 0) puts the SHIFTED polygon
    // centred on the scene origin.
    g_canonical = "{\"bbox_l93\":[0,0,40,60],\"center_l93_x\":15,\"center_l93_y\":25,"
                  "\"radius_m\":50}";
    g_geojson = "{\"type\":\"FeatureCollection\",\"features\":[{"
                "\"type\":\"Feature\",\"properties\":{},"
                "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[["
                "[5,5],[10,5],[10,10],[5,10],[5,5]"
                "]]}}]}";
    g_geojson_len = strlen(g_geojson);
    static const double unused_pos[3] = {0};
    static const uint8_t unused_cls[1] = {0};
    g_positions = unused_pos; g_n_points = 1; g_classes = unused_cls;
    g_lidar_ground_bytes = make_const_ground(40, 60, 0.0f);
    g_lidar_ground_len = raster_total_bytes(40, 60, 1);
    g_elev_bytes = make_const_elev(4, 0.0f);
    g_elev_len = raster_total_bytes(4, 4, 1);
    // Origin at the SHIFTED centre of the polygon = (10,10) + (10,20) midpoint = (15, 25)
    g_origin = "{\"x\":15,\"y\":25,\"z\":0,\"min_z\":0,\"max_z\":0}";
    g_cadfit = "{\"offsetX\":10,\"offsetY\":20}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("parcel-offset");
    pipeline_add_step(p, "load-ground-fixture-with-grid", NULL);
    pipeline_add_step(p, "build-parcel-lines",
                      "{\"subdivide_m\":2,\"ground_cell_size_m\":1.0,"
                      "\"height_offset_m\":0,\"clip_to_disc_radius\":false}");
    cap_t cap = {.n = 1, .names = {"parcel-lines"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"parcel-lines"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    size_t seg_count = h.len / (6 * sizeof(float));
    CHECK(seg_count == 13); // see closing-vertex note above
    // Original polygon vertices were (5..10, 5..10). After +10,+20 shift
    // they're at (15..20, 25..30). After origin subtraction (15, 25),
    // scene-X span 0..5 and scene-Z span 0..-5 (since L93 north → scene -z).
    const float* segs = (const float*)h.bytes;
    int n_ok_x = 0;
    for (size_t s = 0; s < seg_count; s++) {
        // X coords should fall in [0, 5] inclusive (with rounding tolerance).
        float x1 = segs[s * 6 + 0];
        float x2 = segs[s * 6 + 3];
        if (x1 >= -1e-3 && x1 <= 5 + 1e-3 && x2 >= -1e-3 && x2 <= 5 + 1e-3) n_ok_x++;
    }
    CHECK(n_ok_x == (int)seg_count);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_ground_bytes);
    g_lidar_ground_bytes = NULL;
    free(g_elev_bytes);
    g_elev_bytes = NULL;
}

// ----------------------------------------------------------------------------

int main(void) {
    test_ground_keeps_only_class2_min();
    test_ground_bfs_fills_gaps();
    test_parcel_lines_simple_polygon();
    test_parcel_lines_disc_clip();
    test_parcel_lines_cadastre_offset_applied();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
