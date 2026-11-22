// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tests for B5c coregister-ortho-lidar.
//
// Three layers:
//   1. edges.c primitives (Sobel, adaptive normalisation, mask helpers).
//   2. Synthetic coregistration: build aligned LiDAR + ortho rasters that
//      the scorer should report offset ≈ (0, 0). Then shift one side by a
//      known amount and verify the scorer recovers the offset within a
//      sub-cell tolerance.
//   3. End-to-end via the runner: feed synthetic rasters through the real
//      coregister-ortho-lidar step, parse the JSON output, verify fields.

#define _POSIX_C_SOURCE 200809L

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "edges.h"
#include "ortho-steps.h"
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
// edges.c unit tests
// ----------------------------------------------------------------------------

static void test_sobel_flat_image_is_zero(void) {
    const uint32_t cols = 8, rows = 8;
    float img[64];
    for (int i = 0; i < 64; i++) img[i] = 42.0f;
    float out[64];
    edges_sobel_magnitude(img, cols, rows, out);
    for (int i = 0; i < 64; i++) {
        CHECK(out[i] == 0.0f);
    }
}

static void test_sobel_vertical_step(void) {
    // Left half 0, right half 1 → vertical step at c=4. Sobel magnitude at
    // interior cells on the step should equal sqrt((4*(1-0))^2) = 4.
    const uint32_t cols = 8, rows = 8;
    float img[64];
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            img[r * cols + c] = c < 4 ? 0.0f : 1.0f;
        }
    }
    float out[64];
    edges_sobel_magnitude(img, cols, rows, out);
    // Interior cells at c=3 and c=4 are on the step. |gx|=4, |gy|=0 → mag=4.
    for (uint32_t r = 1; r < rows - 1; r++) {
        CHECK_NEAR(out[r * cols + 3], 4.0f, 1e-5);
        CHECK_NEAR(out[r * cols + 4], 4.0f, 1e-5);
    }
    // Border cells zero.
    for (uint32_t c = 0; c < cols; c++) {
        CHECK(out[0 * cols + c] == 0.0f);
        CHECK(out[(rows - 1) * cols + c] == 0.0f);
    }
}

static void test_adaptive_normalize_range(void) {
    // Input with a mean-shifted Gaussian-ish distribution. After normalisation,
    // every output must be in [0, 1], and at least one must be 0 (the minimum).
    float buf[100];
    for (int i = 0; i < 100; i++) {
        buf[i] = (float)(100 - i); // 100..1 descending
    }
    edges_adaptive_normalize(buf, 100);
    for (int i = 0; i < 100; i++) {
        CHECK(buf[i] >= 0.0f);
        CHECK(buf[i] <= 1.0f);
    }
    // Monotonic preservation: since input was decreasing, output must be too.
    for (int i = 0; i < 99; i++) {
        CHECK(buf[i] >= buf[i + 1] - 1e-6f);
    }
}

static void test_fill_nodata_min(void) {
    float src[5] = {NAN, 3.0f, 1.0f, NAN, 5.0f};
    float* filled = edges_fill_nodata(src, 5, NAN); // NAN means "use min"
    CHECK(filled != NULL);
    CHECK(filled[0] == 1.0f); // filled with global min
    CHECK(filled[1] == 3.0f);
    CHECK(filled[2] == 1.0f);
    CHECK(filled[3] == 1.0f);
    CHECK(filled[4] == 5.0f);
    free(filled);
}

static void test_mask_boundaries(void) {
    // 5×5 raster, centre (2,2) is a non-NaN island, rest NaN. With margin=0
    // the border ring stays untouched; only interior cells (r,c in [1, 3])
    // are checked for NaN adjacency. Every interior cell has a NaN in its
    // 3×3 stencil → all 9 interior cells zero out.
    const uint32_t cols = 5, rows = 5;
    float src[25];
    for (int i = 0; i < 25; i++) src[i] = NAN;
    src[2 * cols + 2] = 10.0f;
    float edges[25];
    for (int i = 0; i < 25; i++) edges[i] = 1.0f; // stand-in edges
    edges_mask_boundaries(edges, src, cols, rows, /*margin=*/0);
    // Interior 3×3 ring (rows 1..3, cols 1..3) must be zero.
    for (uint32_t r = 1; r <= 3; r++) {
        for (uint32_t c = 1; c <= 3; c++) {
            CHECK(edges[r * 5 + c] == 0.0f);
        }
    }
    // With margin=1, every border cell AND the centre-touching ring gets
    // zeroed. Re-test with margin=1 to exercise the border path.
    for (int i = 0; i < 25; i++) edges[i] = 1.0f;
    edges_mask_boundaries(edges, src, cols, rows, /*margin=*/1);
    for (int i = 0; i < 25; i++) {
        CHECK(edges[i] == 0.0f);
    }
}

// ----------------------------------------------------------------------------
// End-to-end synthetic test: known offset.
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_coreg_test_XXXXXX");
    if (mkdtemp(out) == NULL) exit(1);
}

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

// Synthesize a lidar-rasters artifact with only a building-like structure
// in `ndsm_like` channel (we use intensity for convenience — same scoring
// pair B). The other channels get neutral values so they don't confuse
// the correlation.
static uint8_t* synth_lidar_rasters(uint32_t cols, uint32_t rows, int box_x, int box_y,
                                    int box_size) {
    uint8_t* buf = raster_alloc(cols, rows, LIDAR_RC__COUNT);
    if (!buf) return NULL;
    for (uint32_t ch = 0; ch < LIDAR_RC__COUNT; ch++) {
        float* p = raster_channel_ptr(buf, cols, rows, ch);
        for (size_t i = 0; i < (size_t)cols * rows; i++) {
            p[i] = 0.0f;
        }
    }
    // Multi-return is all-NaN so pair C falls back to D (roughness × lumStd).
    float* mr = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MULTI_RETURN);
    for (size_t i = 0; i < (size_t)cols * rows; i++) mr[i] = NAN;
    // Density = 1 everywhere so cells don't look "empty".
    float* dens = raster_channel_ptr(buf, cols, rows, LIDAR_RC_DENSITY);
    for (size_t i = 0; i < (size_t)cols * rows; i++) dens[i] = 1.0f;

    // Put a high-intensity box in intensity (for pair B: intensity × luminance).
    float* intens = raster_channel_ptr(buf, cols, rows, LIDAR_RC_INTENSITY);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            int dx = (int)c - box_x;
            int dy = (int)r - box_y;
            bool inside = (dx >= 0 && dx < box_size && dy >= 0 && dy < box_size);
            intens[r * cols + c] = inside ? 1000.0f : 0.0f;
        }
    }
    return buf;
}

// Single-channel raster (e.g. ndsm, mask).
static uint8_t* synth_single_channel(uint32_t cols, uint32_t rows, float (*fn)(int, int, void*),
                                     void* ctx) {
    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) return NULL;
    float* p = raster_channel_ptr(buf, cols, rows, 0);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            p[r * cols + c] = fn((int)c, (int)r, ctx);
        }
    }
    return buf;
}

typedef struct box_ctx {
    int x, y, size;
    float inside_val;
    float outside_val;
} box_ctx_t;

static float box_fn(int c, int r, void* v) {
    box_ctx_t* b = v;
    int dx = c - b->x;
    int dy = r - b->y;
    bool inside = (dx >= 0 && dx < b->size && dy >= 0 && dy < b->size);
    return inside ? b->inside_val : b->outside_val;
}

// Ortho RGB rasters — 7 channels with the box showing up in luminance +
// lumStddev so pairs B and D have signal to correlate against.
static uint8_t* synth_ortho_rgb(uint32_t cols, uint32_t rows, int box_x, int box_y, int box_size) {
    uint8_t* buf = raster_alloc(cols, rows, ORGB__COUNT);
    if (!buf) return NULL;
    float* R = raster_channel_ptr(buf, cols, rows, ORGB_R);
    float* G = raster_channel_ptr(buf, cols, rows, ORGB_G);
    float* B = raster_channel_ptr(buf, cols, rows, ORGB_B);
    float* lum = raster_channel_ptr(buf, cols, rows, ORGB_LUMINANCE);
    float* sat = raster_channel_ptr(buf, cols, rows, ORGB_SATURATION);
    float* grn = raster_channel_ptr(buf, cols, rows, ORGB_GREENNESS);
    float* lstd = raster_channel_ptr(buf, cols, rows, ORGB_LUM_STDDEV);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            int dx = (int)c - box_x;
            int dy = (int)r - box_y;
            bool inside = (dx >= 0 && dx < box_size && dy >= 0 && dy < box_size);
            size_t k = r * cols + c;
            R[k] = inside ? 0.9f : 0.1f;
            G[k] = inside ? 0.9f : 0.1f;
            B[k] = inside ? 0.9f : 0.1f;
            lum[k] = inside ? 0.9f : 0.1f;
            sat[k] = 0.0f;
            grn[k] = 0.0f;
            lstd[k] = inside ? 0.5f : 0.0f; // pair D signal
        }
    }
    return buf;
}

static uint8_t* synth_ortho_irc(uint32_t cols, uint32_t rows) {
    uint8_t* buf = raster_alloc(cols, rows, OIRC__COUNT);
    if (!buf) return NULL;
    float* nir = raster_channel_ptr(buf, cols, rows, OIRC_NIR);
    float* ndvi = raster_channel_ptr(buf, cols, rows, OIRC_NDVI);
    for (size_t i = 0; i < (size_t)cols * rows; i++) {
        nir[i] = 0.5f;
        ndvi[i] = 0.0f;
    }
    return buf;
}

// Globals for the load fixture step.
static uint8_t* g_lidar_bytes = NULL;
static size_t g_lidar_len = 0;
static uint8_t* g_ndsm_bytes = NULL;
static size_t g_ndsm_len = 0;
static uint8_t* g_mask_bytes = NULL;
static size_t g_mask_len = 0;
static uint8_t* g_rgb_bytes = NULL;
static size_t g_rgb_len = 0;
static uint8_t* g_irc_bytes = NULL;
static size_t g_irc_len = 0;
static const char* g_canonical = "{\"bbox_l93\":[0,0,10,10]}";

static int load_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "lidar-rasters", g_lidar_bytes, g_lidar_len) != 0) return -1;
    if (step_write_output(ctx, "lidar-ndsm", g_ndsm_bytes, g_ndsm_len) != 0) return -1;
    if (step_write_output(ctx, "lidar-building-mask", g_mask_bytes, g_mask_len) != 0) return -1;
    if (step_write_output(ctx, "ortho-rgb-rasters", g_rgb_bytes, g_rgb_len) != 0) return -1;
    if (step_write_output(ctx, "ortho-irc-rasters", g_irc_bytes, g_irc_len) != 0) return -1;
    return step_write_output(ctx, "canonical-location", g_canonical, strlen(g_canonical));
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "lidar-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-ndsm", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "lidar-building-mask", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-rgb-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-irc-rasters", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-coreg-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 6,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

typedef struct cap {
    hash_t hash;
    bool got;
} cap_t;
static void cap_cb(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    cap_t* c = ctx;
    if (strcmp(name, "coreg-offset") == 0) {
        c->hash = *h;
        c->got = true;
    }
}

// Core routine: build synthetic rasters, run coregister, parse result.
// Ortho rasters are offset by (ortho_dx_cells, ortho_dy_cells) from lidar.
typedef struct coreg_result {
    double offset_x;
    double offset_y;
    double score;
    int n_pairs;
} coreg_result_t;

static int run_coreg_synthetic(int lidar_box_x, int lidar_box_y, int ortho_box_x, int ortho_box_y,
                               coreg_result_t* out) {
    const uint32_t cols = 40, rows = 40;
    const int box_size = 14;

    g_lidar_bytes = synth_lidar_rasters(cols, rows, lidar_box_x, lidar_box_y, box_size);
    g_lidar_len = raster_total_bytes(cols, rows, LIDAR_RC__COUNT);

    box_ctx_t box_l = {lidar_box_x, lidar_box_y, box_size, 10.0f, 0.0f};
    g_ndsm_bytes = synth_single_channel(cols, rows, box_fn, &box_l);
    g_ndsm_len = raster_total_bytes(cols, rows, 1);
    box_ctx_t mask = {lidar_box_x, lidar_box_y, box_size, 1.0f, 0.0f};
    g_mask_bytes = synth_single_channel(cols, rows, box_fn, &mask);
    g_mask_len = raster_total_bytes(cols, rows, 1);

    g_rgb_bytes = synth_ortho_rgb(cols, rows, ortho_box_x, ortho_box_y, box_size);
    g_rgb_len = raster_total_bytes(cols, rows, ORGB__COUNT);
    g_irc_bytes = synth_ortho_irc(cols, rows);
    g_irc_len = raster_total_bytes(cols, rows, OIRC__COUNT);

    char root[256];
    make_tmpdir(root, sizeof(root));
    // Use a 10m bbox but 40×40 grid → cell_size = 0.25. Match it in params.
    g_canonical = "{\"bbox_l93\":[0,0,10,10]}";

    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 8 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    step_registry_reset();
    CHECK(step_register(&LOAD_DEF) == 0);
    CHECK(registration_register_steps() == 0);

    pipeline_t* p = pipeline_new("coreg-test");
    pipeline_add_step(p, "load-coreg-fixture", NULL);
    pipeline_add_step(p, "coregister-ortho-lidar",
                      "{\"cell_size_m\":0.25,\"search_range_m\":3}");

    cap_t cap = {.got = false};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_cb};
    int dummy = 0;
    const char* subs[] = {"coreg-offset"};
    int rc = runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb);
    pipeline_free(p);

    int ok = (rc == 0 && cap.got) ? 0 : -1;
    if (ok == 0) {
        store_handle_t h;
        if (store_lookup(store, &cap.hash, &h) == 0) {
            // Parse JSON.
            char* copy = malloc(h.len + 1);
            memcpy(copy, h.bytes, h.len);
            copy[h.len] = 0;
            json_error_t je;
            json_t* root_j = json_loads(copy, 0, &je);
            free(copy);
            if (root_j) {
                out->offset_x = json_number_value(json_object_get(root_j, "offsetX"));
                out->offset_y = json_number_value(json_object_get(root_j, "offsetY"));
                out->score = json_number_value(json_object_get(root_j, "score"));
                json_t* pair_scores = json_object_get(root_j, "pair_scores");
                out->n_pairs = (int)json_array_size(pair_scores);
                json_decref(root_j);
            } else {
                ok = -1;
            }
            store_release(store, &h);
        } else {
            ok = -1;
        }
    }

    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(g_lidar_bytes); free(g_ndsm_bytes); free(g_mask_bytes);
    free(g_rgb_bytes); free(g_irc_bytes);
    g_lidar_bytes = g_ndsm_bytes = g_mask_bytes = g_rgb_bytes = g_irc_bytes = NULL;
    return ok;
}

static void test_coreg_aligned_zero_offset(void) {
    // Both boxes at the same location → coreg should find (0, 0).
    coreg_result_t r = {0};
    CHECK(run_coreg_synthetic(13, 13, 13, 13, &r) == 0);
    CHECK_NEAR(r.offset_x, 0.0, 0.2); // sub-cell tolerance: 0.2m < one cell (0.25m)
    CHECK_NEAR(r.offset_y, 0.0, 0.2);
    CHECK(r.score > 0);
    CHECK(r.n_pairs >= 2);
}

static void test_coreg_known_offset_x(void) {
    // LiDAR box at (13, 13), ortho box at (10, 13). The LiDAR features
    // appear 3 cells to the RIGHT of the ortho features in raster-space.
    // To align lidar(r, c) with ortho(r-dy, c-dx) we need dx = +3 cells.
    // Cell size is 0.25m → offsetX = +3 × 0.25 = +0.75m.
    coreg_result_t r = {0};
    CHECK(run_coreg_synthetic(13, 13, 10, 13, &r) == 0);
    CHECK_NEAR(r.offset_x, 0.75, 0.2);
    CHECK_NEAR(r.offset_y, 0.0, 0.2);
}

static void test_coreg_known_offset_y(void) {
    // LiDAR box at (13, 15), ortho box at (13, 13) → dy = +2 cells = +0.5m.
    coreg_result_t r = {0};
    CHECK(run_coreg_synthetic(13, 15, 13, 13, &r) == 0);
    CHECK_NEAR(r.offset_x, 0.0, 0.2);
    CHECK_NEAR(r.offset_y, 0.5, 0.2);
}

static void test_coreg_diagonal_offset(void) {
    // dx = +2 cells, dy = +1 cell → +0.5m, +0.25m.
    coreg_result_t r = {0};
    CHECK(run_coreg_synthetic(14, 14, 12, 13, &r) == 0);
    CHECK_NEAR(r.offset_x, 0.5, 0.2);
    CHECK_NEAR(r.offset_y, 0.25, 0.2);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_sobel_flat_image_is_zero();
    test_sobel_vertical_step();
    test_adaptive_normalize_range();
    test_fill_nodata_min();
    test_mask_boundaries();
    test_coreg_aligned_zero_offset();
    test_coreg_known_offset_x();
    test_coreg_known_offset_y();
    test_coreg_diagonal_offset();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
