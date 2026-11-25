// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_FETCHERS_H
#define V3_STEPS_FETCHERS_H

#include "v3/step.h"

extern const step_def_t fetch_ortho_rgb_def;
extern const step_def_t fetch_ortho_irc_def;
extern const step_def_t fetch_elevation_def;
extern const step_def_t fetch_cadastre_parcels_def;
extern const step_def_t fetch_cadastre_buildings_def;
extern const step_def_t fetch_ortho_surround_def;
extern const step_def_t fetch_elevation_surround_def;

// Register all fetcher steps. Each step internally resolves a fetch_client_t
// via a process-global handle (created on first call, destroyed on
// fetchers_shutdown). The runner and main.c are unchanged — fetchers look
// like any other step from their perspective.
int fetchers_register_steps(void);

// Fail the step with UPSTREAM_FORMAT when a 2xx response body is not what
// was asked for (e.g. IGN WMS answers HTTP 200 + an XML ServiceException
// instead of a JPEG). Puts the URL, size and a printable excerpt of the
// body into the failure detail. Always returns -1.
int fetchers_fail_bad_body(step_context_t* ctx, const char* what, const char* url,
                           const uint8_t* body, size_t len, const char* expected);

// True when the buffer starts with a JPEG SOI marker.
static inline bool fetchers_is_jpeg(const uint8_t* b, size_t n) {
    return n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
}

// True when the buffer's first non-whitespace byte is '{'.
static inline bool fetchers_is_json_object(const uint8_t* b, size_t n) {
    size_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t')) i++;
    return i < n && b[i] == '{';
}

// Release the shared fetch client on process teardown. Idempotent.
void fetchers_shutdown(void);

#endif
