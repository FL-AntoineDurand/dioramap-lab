// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_EXTENSION_H
#define V3_EXTENSION_H

// Processor extension: the seam for deployment-specific behaviour that
// does not belong in the engine (docs/EXTENSIONS.md).
//
// A hosted service usually needs three things the lab build does not:
//
//   1. authenticate each WebSocket connection against its own accounts
//      (API keys, allowed origins, plans);
//   2. limit what a connection may see (e.g. only the artifacts marked
//      public in the step definitions);
//   3. be told about every finished run (usage metering, billing, support).
//
// An extension provides those as callbacks. It is selected at LINK time:
// configure with `-DDIORAMAP_EXTENSION_DIR=<dir>`, where <dir> has a
// CMakeLists.txt defining a static library target `dioramap_extension` that
// implements v3_extension(). Without it the build links
// src/extension/none.c, which returns NULL (no extension: the lab
// behaviour, optionally gated by --token).
//
// Threading: authenticate() runs on the libwebsockets service thread while
// a connection is being established; keep it fast or cache (it blocks other
// connections meanwhile). on_run_finished() runs on the run's worker thread
// after the terminal frame is queued; it must not block for long either —
// buffer and flush in the background.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a connection may see.
typedef enum v3_access {
    V3_ACCESS_FULL = 0,        // every artifact (lab default)
    V3_ACCESS_PUBLIC_ONLY = 1, // only artifacts whose writer is STEP_VIS_PUBLIC
} v3_access_t;

typedef struct v3_ext_env {
    const char* cache_dir;          // processor cache root (writable)
    const char* processor_version;  // DIORAMAP_VERSION
} v3_ext_env_t;

typedef struct v3_ext_auth_request {
    const char* credential; // value of ?<credential_param>=…, NULL when absent
    const char* origin;     // Origin header, "" when absent
} v3_ext_auth_request_t;

typedef struct v3_ext_auth_result {
    bool ok;
    // On refusal: wire error code ("AUTH_REQUIRED", "AUTH_FAILED", …) and a
    // one-line reason sent to the client. Defaults: AUTH_FAILED, "".
    char error_code[32];
    char reason[160];
    // On success. `principal` (e.g. the API key) and `account` (e.g. an org
    // id) are kept on the session and handed back in run reports.
    char principal[96];
    char account[64];
    v3_access_t access;
    // Optional label echoed as SESSION_STARTED.tier ("public", "internal").
    char tier_label[16];
} v3_ext_auth_result_t;

// One per finished run, success or failure.
typedef struct v3_run_report {
    const char* principal;      // "" when the connection was not authenticated by the extension
    const char* account;
    const char* event;          // "scene_load" (RUN) | "late_subscribe" (SUBSCRIBE)
    const char* run_id;
    const char* pipeline;       // may be NULL
    const char* input_hash;     // xxh3-128 hex, may be NULL (never the raw input)
    const char* test_case_label;// may be NULL
    bool has_latlon;
    double lat, lon;
    uint64_t duration_us;
    bool success;
    const char* error_code;     // NULL on success (docs/ERRORS.md codes)
    const char* error_message;  // NULL on success
    size_t n_executed;
    size_t n_cache_hits;
} v3_run_report_t;

typedef struct v3_extension {
    const char* name;              // shown in the serve startup line
    // URL query parameter carrying the client credential ("key" → ?key=…).
    // NULL → "key".
    const char* credential_param;

    // Called once by `serve` before listening. Returns the extension's state
    // (passed back to every callback), or NULL to refuse to start (print why
    // on stderr). May read its own environment variables.
    void* (*init)(const v3_ext_env_t* env);

    // NULL → every connection accepted with V3_ACCESS_FULL. `out` is
    // zero-initialised by the caller.
    void (*authenticate)(void* state, const v3_ext_auth_request_t* req,
                         v3_ext_auth_result_t* out);

    // NULL → not called.
    void (*on_run_finished)(void* state, const v3_run_report_t* report);

    // Called once at shutdown (flush buffers). NULL → not called.
    void (*shutdown)(void* state);
} v3_extension_t;

// Provided by the linked extension library; src/extension/none.c returns NULL.
const v3_extension_t* v3_extension(void);

#ifdef __cplusplus
}
#endif
#endif
