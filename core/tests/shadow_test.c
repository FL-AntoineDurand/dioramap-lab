// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tests for B8a/B8b shadow primitives:
//   B8a: detect-photo-shadows, cast-shadows-from-dsm
//   B8b: sun-position (pure unit), find-best-sun-time, deshadow-ortho-rgb
//
// Synthetic luminance / DSM rasters with known geometry; verify the
// per-cell shadow scores match expectation. Cast-shadow tests vary the
// sun direction (overhead, low-angle east, below-horizon) to exercise the
// branches in the kernel.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "ortho-steps.h"
#include "raster_format.h"
#include "registration-steps.h"
#include "shadow-steps.h"
#include "sun-position.h"
#include "shadow-cast-kernel.h"

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
    snprintf(out, out_sz, "/tmp/dioramap_shadow_test_XXXXXX");
    if (mkdtemp(out) == NULL) exit(1);
}

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

// Fixture globals.
static uint8_t* g_orgb_bytes = NULL;
static size_t g_orgb_len = 0;
static uint8_t* g_lidar_bytes = NULL;
static size_t g_lidar_len = 0;

static int load_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "ortho-rgb-rasters", g_orgb_bytes, g_orgb_len) != 0)
        return -1;
    return step_write_output(ctx, "lidar-rasters", g_lidar_bytes, g_lidar_len);
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "ortho-rgb-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-shadow-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 2,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

typedef struct cap {
    hash_t hashes[2];
    const char* names[2];
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
    CHECK(shadow_register_steps() == 0);
}

// Build a 7-channel ortho-rgb-rasters where every channel except LUMINANCE
// is zeroed; LUMINANCE is filled by the caller via a callback.
typedef float (*lum_fn)(int c, int r, void* ctx);
static uint8_t* synth_orgb(uint32_t cols, uint32_t rows, lum_fn fn, void* ctx) {
    uint8_t* buf = raster_alloc(cols, rows, ORGB__COUNT);
    for (uint32_t ch = 0; ch < ORGB__COUNT; ch++) {
        float* p = raster_channel_ptr(buf, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = 0;
    }
    float* lum = raster_channel_ptr(buf, cols, rows, ORGB_LUMINANCE);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            lum[(size_t)r * cols + c] = fn((int)c, (int)r, ctx);
        }
    }
    return buf;
}

// Build a 7-channel lidar-rasters with MAX_Z filled by callback; other
// channels zeroed.
typedef float (*z_fn)(int c, int r, void* ctx);
static uint8_t* synth_lidar(uint32_t cols, uint32_t rows, z_fn fn, void* ctx) {
    uint8_t* buf = raster_alloc(cols, rows, LIDAR_RC__COUNT);
    for (uint32_t ch = 0; ch < LIDAR_RC__COUNT; ch++) {
        float* p = raster_channel_ptr(buf, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = 0;
    }
    float* mz = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MAX_Z);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            mz[(size_t)r * cols + c] = fn((int)c, (int)r, ctx);
        }
    }
    return buf;
}

// ----------------------------------------------------------------------------
// detect-photo-shadows
// ----------------------------------------------------------------------------

typedef struct dark_box {
    int x0, y0, x1, y1; // inclusive shadow region
    float lit, shadow;
} dark_box_t;
static float dark_box_lum(int c, int r, void* v) {
    dark_box_t* b = v;
    if (c >= b->x0 && c <= b->x1 && r >= b->y0 && r <= b->y1) return b->shadow;
    return b->lit;
}

static void test_detect_shadows_dark_patch(void) {
    register_test_steps();
    // 30×30 raster, shadow patch (10..20, 10..20), lit=0.9, shadow=0.3.
    // ratio = 0.3 / 0.9 = 0.333 < 0.65 → score = (0.65 - 0.333)/0.65 ≈ 0.487.
    dark_box_t box = {10, 10, 20, 20, 0.9f, 0.3f};
    g_orgb_bytes = synth_orgb(30, 30, dark_box_lum, &box);
    g_orgb_len = raster_total_bytes(30, 30, ORGB__COUNT);
    g_lidar_bytes = raster_alloc(30, 30, LIDAR_RC__COUNT);
    g_lidar_len = raster_total_bytes(30, 30, LIDAR_RC__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("detect-shadow-test");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    pipeline_add_step(p, "detect-photo-shadows",
                      "{\"radius_cells\":12,\"threshold\":0.65}");
    cap_t cap = {.n = 1, .names = {"photo-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"photo-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    CHECK(store_lookup(store, &cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == 30 && v.rows == 30);
    // Center of shadow patch = cell (15, 15). With R=12, the local-max
    // window reaches the surrounding 0.9 cells → ratio = 0.333 → score ≈ 0.487.
    CHECK_NEAR(v.channels[0][15 * 30 + 15], (0.65f - 0.3f / 0.9f) / 0.65f, 1e-3);
    // Far corner (well outside the shadow): luminance == local max → score = 0.
    CHECK_NEAR(v.channels[0][29 * 30 + 29], 0.0f, 1e-5);
    // Lit area near the patch: also 0 (it IS the local max).
    CHECK_NEAR(v.channels[0][5 * 30 + 5], 0.0f, 1e-5);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

static void test_detect_shadows_uniform_image_zero(void) {
    register_test_steps();
    // Uniform luminance 0.5 → every cell ratio = 1.0 → score = 0 everywhere.
    dark_box_t box = {-1, -1, -1, -1, 0.5f, 0.5f};
    g_orgb_bytes = synth_orgb(20, 20, dark_box_lum, &box);
    g_orgb_len = raster_total_bytes(20, 20, ORGB__COUNT);
    g_lidar_bytes = raster_alloc(20, 20, LIDAR_RC__COUNT);
    g_lidar_len = raster_total_bytes(20, 20, LIDAR_RC__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("detect-uniform");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    pipeline_add_step(p, "detect-photo-shadows", NULL);
    cap_t cap = {.n = 1, .names = {"photo-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"photo-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    for (size_t i = 0; i < (size_t)v.cols * v.rows; i++) {
        CHECK(v.channels[0][i] == 0.0f);
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

// ----------------------------------------------------------------------------
// cast-shadows-from-dsm
// ----------------------------------------------------------------------------

// Synthetic DSM: a tall step (z = high_z) on the WEST half of the grid,
// flat ground (z = 0) on the EAST half. With sun coming from the east
// (sun_dx < 0, i.e. the sun's L93 east component is negative since the
// vector points TOWARD the sun), the step's east face casts a shadow
// onto the eastern flat ground cells immediately adjacent to the step.
typedef struct step_dsm {
    int step_col;
    float high_z;
    float low_z;
} step_dsm_t;
static float step_dsm_z(int c, int r, void* v) {
    step_dsm_t* d = v;
    (void)r;
    return c < d->step_col ? d->high_z : d->low_z;
}

static void test_cast_shadow_below_horizon_all_shadow(void) {
    register_test_steps();
    step_dsm_t d = {10, 5.0f, 0.0f};
    g_lidar_bytes = synth_lidar(20, 20, step_dsm_z, &d);
    g_lidar_len = raster_total_bytes(20, 20, LIDAR_RC__COUNT);
    g_orgb_bytes = raster_alloc(20, 20, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(20, 20, ORGB__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("cast-night");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    // Sun pointing DOWN (below horizon) → everything in shadow.
    pipeline_add_step(p, "cast-shadows-from-dsm",
                      "{\"sun_dx\":0,\"sun_dy\":0,\"sun_dz\":-1,\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"synth-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"synth-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    for (size_t i = 0; i < (size_t)v.cols * v.rows; i++) {
        CHECK(v.channels[0][i] == 1.0f);
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

static void test_cast_shadow_overhead_no_shadow(void) {
    register_test_steps();
    step_dsm_t d = {10, 5.0f, 0.0f};
    g_lidar_bytes = synth_lidar(20, 20, step_dsm_z, &d);
    g_lidar_len = raster_total_bytes(20, 20, LIDAR_RC__COUNT);
    g_orgb_bytes = raster_alloc(20, 20, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(20, 20, ORGB__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("cast-overhead");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    // Sun directly overhead → no horizontal motion → no shadows.
    pipeline_add_step(p, "cast-shadows-from-dsm",
                      "{\"sun_dx\":0,\"sun_dy\":0,\"sun_dz\":1,\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"synth-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"synth-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    for (size_t i = 0; i < (size_t)v.cols * v.rows; i++) {
        CHECK(v.channels[0][i] == 0.0f);
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

static void test_cast_shadow_step_casts_eastward(void) {
    register_test_steps();
    // Wall at column 10, height 10. Sun coming from the WEST (so it shines
    // FROM the west TOWARD the east). The sun-direction vector points
    // TOWARD the sun, so for a sun in the west the vector has +y? NO — the
    // sun direction in L93 axes (x=east, y=north, z=up). Sun in the west
    // means we look westward to see it: dx = -1, dz = some positive.
    // With 45° elevation: dx = -cos(45) ≈ -0.707, dy = 0, dz = sin(45) ≈ 0.707.
    // Wall at col 10, height 10. The wall's east face is in shadow because
    // the sun comes from the west. For the WEST half (cols < 10) cells:
    // they're at z=10, the ray marches west-up — leaves the grid quickly,
    // unblocked → lit. For the EAST half (cols ≥ 10) cells: they're at z=0,
    // ray marches west-up; needs to clear z=10 wall to escape. Wall is at
    // (cols < 10), so cells at col=10..K are blocked when (10+stepX) cells
    // away the wall is still > ray altitude.
    //
    // Detail: a cell at col=15 starts at (15.5, *, 0+0.1). After one step
    // (stepX = -0.707, stepZ = +0.707) it's at (14.79, *, 0.81). After
    // ~7 steps it reaches col=10 at altitude ~5.05 — wall is z=10 → blocked.
    // So cells immediately east of the wall (cols 10..several) should be
    // shadowed.
    step_dsm_t d = {10, 10.0f, 0.0f};
    g_lidar_bytes = synth_lidar(40, 20, step_dsm_z, &d);
    g_lidar_len = raster_total_bytes(40, 20, LIDAR_RC__COUNT);
    g_orgb_bytes = raster_alloc(40, 20, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(40, 20, ORGB__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("cast-step-east");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    pipeline_add_step(p, "cast-shadows-from-dsm",
                      "{\"sun_dx\":-0.707,\"sun_dy\":0,\"sun_dz\":0.707,"
                      "\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"synth-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"synth-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    // Mid-row cell on the east side near the wall: should be in shadow.
    // Cells at col 10..16 east of the wall (col >= 10, z=0) — march west,
    // reach the wall at z still well below 10 → blocked → 1.0.
    // Far east cells (col 25+): the ray exits the grid before reaching
    // any obstacle → 0.0.
    int shadowed_near = 0;
    for (int c = 10; c <= 14; c++) {
        if (v.channels[0][10 * 40 + c] == 1.0f) shadowed_near++;
    }
    CHECK(shadowed_near >= 4); // expect most near cells in shadow
    // Far east cell at col 35: the ray, marching west-up, will hit the
    // wall (col 10) at altitude ~17.7 — well above the wall's z=10 →
    // unblocked → lit.
    CHECK(v.channels[0][10 * 40 + 35] == 0.0f);
    // West-side cells are at z=10 (= wall top). They start at z=10+epsilon,
    // march west-up; the wall extends from col 0..9 all at z=10, so the
    // ray is always above the wall surface (z=10+eps+steps) → lit.
    CHECK(v.channels[0][10 * 40 + 5] == 0.0f);
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

static void test_cast_shadow_nan_cells_lit(void) {
    register_test_steps();
    // All-NaN DSM → every cell emitted as lit (0.0).
    step_dsm_t d = {0, NAN, NAN};
    g_lidar_bytes = synth_lidar(10, 10, step_dsm_z, &d);
    g_lidar_len = raster_total_bytes(10, 10, LIDAR_RC__COUNT);
    g_orgb_bytes = raster_alloc(10, 10, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(10, 10, ORGB__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("cast-nan");
    pipeline_add_step(p, "load-shadow-fixture", NULL);
    pipeline_add_step(p, "cast-shadows-from-dsm",
                      "{\"sun_dx\":-0.5,\"sun_dy\":0,\"sun_dz\":0.866,"
                      "\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"synth-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"synth-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);
    store_handle_t h;
    store_lookup(store, &cap.hashes[0], &h);
    raster_view_t v;
    raster_view_open(h.bytes, h.len, &v);
    for (size_t i = 0; i < (size_t)v.cols * v.rows; i++) {
        CHECK(v.channels[0][i] == 0.0f);
    }
    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_orgb_bytes);
    free(g_lidar_bytes);
    g_orgb_bytes = NULL;
    g_lidar_bytes = NULL;
}

// ----------------------------------------------------------------------------
// sun-position oracle (B8b)
// ----------------------------------------------------------------------------
// Numbers below were generated against suncalc.js (via `node -e ...`) for
// Notre-Dame Paris (lat=48.8566, lon=2.3522) at four UTC instants spread
// across the year. The C port is allowed to differ by ~1e-5 rad — that's
// roughly 2 arcseconds, well below SunCalc's own 1-arcminute accuracy
// envelope. Difference comes from floating-point accumulation order; the
// algorithm is identical line-for-line.
static void test_sun_position_oracle(void) {
    const double lat = 48.8566, lon = 2.3522;
    struct {
        const char* label;
        int y, mo, d, h, mi;
        double az_js, alt_js;
    } cases[] = {
        {"jun-15-1430z", 2024, 6, 15, 14, 30, 1.13593129, 0.86876687},
        {"dec-21-1200z", 2024, 12, 21, 12, 0, 0.04568748, 0.30828093},
        {"mar-21-0600z", 2024, 3, 21, 6, 0, -1.56819729, 0.00878246},
        {"sep-21-1800z", 2024, 9, 21, 18, 0, 1.62906651, -0.03997889},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double u = sun_unix_from_utc(cases[i].y, cases[i].mo, cases[i].d, cases[i].h, cases[i].mi);
        sun_position_t p = sun_position_at(u, lat, lon);
        CHECK_NEAR(p.azimuth, cases[i].az_js, 1e-5);
        CHECK_NEAR(p.altitude, cases[i].alt_js, 1e-5);
    }
}

// Sun-direction sanity: south-southwest June afternoon should give a
// vector with roughly (-0.6, -0.3, +0.75) in L93 axes, matching the
// hard-coded sun used in the viewer-shadows pipeline. Loose tolerances —
// this is a smoke check, not a precision test.
static void test_sun_dir_l93_jun_paris(void) {
    double u = sun_unix_from_utc(2024, 6, 15, 14, 30);
    sun_position_t p = sun_position_at(u, 48.8566, 2.3522);
    double dir[3];
    sun_dir_l93(p, dir);
    CHECK_NEAR(dir[0], -0.586, 0.01); // east component negative (sun in SW)
    CHECK_NEAR(dir[1], -0.272, 0.01); // north component negative (sun in S)
    CHECK_NEAR(dir[2], 0.764, 0.01);  // up component, ~50° altitude
}

// ----------------------------------------------------------------------------
// find-best-sun-time recovery (B8b)
// ----------------------------------------------------------------------------
// Strategy: pick a known UTC instant, compute the true sun direction with
// the C port, ray-march that direction over a wall-and-flat synthetic DSM
// to produce a "photo-shadow-mask" (synthetic — but it represents what
// the photo SHOULD show at that sun position). Then run find-best-sun-time
// and check the recovered direction is close to the truth one. Fine-pass
// alone won't reach the truth from a far-off coarse winner, so the test
// is a real end-to-end recovery.

// Globals for an extended fixture: lidar + ortho-rgb (B8a defaults) +
// canonical-location + photo-shadow-mask. Tests set whichever they need.
static char* g_canon_loc = NULL;
static uint8_t* g_photo_mask_bytes = NULL;
static size_t g_photo_mask_len = 0;
static uint8_t* g_oirc_bytes = NULL;
static size_t g_oirc_len = 0;
static uint8_t* g_fitted_mask_bytes = NULL;
static size_t g_fitted_mask_len = 0;

// Loader for find-best-sun-time tests. Writes the FBST inputs only, so
// pipeline validation doesn't see duplicate writes against the step's
// `fitted-shadow-mask` output.
static int load_fbst_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "ortho-rgb-rasters", g_orgb_bytes, g_orgb_len) != 0) return -1;
    if (step_write_output(ctx, "lidar-rasters", g_lidar_bytes, g_lidar_len) != 0) return -1;
    if (step_write_output(ctx, "canonical-location", g_canon_loc, strlen(g_canon_loc)) != 0)
        return -1;
    return step_write_output(ctx, "photo-shadow-mask", g_photo_mask_bytes, g_photo_mask_len);
}
static const step_write_t load_fbst_writes[] = {
    {.name = "ortho-rgb-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "photo-shadow-mask", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_FBST_DEF = {
    .name = "load-fbst-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_fbst_writes,
    .n_writes = 4,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_fbst_exec,
};

// Loader for deshadow tests. Writes the deshadow-ortho-rgb inputs only.
static int load_deshadow_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "ortho-rgb-rasters", g_orgb_bytes, g_orgb_len) != 0) return -1;
    if (step_write_output(ctx, "ortho-irc-rasters", g_oirc_bytes, g_oirc_len) != 0) return -1;
    return step_write_output(ctx, "fitted-shadow-mask", g_fitted_mask_bytes, g_fitted_mask_len);
}
static const step_write_t load_deshadow_writes[] = {
    {.name = "ortho-rgb-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-irc-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "fitted-shadow-mask", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DESHADOW_DEF = {
    .name = "load-deshadow-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_deshadow_writes,
    .n_writes = 3,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_deshadow_exec,
};

static void register_fbst_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_FBST_DEF) == 0);
    CHECK(shadow_register_steps() == 0);
}
static void register_deshadow_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_DESHADOW_DEF) == 0);
    CHECK(shadow_register_steps() == 0);
}

static void free_b8b_globals(void) {
    free(g_orgb_bytes); g_orgb_bytes = NULL;
    free(g_lidar_bytes); g_lidar_bytes = NULL;
    free(g_canon_loc); g_canon_loc = NULL;
    free(g_photo_mask_bytes); g_photo_mask_bytes = NULL;
    free(g_oirc_bytes); g_oirc_bytes = NULL;
    free(g_fitted_mask_bytes); g_fitted_mask_bytes = NULL;
}

static void test_find_best_sun_time_recovery(void) {
    register_fbst_test_steps();

    const uint32_t cols = 60, rows = 60;
    const double cell_size = 1.0;
    // Synthetic DSM: tall block (z=15) covering cols 25..35, rows 25..35.
    // Plenty of geometry for shadows in any sun direction.
    float* maxZ_ref = (float*)malloc((size_t)cols * rows * sizeof(float));
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            float z = (c >= 25 && c <= 35 && r >= 25 && r <= 35) ? 15.0f : 0.0f;
            maxZ_ref[(size_t)r * cols + c] = z;
        }
    }
    // Stuff into a synth_lidar buffer.
    g_lidar_bytes = raster_alloc(cols, rows, LIDAR_RC__COUNT);
    g_lidar_len = raster_total_bytes(cols, rows, LIDAR_RC__COUNT);
    for (uint32_t ch = 0; ch < LIDAR_RC__COUNT; ch++) {
        float* p = raster_channel_ptr(g_lidar_bytes, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = 0;
    }
    float* maxZ = raster_channel_ptr(g_lidar_bytes, cols, rows, LIDAR_RC_MAX_Z);
    memcpy(maxZ, maxZ_ref, (size_t)cols * rows * sizeof(float));

    // Truth: June 15 14:30 UTC at Paris.
    const int t_y = 2024, t_mo = 6, t_d = 15, t_h = 14, t_mi = 30;
    double u = sun_unix_from_utc(t_y, t_mo, t_d, t_h, t_mi);
    sun_position_t sp = sun_position_at(u, 48.8566, 2.3522);
    double truth_dir[3];
    sun_dir_l93(sp, truth_dir);

    // Photo-shadow-mask = the synthetic shadow at truth direction. Use
    // hard 0/1 values so soft_precision treats the recovery problem cleanly.
    uint8_t* photo_buf = raster_alloc(cols, rows, 1);
    float* photo = raster_channel_ptr(photo_buf, cols, rows, 0);
    cast_shadow_grid(maxZ, cols, rows,
                     truth_dir[0], truth_dir[1], truth_dir[2],
                     cell_size, 0.1, photo);
    g_photo_mask_bytes = photo_buf;
    g_photo_mask_len = raster_total_bytes(cols, rows, 1);

    g_orgb_bytes = raster_alloc(cols, rows, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(cols, rows, ORGB__COUNT);
    g_canon_loc = strdup("{\"lat\":48.8566,\"lon\":2.3522}");

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 64 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("fbst-recover");
    pipeline_add_step(p, "load-fbst-fixture", NULL);
    pipeline_add_step(p, "find-best-sun-time",
                      "{\"year\":2024,\"cell_size_m\":1.0,\"epsilon_m\":0.1}");
    cap_t cap = {.n = 2, .names = {"sun-direction", "fitted-shadow-mask"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"sun-direction", "fitted-shadow-mask"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 2, &cb) == 0);

    store_handle_t h_dir, h_mask;
    CHECK(store_lookup(store, &cap.hashes[0], &h_dir) == 0);
    CHECK(store_lookup(store, &cap.hashes[1], &h_mask) == 0);

    // Parse the JSON sun-direction.
    char json_copy[1024];
    size_t copy_len = h_dir.len < sizeof(json_copy) - 1 ? h_dir.len : sizeof(json_copy) - 1;
    memcpy(json_copy, h_dir.bytes, copy_len);
    json_copy[copy_len] = 0;

    // Crude parse — find the sun_d* fields. (Avoid pulling jansson into the
    // test linkage; the format is fully under our control.)
    double rec_dx = 0, rec_dy = 0, rec_dz = 0, rec_score = 0;
    sscanf(strstr(json_copy, "\"sun_dx\":") + 9, "%lf", &rec_dx);
    sscanf(strstr(json_copy, "\"sun_dy\":") + 9, "%lf", &rec_dy);
    sscanf(strstr(json_copy, "\"sun_dz\":") + 9, "%lf", &rec_dz);
    sscanf(strstr(json_copy, "\"score\":") + 8, "%lf", &rec_score);

    fprintf(stderr, "[fbst-recover] truth=(%.3f,%.3f,%.3f) recovered=(%.3f,%.3f,%.3f) score=%.3f\n",
            truth_dir[0], truth_dir[1], truth_dir[2], rec_dx, rec_dy, rec_dz, rec_score);

    // Score should be very high — synthetic photo IS the synthetic cast
    // at truth, so when the search lands at truth, the soft precision
    // approaches 1.0. Coarse grid is ±15 days × ±30 min, so the fine pass
    // should hit a candidate within ~5 min of truth.
    CHECK(rec_score > 0.9);
    // Recovered direction within 5° of truth (cos 5° ≈ 0.996).
    double dot = rec_dx * truth_dir[0] + rec_dy * truth_dir[1] + rec_dz * truth_dir[2];
    CHECK(dot > 0.996);

    // Fitted-shadow-mask should equal the truth mask cell-for-cell (it's
    // recast at the recovered direction, which is essentially the truth).
    raster_view_t v;
    CHECK(raster_view_open(h_mask.bytes, h_mask.len, &v) == 0);
    CHECK(v.cols == cols && v.rows == rows);
    int agree = 0;
    for (size_t i = 0; i < (size_t)cols * rows; i++) {
        if (v.channels[0][i] == photo[i]) agree++;
    }
    // ≥99% pixel agreement (tiny boundary cells may differ if the recovered
    // sun is one fine-step off truth).
    CHECK(agree > (int)((double)cols * rows * 0.99));

    store_release(store, &h_dir);
    store_release(store, &h_mask);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(maxZ_ref);
    free_b8b_globals();
}

// ----------------------------------------------------------------------------
// deshadow-ortho-rgb correction (B8b)
// ----------------------------------------------------------------------------
// Synthetic ortho with two halves: lit (R=G=B=200/255≈0.784) and "in
// shadow" (R=G=B=80/255≈0.314). The deshadow step internally scales to
// 0..255 to match bench-v2 semantics, so quoting expected values in 0..255
// space first then converting keeps the arithmetic readable.
// NDVI=0 everywhere (non-veg). Fitted-shadow-mask = 1 in the shadow half,
// 0 elsewhere. The category-aware correction should compute factor =
// 200/80 = 2.5 → push shadow cells toward 200.
static void test_deshadow_recovery_non_veg(void) {
    register_deshadow_test_steps();

    const uint32_t cols = 40, rows = 40;
    const float lit = 200.0f / 255.0f;
    const float sh = 80.0f / 255.0f;

    // ortho-rgb-rasters: R/G/B = lit on left half, sh on right half.
    g_orgb_bytes = raster_alloc(cols, rows, ORGB__COUNT);
    g_orgb_len = raster_total_bytes(cols, rows, ORGB__COUNT);
    for (uint32_t ch = 0; ch < ORGB__COUNT; ch++) {
        float* p = raster_channel_ptr(g_orgb_bytes, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = 0;
    }
    float* in_R = raster_channel_ptr(g_orgb_bytes, cols, rows, ORGB_R);
    float* in_G = raster_channel_ptr(g_orgb_bytes, cols, rows, ORGB_G);
    float* in_B = raster_channel_ptr(g_orgb_bytes, cols, rows, ORGB_B);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            float v = (c < cols / 2) ? lit : sh;
            in_R[r * cols + c] = v;
            in_G[r * cols + c] = v;
            in_B[r * cols + c] = v;
        }
    }

    // ortho-irc-rasters: NDVI = 0 (non-veg).
    g_oirc_bytes = raster_alloc(cols, rows, OIRC__COUNT);
    g_oirc_len = raster_total_bytes(cols, rows, OIRC__COUNT);
    for (uint32_t ch = 0; ch < OIRC__COUNT; ch++) {
        float* p = raster_channel_ptr(g_oirc_bytes, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) p[i] = 0;
    }

    // fitted-shadow-mask: 1 on right half, 0 on left.
    g_fitted_mask_bytes = raster_alloc(cols, rows, 1);
    g_fitted_mask_len = raster_total_bytes(cols, rows, 1);
    float* mask = raster_channel_ptr(g_fitted_mask_bytes, cols, rows, 0);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            mask[r * cols + c] = (c < cols / 2) ? 0.0f : 1.0f;
        }
    }

    // lidar-rasters: doesn't matter for deshadow but loader still writes it.
    g_lidar_bytes = raster_alloc(cols, rows, LIDAR_RC__COUNT);
    g_lidar_len = raster_total_bytes(cols, rows, LIDAR_RC__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root,
                                                   .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("deshadow-recover");
    pipeline_add_step(p, "load-deshadow-fixture", NULL);
    pipeline_add_step(p, "deshadow-ortho-rgb",
                      "{\"min_samples\":50,\"max_factor\":8.0}");
    cap_t cap = {.n = 1, .names = {"deshadowed-ortho-rgb-rasters"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"deshadowed-ortho-rgb-rasters"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t h;
    CHECK(store_lookup(store, &cap.hashes[0], &h) == 0);
    raster_view_t v;
    CHECK(raster_view_open(h.bytes, h.len, &v) == 0);
    CHECK(v.cols == cols && v.rows == rows);
    const float* out_R = v.channels[0];
    const float* out_G = v.channels[1];
    const float* out_B = v.channels[2];

    // Lit half: untouched (mask <= 0.25 path).
    CHECK_NEAR(out_R[5 * cols + 5], lit, 1e-5);
    CHECK_NEAR(out_G[5 * cols + 5], lit, 1e-5);
    CHECK_NEAR(out_B[5 * cols + 5], lit, 1e-5);

    // Shadow half (deep, mask=1, far from boundary): factor = 200/80 = 2.5
    // applied directly. Then the polish step (sat × bright boost) multiplies
    // by ~1.08 — for a perfectly grey input (R=G=B), the sat boost cancels
    // (lum == r), so only the bright boost lifts. 80 × 2.5 × 1.08 = 216,
    // then divided by 255 → ≈ 0.847.
    //   pixLum = 80; shadowLum = 80; litLum = 200; range = 120.
    //   strength = 1 - (80-80)/120 = 1; effective = 1.
    //   aR = 1 + (2.5-1)*1 = 2.5; r = 80 * 2.5 = 200.
    //   polish: lum = 200, satBoost = 1.25, brightBoost = 1.08.
    //   r = (200 + (200-200)*1.25) * 1.08 = 216 → /255 ≈ 0.847.
    float expected_shadow = 216.0f / 255.0f;
    CHECK_NEAR(out_R[20 * cols + 30], expected_shadow, 0.01);
    CHECK_NEAR(out_G[20 * cols + 30], expected_shadow, 0.01);
    CHECK_NEAR(out_B[20 * cols + 30], expected_shadow, 0.01);

    store_release(store, &h);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free_b8b_globals();
}

// ----------------------------------------------------------------------------

int main(void) {
    test_detect_shadows_dark_patch();
    test_detect_shadows_uniform_image_zero();
    test_cast_shadow_below_horizon_all_shadow();
    test_cast_shadow_overhead_no_shadow();
    test_cast_shadow_step_casts_eastward();
    test_cast_shadow_nan_cells_lit();
    test_sun_position_oracle();
    test_sun_dir_l93_jun_paris();
    test_find_best_sun_time_recovery();
    test_deshadow_recovery_non_veg();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
