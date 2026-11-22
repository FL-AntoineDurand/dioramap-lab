// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Thin PROJ wrapper. We use PROJ's context-per-thread API (`PJ_CONTEXT`)
// plus a per-thread transformer handle cached via pthread_key_t so each
// thread does its proj_create_crs_to_crs once and re-uses it.
//
// PROJ's transformer handles are NOT thread-safe — two threads must not
// share a PJ*. The per-thread cache solves that without locking.

#include "v3/coords.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include <proj.h>

typedef struct thread_ctx {
    PJ_CONTEXT* ctx;
    PJ* fwd; // WGS84 → L93
    PJ* inv; // L93 → WGS84
} thread_ctx_t;

static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

static void destroy_thread_ctx(void* p) {
    thread_ctx_t* tc = p;
    if (!tc)
        return;
    if (tc->fwd)
        proj_destroy(tc->fwd);
    if (tc->inv)
        proj_destroy(tc->inv);
    if (tc->ctx)
        proj_context_destroy(tc->ctx);
    free(tc);
}

static int g_key_ok = 0;

static void init_key(void) {
    // Frama-C audit finding COORD-1: pthread_key_create can fail (EAGAIN
    // when PTHREAD_KEYS_MAX is exceeded, ENOMEM). If it does, `g_key`
    // stays undefined and pthread_getspecific(g_key) is UB. Record the
    // outcome so get_thread_ctx can bail safely.
    g_key_ok = (pthread_key_create(&g_key, destroy_thread_ctx) == 0);
}

static thread_ctx_t* get_thread_ctx(void) {
    pthread_once(&g_key_once, init_key);
    if (!g_key_ok) {
        return NULL; // COORD-1: pthread_key_create failed during init.
    }
    thread_ctx_t* tc = pthread_getspecific(g_key);
    if (tc)
        return tc;
    tc = calloc(1, sizeof(*tc));
    if (!tc)
        return NULL;
    tc->ctx = proj_context_create();
    if (!tc->ctx) {
        free(tc);
        return NULL;
    }
    // PROJ's from-definition API takes EPSG codes directly.
    tc->fwd = proj_create_crs_to_crs(tc->ctx, "EPSG:4326", "EPSG:2154", NULL);
    tc->inv = proj_create_crs_to_crs(tc->ctx, "EPSG:2154", "EPSG:4326", NULL);
    if (!tc->fwd || !tc->inv) {
        destroy_thread_ctx(tc);
        return NULL;
    }
    // Normalize axis order so we always feed (lon, lat) regardless of CRS
    // definition quirks.
    PJ* normed = proj_normalize_for_visualization(tc->ctx, tc->fwd);
    if (normed) {
        proj_destroy(tc->fwd);
        tc->fwd = normed;
    }
    normed = proj_normalize_for_visualization(tc->ctx, tc->inv);
    if (normed) {
        proj_destroy(tc->inv);
        tc->inv = normed;
    }
    // COORD-2: pthread_setspecific can fail (ENOMEM, EINVAL). If it does,
    // we'd leak `tc` plus its two PROJ transformer handles on the next
    // call (because the next get_thread_ctx would see no TLS value and
    // allocate again). Clean up immediately and signal failure.
    if (pthread_setspecific(g_key, tc) != 0) {
        destroy_thread_ctx(tc);
        return NULL;
    }
    return tc;
}

int wgs84_to_l93(double lon_deg, double lat_deg, double* out_x, double* out_y) {
    thread_ctx_t* tc = get_thread_ctx();
    if (!tc)
        return -1;
    PJ_COORD in = proj_coord(lon_deg, lat_deg, 0, 0);
    PJ_COORD out = proj_trans(tc->fwd, PJ_FWD, in);
    if (out.xy.x == HUGE_VAL || out.xy.y == HUGE_VAL)
        return -1;
    *out_x = out.xy.x;
    *out_y = out.xy.y;
    return 0;
}

int l93_to_wgs84(double x, double y, double* out_lon_deg, double* out_lat_deg) {
    thread_ctx_t* tc = get_thread_ctx();
    if (!tc)
        return -1;
    PJ_COORD in = proj_coord(x, y, 0, 0);
    PJ_COORD out = proj_trans(tc->inv, PJ_FWD, in);
    if (out.xy.x == HUGE_VAL || out.xy.y == HUGE_VAL)
        return -1;
    *out_lon_deg = out.xy.x;
    *out_lat_deg = out.xy.y;
    return 0;
}
