// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// CUDA-readiness canary (§7.6 of UNIFIED-ARCHITECTURE.md).
//
// The step copies its input artifact to its output, byte-for-byte.
//
// - In a CPU-only build, the exec path is a host memcpy. The step's
//   compute_location is STEP_CL_CPU.
// - With -DDIORAMAP_CUDA=ON, a sibling gpu-smoke.cu compiles an identity
//   CUDA kernel. The exec path allocates a device buffer, does H2D memcpy,
//   launches the kernel, does D2H memcpy, and verifies the round trip. The
//   step's compute_location becomes STEP_CL_GPU.
//
// Existence of this step guarantees every piece of the GPU wiring (CMake
// flag, NVCC availability, linking, device allocation, transfers, kernel
// launch) is exercised at build/test time. Real GPU algorithm ports land
// in Phase 9+ and follow the same pattern.

#include "gpu-smoke.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef DIORAMAP_CUDA
// Defined in gpu-smoke.cu
extern int gpu_smoke_cuda_identity(const void* in, size_t n, void* out);
#endif

static int exec_fn(step_context_t* ctx) {
    size_t n = 0;
    const void* in = step_read(ctx, "smoke-input", &n);
    if (!in || n == 0) {
        return -1;
    }
    void* out = malloc(n);
    if (!out) {
        return -1;
    }
#ifdef DIORAMAP_CUDA
    if (gpu_smoke_cuda_identity(in, n, out) != 0) {
        free(out);
        return -1;
    }
#else
    memcpy(out, in, n);
#endif
    int rc = step_write_output(ctx, "smoke-output", out, n);
    free(out);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = "smoke-input"}};
static const step_write_t writes_arr[] = {
    {.name = "smoke-output",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t gpu_smoke_def = {
    .name = "gpu-smoke",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
#ifdef DIORAMAP_CUDA
    .compute_location = STEP_CL_GPU,
#else
    .compute_location = STEP_CL_CPU,
#endif
    .streaming = false,
    .exec = exec_fn,
};

// Minimal "fetcher" helper used by the smoke pipeline to turn the
// pipeline-input artifact into "smoke-input". Every pipeline needs at
// least one step that reads pipeline-input and produces named artifacts
// for downstream steps; this is that step.
static int smoke_input_exec(step_context_t* ctx) {
    size_t n = 0;
    const void* in = step_read(ctx, PIPELINE_INPUT_ARTIFACT, &n);
    if (!in || n == 0) {
        return -1;
    }
    return step_write_output(ctx, "smoke-input", in, n);
}

static const step_read_t si_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t si_writes[] = {
    {.name = "smoke-input",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

static const step_def_t smoke_input_def = {
    .name = "smoke-input-from-run-input",
    .version = 1,
    .reads = si_reads,
    .n_reads = 1,
    .writes = si_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = smoke_input_exec,
};

int gpu_smoke_register(void) {
    if (step_register(&smoke_input_def) != 0) {
        return -1;
    }
    if (step_register(&gpu_smoke_def) != 0) {
        return -1;
    }
    return 0;
}
