// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-canary
// Reads:  per-building-records (touched but not actually consumed)
// Writes: v7-canary  (OPAQUE_JSON — `{"ok":true,"v7":"linked",...}`)
//
// Phase 5.0 infrastructure check. Proves that:
//   1. v3 CMake successfully links the v7 step library.
//   2. The C-step exec function can read inputs and emit JSON.
//   3. The DIORAMAP_V7 gate controls whether the v7 lib is built/linked.
//      v7 is the product roof-recon port, so it defaults ON and DOES ship
//      in production (unlike the research-track polyfit/ksr/roofer libs,
//      which are gated separately on DIORAMAP_RESEARCH=OFF). When DIORAMAP_V7
//      is off, the canary doesn't appear in the binary's symbol table.
//
// Real port steps (v7-tile-signals, v7-hemisphere-segment, …) land in
// subsequent commits, each with a per-step reference comparison against
// bench-v2 to verify numerical equivalence. This canary stays in the
// registry as a permanent build-health sentinel, in line with the
// polyfit-canary / ksr-canary / roofer-canary pattern.

#include <stdio.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"

static int v7_canary_exec(step_context_t* ctx) {
    size_t len = 0;
    const void* in = step_read(ctx, "per-building-records", &len);
    if (!in) return -1;

    const char* msg =
        "{\"ok\":true,\"v7\":\"linked\",\"phase\":\"5.0\",\"step\":\"v7-canary\"}";
    return step_write_output(ctx, "v7-canary", msg, strlen(msg));
}

static const step_read_t v7_canary_reads[] = {
    {.name = "per-building-records", .array = true},
};
static const step_write_t v7_canary_writes[] = {
    {.name = "v7-canary", .kind = AK_OPAQUE_JSON, .visibility = STEP_VIS_DEBUG},
};

const step_def_t v7_canary_def = {
    .name = "v7-canary",
    .version = 1,
    .reads = v7_canary_reads,
    .n_reads = 1,
    .writes = v7_canary_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_canary_exec,
};
