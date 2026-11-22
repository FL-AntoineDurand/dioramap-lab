// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// deshadow-ortho-rgb
// Reads:  ortho-rgb-rasters    (R, G, B channels — F32, normalised [0, 1]),
//         ortho-irc-rasters    (NDVI channel — F32, [-1, 1]),
//         fitted-shadow-mask   (F32 single-channel mask, 0=lit, 1=shadow).
// Params: { "ndvi_threshold": float (default 0.25),
//           "min_samples":    int   (default 200),
//           "max_factor":     float (default 8.0) }
// Writes: deshadowed-ortho-rgb-rasters — 3-channel F32 raster
//         (R, G, B in the same [0, 1] scale as the input). The downstream
//         lab UI can re-encode this raster to JPEG on demand; the pipeline
//         itself never touches an encoder, keeping the dataflow lossless.
//
// Implementation note on scale: bench-v2 categoryAwareDeshadow operates on
// 8-bit RGBA bytes (range 0..255) — its `max(1, …)` shadow floor and
// `range > 1` threshold both translate to "ignore values closer to pure
// black than ~1/255". To preserve those exact semantics on our 0..1
// rasters we scale input to 0..255 at entry and back to 0..1 at exit. The
// factors are dimensionless ratios, unaffected by the scaling.
//
// Direct port of bench-v2 viewer/backend/preprocess/deshadow.js
// categoryAwareDeshadow, adapted from operating on a full-resolution RGBA
// bitmap (with cell-grid bilinear lookup) to operating directly on the
// raster grid. Differences from the JS version:
//   - No bitmap → no per-pixel py/rf and px/cf mapping; the raster IS the
//     output domain. Sample stride = 1 (every cell visited).
//   - No transition blur (pass 3 in JS): the raster grid is already at
//     cell resolution, so the cell-grid step artifacts the JS pass blurs
//     don't exist here. The JS bench needed it because deshadow ran on a
//     ~1365px crop driven by a 200-cell mask; we operate on the 200-cell
//     domain directly.
//   - Three-pass (bucket sums → factors → per-pixel apply) preserved
//     identically.
//
// The two-category split (vegetation / non-vegetation, NDVI > threshold)
// is what stops dark roofs from being lifted toward grass-colour and
// vice-versa. Per-category lit/shadow means → R/G/B factors → per-pixel
// correction blended by mask softness × per-pixel darkness.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/step.h"

#include "../ortho/ortho-steps.h"
#include "../registration/raster_format.h"
#include "shadow-steps.h"

typedef struct dsh_params {
    double ndvi_threshold;
    int min_samples;
    double max_factor;
} dsh_params_t;

static int parse_params(const char* s, dsh_params_t* o) {
    o->ndvi_threshold = 0.25;
    o->min_samples = 200;
    o->max_factor = 8.0;
    if (!s) return 0;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return 0;
    json_t* j;
    j = json_object_get(p, "ndvi_threshold");
    if (json_is_number(j)) o->ndvi_threshold = json_number_value(j);
    j = json_object_get(p, "min_samples");
    if (json_int_or_default(j, 1, 1000000, "min_samples", &o->min_samples) != 0) {
        json_decref(p); return -1;
    }
    j = json_object_get(p, "max_factor");
    if (json_is_number(j)) o->max_factor = json_number_value(j);
    json_decref(p);
    return 0;
}

typedef struct bucket_t {
    double sR, sG, sB;
    long n;
} bucket_t;

typedef struct factor_t {
    double R, G, B;
    double lit_lum, shadow_lum;
    int identity; // 1 if R=G=B=1 (insufficient samples → no correction)
} factor_t;

static int exec_fn(step_context_t* ctx) {
    dsh_params_t p;
    parse_params(step_get_params(ctx), &p);

    size_t orgb_len = 0;
    const uint8_t* orgb_in = (const uint8_t*)step_read(ctx, "ortho-rgb-rasters", &orgb_len);
    if (!orgb_in) return -1;
    raster_view_t orgb;
    if (raster_view_open(orgb_in, orgb_len, &orgb) != 0 || orgb.n_channels < ORGB__COUNT) {
        return -1;
    }
    uint32_t cols = orgb.cols;
    uint32_t rows = orgb.rows;
    size_t cells = (size_t)cols * rows;
    const float* in_R = orgb.channels[ORGB_R];
    const float* in_G = orgb.channels[ORGB_G];
    const float* in_B = orgb.channels[ORGB_B];

    size_t oirc_len = 0;
    const uint8_t* oirc_in = (const uint8_t*)step_read(ctx, "ortho-irc-rasters", &oirc_len);
    if (!oirc_in) return -1;
    raster_view_t oirc;
    if (raster_view_open(oirc_in, oirc_len, &oirc) != 0 || oirc.n_channels < OIRC__COUNT) {
        return -1;
    }
    if (oirc.cols != cols || oirc.rows != rows) return -1;
    const float* ndvi = oirc.channels[OIRC_NDVI];

    size_t mask_len = 0;
    const uint8_t* mask_in = (const uint8_t*)step_read(ctx, "fitted-shadow-mask", &mask_len);
    if (!mask_in) return -1;
    raster_view_t mask_view;
    if (raster_view_open(mask_in, mask_len, &mask_view) != 0 || mask_view.n_channels < 1) {
        return -1;
    }
    if (mask_view.cols != cols || mask_view.rows != rows) return -1;
    const float* mask = mask_view.channels[0];

    // ---- Pass 1: bucket sums, hard threshold to keep means clean ----
    // buckets[cat][bucket] where cat ∈ {0=non-veg, 1=veg}, bucket ∈ {0=lit, 1=shadow}.
    // Sums accumulated in 0..255 scale (× 255) so the JS-derived constants
    // (`max(1, …)` floor, `range > 1` test, polish thresholds) stay valid.
    bucket_t buckets[2][2] = {{{0,0,0,0},{0,0,0,0}},{{0,0,0,0},{0,0,0,0}}};
    for (size_t i = 0; i < cells; i++) {
        float m = mask[i];
        int b;
        if (m < 0.15f) b = 0;
        else if (m > 0.85f) b = 1;
        else continue;
        float n = ndvi[i];
        int cat = (isfinite(n) && n > p.ndvi_threshold) ? 1 : 0;
        bucket_t* bk = &buckets[cat][b];
        bk->sR += in_R[i] * 255.0;
        bk->sG += in_G[i] * 255.0;
        bk->sB += in_B[i] * 255.0;
        bk->n++;
    }

    factor_t factors[2];
    for (int cat = 0; cat < 2; cat++) {
        bucket_t* lit = &buckets[cat][0];
        bucket_t* sh = &buckets[cat][1];
        if (sh->n < p.min_samples || lit->n < p.min_samples) {
            factors[cat] = (factor_t){1, 1, 1, 0, 0, 1};
            continue;
        }
        double litR = lit->sR / lit->n;
        double litG = lit->sG / lit->n;
        double litB = lit->sB / lit->n;
        double shR = sh->sR / sh->n; if (shR < 1) shR = 1;
        double shG = sh->sG / sh->n; if (shG < 1) shG = 1;
        double shB = sh->sB / sh->n; if (shB < 1) shB = 1;
        double fR = litR / shR; if (fR < 0.5) fR = 0.5; if (fR > p.max_factor) fR = p.max_factor;
        double fG = litG / shG; if (fG < 0.5) fG = 0.5; if (fG > p.max_factor) fG = p.max_factor;
        double fB = litB / shB; if (fB < 0.5) fB = 0.5; if (fB > p.max_factor) fB = p.max_factor;
        factors[cat] = (factor_t){
            .R = fR, .G = fG, .B = fB,
            .lit_lum = 0.299 * litR + 0.587 * litG + 0.114 * litB,
            .shadow_lum = 0.299 * shR + 0.587 * shG + 0.114 * shB,
            .identity = 0,
        };
    }

    // ---- Pass 2: per-pixel apply ----
    uint8_t* out_buf = raster_alloc(cols, rows, 3);
    if (!out_buf) return -1;
    float* out_R = raster_channel_ptr(out_buf, cols, rows, 0);
    float* out_G = raster_channel_ptr(out_buf, cols, rows, 1);
    float* out_B = raster_channel_ptr(out_buf, cols, rows, 2);

    for (size_t i = 0; i < cells; i++) {
        float m_lin = mask[i];
        double orR = in_R[i] * 255.0;
        double orG = in_G[i] * 255.0;
        double orB = in_B[i] * 255.0;
        if (m_lin <= 0.25f) {
            out_R[i] = in_R[i];
            out_G[i] = in_G[i];
            out_B[i] = in_B[i];
            continue;
        }
        double m;
        if (m_lin >= 0.75f) {
            m = 1.0;
        } else {
            double t = (m_lin - 0.25) / 0.5;
            m = t * t * (3.0 - 2.0 * t); // smoothstep
        }
        float n = ndvi[i];
        int cat = (isfinite(n) && n > p.ndvi_threshold) ? 1 : 0;
        factor_t* f = &factors[cat];
        if (f->identity) {
            out_R[i] = in_R[i];
            out_G[i] = in_G[i];
            out_B[i] = in_B[i];
            continue;
        }
        double pix_lum = 0.299 * orR + 0.587 * orG + 0.114 * orB;
        double range = f->lit_lum - f->shadow_lum;
        double strength = 1.0;
        if (range > 1.0) {
            strength = 1.0 - (pix_lum - f->shadow_lum) / range;
            if (strength < 0) strength = 0;
            else if (strength > 1) strength = 1;
        }
        double effective = m * strength;
        double green_boost = (cat == 1) ? 1.15 : 1.0;
        double aR = 1.0 + (f->R - 1.0) * effective;
        double aG = 1.0 + (f->G * green_boost - 1.0) * effective;
        double aB = 1.0 + (f->B - 1.0) * effective;
        double r = orR * aR;
        double g = orG * aG;
        double b = orB * aB;
        if (effective > 0.1) {
            double lum = 0.299 * r + 0.587 * g + 0.114 * b;
            double sat_boost = 1.0 + 0.25 * effective;
            double bright_boost = 1.0 + 0.08 * effective;
            r = (lum + (r - lum) * sat_boost) * bright_boost;
            g = (lum + (g - lum) * sat_boost) * bright_boost;
            b = (lum + (b - lum) * sat_boost) * bright_boost;
        }
        if (r > 255) r = 255;
        if (r < 0) r = 0;
        if (g > 255) g = 255;
        if (g < 0) g = 0;
        if (b > 255) b = 255;
        if (b < 0) b = 0;
        out_R[i] = (float)(r / 255.0);
        out_G[i] = (float)(g / 255.0);
        out_B[i] = (float)(b / 255.0);
    }

    return step_write_output_owned(ctx, "deshadowed-ortho-rgb-rasters", out_buf,
                                   raster_total_bytes(cols, rows, 3));
}

static const step_read_t reads_arr[] = {
    {.name = "ortho-rgb-rasters"},
    {.name = "ortho-irc-rasters"},
    {.name = "fitted-shadow-mask"},
};
static const step_write_t writes_arr[] = {
    {.name = "deshadowed-ortho-rgb-rasters", .kind = AK_RASTER_F32, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t deshadow_ortho_rgb_def = {
    .name = "deshadow-ortho-rgb",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
