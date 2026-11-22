// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Tests for B5b ortho raster steps:
//   build-ortho-rgb-rasters, build-ortho-irc-rasters
// Plus the JPEG decode + bilinear resample helper.
//
// Synthesizes JPEGs in-memory with libjpeg (no fixture files needed) so the
// tests are reproducible offline.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "jpeg_decode.h"
#include "ortho-steps.h"
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
// JPEG synthesis helper: encode a w×h RGB buffer to a JPEG byte stream in
// memory. Quality 95 keeps the round-trip noise low so tests can assert
// near-exact pixel matches.
// ----------------------------------------------------------------------------

static int encode_rgb_to_jpeg(const uint8_t* rgb, int w, int h, uint8_t** out_bytes,
                              size_t* out_len) {
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
    *out_bytes = mem;
    *out_len = (size_t)mem_len;
    return 0;
}

// ----------------------------------------------------------------------------
// JPEG decode + resample unit tests
// ----------------------------------------------------------------------------

static void test_jpeg_decode_roundtrip(void) {
    // 8×4 image with a known pattern: (R, G, B) = (10*x + y, x*y + 1, 7*x).
    int w = 8, h = 4;
    uint8_t* rgb = malloc(w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = rgb + (y * w + x) * 3;
            p[0] = (uint8_t)(10 * x + y);
            p[1] = (uint8_t)(x * y + 1);
            p[2] = (uint8_t)(7 * x);
        }
    }
    uint8_t* jpeg_bytes;
    size_t jpeg_len;
    encode_rgb_to_jpeg(rgb, w, h, &jpeg_bytes, &jpeg_len);
    free(rgb);

    decoded_image_t img;
    CHECK(jpeg_decode_rgba(jpeg_bytes, jpeg_len, &img) == 0);
    CHECK(img.w == w);
    CHECK(img.h == h);
    // Alpha must be 255 everywhere.
    for (int i = 0; i < w * h; i++) {
        CHECK(img.rgba[i * 4 + 3] == 255);
    }
    decoded_image_free(&img);
    free(jpeg_bytes);
}

static void test_jpeg_decode_rejects_garbage(void) {
    uint8_t junk[64] = {0xFF, 0xD8, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF};
    decoded_image_t img;
    CHECK(jpeg_decode_rgba(junk, sizeof(junk), &img) == -1);
    CHECK(img.rgba == NULL);
}

static void test_resample_identity(void) {
    int w = 4, h = 3;
    uint8_t src[48];
    for (int i = 0; i < 48; i++) {
        src[i] = (uint8_t)(i * 3);
    }
    uint8_t* dst = jpeg_resample_rgba(src, w, h, w, h, /*flip_y=*/false);
    CHECK(dst != NULL);
    // Identity resample must return byte-equal output (within rounding).
    for (int i = 0; i < 48; i++) {
        CHECK_NEAR(dst[i], src[i], 1);
    }
    free(dst);
}

static void test_resample_flip_y(void) {
    // Two-row source: top row red, bottom row blue. After flip_y the
    // destination row 0 must be blue.
    int w = 2, h = 2;
    uint8_t src[16] = {
        255, 0, 0, 255, 255, 0, 0, 255, // row 0: red
        0,   0, 255, 255, 0,   0, 255, 255 // row 1: blue
    };
    uint8_t* dst = jpeg_resample_rgba(src, w, h, w, h, /*flip_y=*/true);
    CHECK(dst != NULL);
    // dst row 0 should now be blue (read from src row 1).
    CHECK(dst[0] == 0);
    CHECK(dst[1] == 0);
    CHECK(dst[2] == 255);
    // dst row 1 should be red.
    CHECK(dst[8] == 255);
    CHECK(dst[10] == 0);
    free(dst);
}

static void test_resample_downsample_constant_color(void) {
    // 16×16 solid magenta (255, 0, 255) → 4×4 must still be magenta.
    int w = 16, h = 16;
    uint8_t* src = malloc(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        src[i * 4 + 0] = 255;
        src[i * 4 + 1] = 0;
        src[i * 4 + 2] = 255;
        src[i * 4 + 3] = 255;
    }
    uint8_t* dst = jpeg_resample_rgba(src, w, h, 4, 4, false);
    CHECK(dst != NULL);
    for (int i = 0; i < 16; i++) {
        CHECK(dst[i * 4 + 0] == 255);
        CHECK(dst[i * 4 + 1] == 0);
        CHECK(dst[i * 4 + 2] == 255);
    }
    free(src);
    free(dst);
}

// ----------------------------------------------------------------------------
// build-ortho-rgb-rasters end-to-end via the runner
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_ortho_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        exit(1);
    }
}

static intent_map_t* open_test_im(const char* root) {
    char p[512];
    snprintf(p, sizeof(p), "%s/intent.log", root);
    return intent_map_open(p);
}

// Globals threaded into the load-fixture step (same pattern used in
// registration_test.c and normals_test.c).
static const uint8_t* g_jpeg = NULL;
static size_t g_jpeg_len = 0;
static const uint8_t* g_irc_jpeg = NULL;
static size_t g_irc_len = 0;
static const char* g_canonical = NULL;

static int load_exec(step_context_t* ctx) {
    if (step_write_output(ctx, "ortho-rgb-jpeg", g_jpeg, g_jpeg_len) != 0) {
        return -1;
    }
    // Every declared write must populate; for tests that don't use the IRC
    // path, write a single-byte placeholder so the runner is satisfied.
    // build-ortho-irc-rasters won't be invoked in the rgb-only pipeline.
    static const uint8_t placeholder[1] = {0};
    const uint8_t* irc = g_irc_jpeg ? g_irc_jpeg : placeholder;
    size_t irc_len = g_irc_jpeg ? g_irc_len : 1;
    if (step_write_output(ctx, "ortho-irc-jpeg", irc, irc_len) != 0) {
        return -1;
    }
    return step_write_output(ctx, "canonical-location", g_canonical, strlen(g_canonical));
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "ortho-rgb-jpeg", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "ortho-irc-jpeg", .kind = 0, .visibility = STEP_VIS_DEBUG},
    {.name = "canonical-location", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-ortho-fixture",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 3,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

typedef struct cap {
    hash_t hashes[4];
    const char* names[4];
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

static void register_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_DEF) == 0);
    CHECK(ortho_register_steps() == 0);
}

static void test_ortho_rgb_rasters_solid_red(void) {
    register_test_steps();
    // 256×256 solid red JPEG → ortho-rgb-rasters should read R≈1, G≈0, B≈0,
    // luminance≈0.299, saturation≈1, greenness≈-1 in every cell.
    int img_w = 256, img_h = 256;
    uint8_t* rgb = malloc(img_w * img_h * 3);
    for (int i = 0; i < img_w * img_h; i++) {
        rgb[i * 3 + 0] = 255;
        rgb[i * 3 + 1] = 0;
        rgb[i * 3 + 2] = 0;
    }
    uint8_t* jpeg_bytes;
    size_t jpeg_len;
    encode_rgb_to_jpeg(rgb, img_w, img_h, &jpeg_bytes, &jpeg_len);
    free(rgb);

    g_jpeg = jpeg_bytes;
    g_jpeg_len = jpeg_len;
    g_irc_jpeg = NULL;
    g_canonical = "{\"bbox_l93\":[0,0,10,10]}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 4 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("ortho-rgb-test");
    pipeline_add_step(p, "load-ortho-fixture", NULL);
    pipeline_add_step(p, "build-ortho-rgb-rasters", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 1, .names = {"ortho-rgb-rasters"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_artifact};
    int dummy = 0;
    const char* subs[] = {"ortho-rgb-rasters"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t sh;
    CHECK(store_lookup(store, &cap.hashes[0], &sh) == 0);
    raster_view_t v;
    CHECK(raster_view_open(sh.bytes, sh.len, &v) == 0);
    CHECK(v.cols == 10 && v.rows == 10);
    CHECK(v.n_channels == ORGB__COUNT);

    // Ignore boundary cells (1-cell border) for tolerance — bilinear leaks
    // a tiny amount of edge pixels in. JPEG is lossy but at q=95 ±0.02 is fine.
    for (uint32_t row = 1; row < v.rows - 1; row++) {
        for (uint32_t col = 1; col < v.cols - 1; col++) {
            size_t k = (size_t)row * v.cols + col;
            CHECK_NEAR(v.channels[ORGB_R][k], 1.0f, 0.05);
            CHECK_NEAR(v.channels[ORGB_G][k], 0.0f, 0.05);
            CHECK_NEAR(v.channels[ORGB_B][k], 0.0f, 0.05);
            CHECK_NEAR(v.channels[ORGB_LUMINANCE][k], 0.299f, 0.05);
            CHECK_NEAR(v.channels[ORGB_SATURATION][k], 1.0f, 0.05);
            CHECK_NEAR(v.channels[ORGB_GREENNESS][k], -1.0f, 0.05);
        }
    }
    store_release(store, &sh);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(jpeg_bytes);
}

static void test_ortho_irc_ndvi_vegetation(void) {
    register_test_steps();
    // RGB ortho: solid green (0, 255, 0) — R channel = 0.
    // IRC ortho: solid (255, 0, 0) — encodes NIR=255 in R.
    // NDVI = (NIR - R) / (NIR + R + epsilon) = (1.0 - 0.0) / (1.0 + 0.0 + ε) ≈ 1.0
    int img_w = 256, img_h = 256;
    uint8_t* rgb = malloc(img_w * img_h * 3);
    uint8_t* irc = malloc(img_w * img_h * 3);
    for (int i = 0; i < img_w * img_h; i++) {
        rgb[i * 3 + 0] = 0;
        rgb[i * 3 + 1] = 255;
        rgb[i * 3 + 2] = 0;
        irc[i * 3 + 0] = 255;
        irc[i * 3 + 1] = 0;
        irc[i * 3 + 2] = 0;
    }
    uint8_t* rgb_jpeg;
    size_t rgb_jpeg_len;
    encode_rgb_to_jpeg(rgb, img_w, img_h, &rgb_jpeg, &rgb_jpeg_len);
    free(rgb);
    uint8_t* irc_jpeg;
    size_t irc_jpeg_len;
    encode_rgb_to_jpeg(irc, img_w, img_h, &irc_jpeg, &irc_jpeg_len);
    free(irc);

    g_jpeg = rgb_jpeg;
    g_jpeg_len = rgb_jpeg_len;
    g_irc_jpeg = irc_jpeg;
    g_irc_len = irc_jpeg_len;
    g_canonical = "{\"bbox_l93\":[0,0,10,10]}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 4 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("ortho-irc-test");
    pipeline_add_step(p, "load-ortho-fixture", NULL);
    pipeline_add_step(p, "build-ortho-rgb-rasters", "{\"cell_size_m\":1.0}");
    pipeline_add_step(p, "build-ortho-irc-rasters", "{\"cell_size_m\":1.0}");
    cap_t cap = {.n = 2, .names = {"ortho-rgb-rasters", "ortho-irc-rasters"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_artifact};
    int dummy = 0;
    const char* subs[] = {"ortho-irc-rasters"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t sh;
    CHECK(store_lookup(store, &cap.hashes[1], &sh) == 0);
    raster_view_t v;
    CHECK(raster_view_open(sh.bytes, sh.len, &v) == 0);
    CHECK(v.n_channels == OIRC__COUNT);
    for (uint32_t row = 1; row < v.rows - 1; row++) {
        for (uint32_t col = 1; col < v.cols - 1; col++) {
            size_t k = (size_t)row * v.cols + col;
            CHECK_NEAR(v.channels[OIRC_NIR][k], 1.0f, 0.05);
            CHECK_NEAR(v.channels[OIRC_NDVI][k], 1.0f, 0.05);
        }
    }
    store_release(store, &sh);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(rgb_jpeg);
    free(irc_jpeg);
}

static void test_ortho_resample_matches_lidar_grid(void) {
    register_test_steps();
    // bbox 100×100, cell=0.5 → 200×200 grid (matches build-lidar-rasters).
    int img_w = 256, img_h = 256;
    uint8_t* rgb = malloc(img_w * img_h * 3);
    for (int i = 0; i < img_w * img_h; i++) {
        rgb[i * 3 + 0] = 128;
        rgb[i * 3 + 1] = 128;
        rgb[i * 3 + 2] = 128;
    }
    uint8_t* jpeg_bytes;
    size_t jpeg_len;
    encode_rgb_to_jpeg(rgb, img_w, img_h, &jpeg_bytes, &jpeg_len);
    free(rgb);

    g_jpeg = jpeg_bytes;
    g_jpeg_len = jpeg_len;
    g_irc_jpeg = NULL;
    g_canonical = "{\"bbox_l93\":[0,0,100,100]}";

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 4 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});
    pipeline_t* p = pipeline_new("ortho-grid-test");
    pipeline_add_step(p, "load-ortho-fixture", NULL);
    pipeline_add_step(p, "build-ortho-rgb-rasters", "{\"cell_size_m\":0.5}");
    cap_t cap = {.n = 1, .names = {"ortho-rgb-rasters"}};
    runner_callbacks_t cb = {.ctx = &cap, .on_artifact_ready = cap_artifact};
    int dummy = 0;
    const char* subs[] = {"ortho-rgb-rasters"};
    CHECK(runner_run(r, p, &dummy, sizeof(dummy), subs, 1, &cb) == 0);

    store_handle_t sh;
    CHECK(store_lookup(store, &cap.hashes[0], &sh) == 0);
    raster_view_t v;
    CHECK(raster_view_open(sh.bytes, sh.len, &v) == 0);
    CHECK(v.cols == 200 && v.rows == 200);
    store_release(store, &sh);
    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    free(jpeg_bytes);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_jpeg_decode_roundtrip();
    test_jpeg_decode_rejects_garbage();
    test_resample_identity();
    test_resample_flip_y();
    test_resample_downsample_constant_color();
    test_ortho_rgb_rasters_solid_red();
    test_ortho_irc_ndvi_vegetation();
    test_ortho_resample_matches_lidar_grid();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
