// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// find-best-sun-time
// Reads:  lidar-rasters (uses MAX_Z channel),
//         photo-shadow-mask (single-channel F32),
//         canonical-location (JSON — lat/lon).
// Params: { "year": int (default 2024) — year to search within (the
//                                        ortho's capture year),
//           "cell_size_m": float (default 0.5),
//           "epsilon_m":   float (default 0.1),
//           "altitude_floor": float (default 0.05) — skip candidates whose
//                                                    sun is below this many
//                                                    radians (≈3°). Matches
//                                                    findBestSunTime in JS. }
// Writes:
//   sun-direction       — opaque JSON {sun_dx,sun_dy,sun_dz, year/month/
//                         day/hour/minute, score, candidates_evaluated}
//   fitted-shadow-mask  — F32 single-channel raster cast at the winning
//                         sun direction (saves the deshadow step from
//                         re-running the cast)
//
// Algorithm (port of bench-v2 viewer/backend/preprocess/shadow-fit.js
// findBestSunTime, see PLAN-VIEWER-PORT §11 B8):
//   1. Coarse pass — month ∈ {Mar..Oct}, day ∈ {1, 15}, hour ∈ {9..17},
//      minute ∈ {0, 30}. 16 days × 18 times = 288 candidates max.
//   2. Fine pass — ±7 days × ±15 minutes around the coarse winner,
//      day step 1, minute step 5. 15 × 7 = 105 candidates.
//   3. Score — soft precision (intersection / synth-sum). Fast path:
//      candidates with sun below altitude_floor skipped without casting.
//   4. After winner, recast at that direction and emit fitted-shadow-mask.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/step.h"

#include "../registration/raster_format.h"
#include "../registration/registration-steps.h"
#include "shadow-cast-kernel.h"
#include "shadow-steps.h"
#include "sun-position.h"

typedef struct fbs_params {
    int year;
    double cell_size_m;
    double epsilon_m;
    double altitude_floor;
} fbs_params_t;

static int parse_params(const char* s, fbs_params_t* o) {
    o->year = 2024;
    o->cell_size_m = 0.5;
    o->epsilon_m = 0.1;
    o->altitude_floor = 0.05;
    if (!s) return 0;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return 0;
    json_t* j;
    j = json_object_get(p, "year");
    if (json_int_or_default(j, 1900, 2100, "year", &o->year) != 0) {
        json_decref(p); return -1;
    }
    j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) o->cell_size_m = v;
    }
    j = json_object_get(p, "epsilon_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v >= 0 && v < 100.0) o->epsilon_m = v;
    }
    j = json_object_get(p, "altitude_floor");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v >= 0 && v < 1.5) o->altitude_floor = v;
    }
    json_decref(p);
    return 0;
}

// Soft precision: of the cells flagged in the synthetic mask, what fraction
// are also flagged in the photo mask. Matches softPrecision in shadow-fit.js
// — see that file's docstring for why this metric vs IoU.
static double soft_precision(const float* synth, const float* photo, size_t n) {
    double inter = 0;
    double synth_sum = 0;
    for (size_t i = 0; i < n; i++) {
        double s = synth[i];
        double p = photo[i];
        inter += s < p ? s : p;
        synth_sum += s;
    }
    if (synth_sum < 10.0) return 0.0;
    return inter / synth_sum;
}

// Days in a Gregorian month (1-indexed). Month-end clamp helper for
// fine-pass day arithmetic — we add days to a base date and want a clean
// (y, m, d) tuple back, leap-year aware.
static int days_in_month(int year, int month) {
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0;
        return 28 + leap;
    }
    return dim[month - 1];
}

// Add `days` days (can be negative) and `minutes` minutes to (y, m, d, h, mi).
// Walks month/year boundaries cleanly. Output through pointers.
static void offset_date(int y, int m, int d, int h, int mi, int days, int minutes,
                        int* oy, int* om, int* od, int* oh, int* omin) {
    long total_min = (long)mi + (long)minutes;
    long add_h = total_min / 60;
    long new_min = total_min - add_h * 60;
    if (new_min < 0) { new_min += 60; add_h -= 1; }
    long total_h = (long)h + add_h;
    long add_d = total_h / 24;
    long new_h = total_h - add_d * 24;
    if (new_h < 0) { new_h += 24; add_d -= 1; }
    long new_d = (long)d + add_d + (long)days;

    while (new_d < 1) {
        m -= 1;
        if (m < 1) { m = 12; y -= 1; }
        new_d += days_in_month(y, m);
    }
    while (1) {
        int dim = days_in_month(y, m);
        if (new_d <= dim) break;
        new_d -= dim;
        m += 1;
        if (m > 12) { m = 1; y += 1; }
    }
    *oy = y;
    *om = m;
    *od = (int)new_d;
    *oh = (int)new_h;
    *omin = (int)new_min;
}

typedef struct best_t {
    int year, month, day, hour, minute;
    double score;
    double sun_dx, sun_dy, sun_dz;
    int candidates_evaluated;
} best_t;

// Returns 1 if a candidate was scored (and may have updated *best), 0 if
// the candidate was skipped (sun below floor) without scoring.
static int evaluate_candidate(int y, int mo, int d, int h, int mi,
                              double lat, double lon, fbs_params_t* p,
                              const float* maxZ, uint32_t cols, uint32_t rows,
                              const float* photo_mask, float* scratch,
                              best_t* best) {
    best->candidates_evaluated++;
    double u = sun_unix_from_utc(y, mo, d, h, mi);
    sun_position_t sp = sun_position_at(u, lat, lon);
    double dir[3];
    sun_dir_l93(sp, dir);
    if (dir[2] <= p->altitude_floor) {
        return 0;
    }
    cast_shadow_grid(maxZ, cols, rows, dir[0], dir[1], dir[2],
                     p->cell_size_m, p->epsilon_m, scratch);
    double score = soft_precision(scratch, photo_mask, (size_t)cols * rows);
    if (score > best->score) {
        best->score = score;
        best->year = y;
        best->month = mo;
        best->day = d;
        best->hour = h;
        best->minute = mi;
        best->sun_dx = dir[0];
        best->sun_dy = dir[1];
        best->sun_dz = dir[2];
    }
    return 1;
}

static int exec_fn(step_context_t* ctx) {
    fbs_params_t params;
    parse_params(step_get_params(ctx), &params);

    // canonical-location → lat/lon
    size_t loc_len = 0;
    const char* loc_bytes = (const char*)step_read(ctx, "canonical-location", &loc_len);
    if (!loc_bytes) return -1;
    json_error_t je;
    json_t* loc = json_loadb(loc_bytes, loc_len, 0, &je);
    if (!loc) return -1;
    json_t* j_lat = json_object_get(loc, "lat");
    json_t* j_lon = json_object_get(loc, "lon");
    if (!json_is_number(j_lat) || !json_is_number(j_lon)) {
        json_decref(loc);
        return -1;
    }
    double lat = json_number_value(j_lat);
    double lon = json_number_value(j_lon);
    json_decref(loc);

    // lidar-rasters → MAX_Z channel
    size_t lr_len = 0;
    const uint8_t* lr_in = (const uint8_t*)step_read(ctx, "lidar-rasters", &lr_len);
    if (!lr_in) return -1;
    raster_view_t lr;
    if (raster_view_open(lr_in, lr_len, &lr) != 0 || lr.n_channels < LIDAR_RC__COUNT) {
        return -1;
    }
    uint32_t cols = lr.cols;
    uint32_t rows = lr.rows;
    const float* maxZ = lr.channels[LIDAR_RC_MAX_Z];

    // photo-shadow-mask → 1-channel F32
    size_t ps_len = 0;
    const uint8_t* ps_in = (const uint8_t*)step_read(ctx, "photo-shadow-mask", &ps_len);
    if (!ps_in) return -1;
    raster_view_t ps;
    if (raster_view_open(ps_in, ps_len, &ps) != 0 || ps.n_channels < 1) {
        return -1;
    }
    if (ps.cols != cols || ps.rows != rows) return -1;
    const float* photo_mask = ps.channels[0];

    size_t cells = (size_t)cols * rows;
    float* scratch = (float*)safe_malloc_array(cells, sizeof(float));
    if (!scratch) return -1;

    best_t best = {
        .year = params.year, .month = 6, .day = 15, .hour = 12, .minute = 0,
        .score = -1.0,
        .sun_dx = 0, .sun_dy = 0, .sun_dz = 1,
        .candidates_evaluated = 0,
    };

    // ---- Coarse pass ----
    static const int coarse_months[] = {3, 4, 5, 6, 7, 8, 9, 10};
    static const int coarse_days[] = {1, 15};
    static const int coarse_hours[] = {9, 10, 11, 12, 13, 14, 15, 16, 17};
    static const int coarse_minutes[] = {0, 30};
    for (size_t mi = 0; mi < sizeof(coarse_months) / sizeof(int); mi++) {
        for (size_t di = 0; di < sizeof(coarse_days) / sizeof(int); di++) {
            for (size_t hi = 0; hi < sizeof(coarse_hours) / sizeof(int); hi++) {
                for (size_t qi = 0; qi < sizeof(coarse_minutes) / sizeof(int); qi++) {
                    evaluate_candidate(params.year, coarse_months[mi], coarse_days[di],
                                       coarse_hours[hi], coarse_minutes[qi],
                                       lat, lon, &params, maxZ, cols, rows,
                                       photo_mask, scratch, &best);
                }
            }
        }
    }

    // ---- Fine pass ----
    // ±7 days × ±15 min around best, day step 1, min step 5.
    if (best.score >= 0) {
        for (int dd = -7; dd <= 7; dd++) {
            for (int dm = -15; dm <= 15; dm += 5) {
                int y, mo, d, h, mi;
                offset_date(best.year, best.month, best.day, best.hour, best.minute,
                            dd, dm, &y, &mo, &d, &h, &mi);
                evaluate_candidate(y, mo, d, h, mi, lat, lon, &params,
                                   maxZ, cols, rows, photo_mask, scratch, &best);
            }
        }
    }

    // ---- Cast at winner for the fitted-shadow-mask output ----
    // (Note: the scratch buffer already holds the cast for whatever the
    // last candidate was, not the winner. Recast explicitly.)
    uint8_t* fitted_buf = raster_alloc(cols, rows, 1);
    if (!fitted_buf) {
        free(scratch);
        return -1;
    }
    float* fitted = raster_channel_ptr(fitted_buf, cols, rows, 0);
    if (best.score >= 0) {
        cast_shadow_grid(maxZ, cols, rows, best.sun_dx, best.sun_dy, best.sun_dz,
                         params.cell_size_m, params.epsilon_m, fitted);
    } else {
        // Pathological scene: no daylight candidates above floor. Mark all
        // lit so downstream deshadow degrades to a no-op.
        for (size_t i = 0; i < cells; i++) fitted[i] = 0.0f;
    }
    free(scratch);

    if (step_write_output_owned(ctx, "fitted-shadow-mask", fitted_buf,
                                raster_total_bytes(cols, rows, 1)) != 0) {
        return -1;
    }

    char json_buf[512];
    int n = snprintf(json_buf, sizeof(json_buf),
                     "{\"sun_dx\":%.6f,\"sun_dy\":%.6f,\"sun_dz\":%.6f,"
                     "\"year\":%d,\"month\":%d,\"day\":%d,\"hour\":%d,\"minute\":%d,"
                     "\"score\":%.6f,\"candidates_evaluated\":%d}",
                     best.sun_dx, best.sun_dy, best.sun_dz,
                     best.year, best.month, best.day, best.hour, best.minute,
                     best.score >= 0 ? best.score : 0.0, best.candidates_evaluated);
    if (n < 0 || (size_t)n >= sizeof(json_buf)) return -1;
    return step_write_output(ctx, "sun-direction", json_buf, (size_t)n);
}

static const step_read_t reads_arr[] = {
    {.name = "canonical-location"},
    {.name = "lidar-rasters"},
    {.name = "photo-shadow-mask"},
};
static const step_write_t writes_arr[] = {
    {.name = "fitted-shadow-mask", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
    {.name = "sun-direction", .kind = AK_OPAQUE_JSON, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t find_best_sun_time_def = {
    .name = "find-best-sun-time",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 2,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};
