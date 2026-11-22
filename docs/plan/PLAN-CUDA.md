# CUDA Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). GPU runtime and the `gpu-smoke` canary step.*

## Scope

- **CMake CUDA integration** behind `-DDIORAMAP_CUDA=ON`.
- **CUDA context lifecycle**: init at startup, shared across threads via one primary context per device.
- **Per-thread streams**: each compute thread owns a dedicated CUDA stream.
- **Pinned host allocator**: `cudaHostAlloc` for heap buffers on the GPU fast path.
- **Device buffer pool**: `cuMemPool` for artifact allocations; reuse across runs.
- **`gpu-smoke` step**: the canary validating the whole GPU path end-to-end.
- **Placement transfer primitives** for the runner (§7.4).

## Key decisions

1. **Compile-time opt-in**. No runtime fallback. CPU-only and CUDA builds are separate binaries; no branching inside the binary based on GPU presence.
2. **One primary context per GPU**, shared via CUDA's driver-level context stack. Threads acquire on use.
3. **One stream per compute thread**. Concurrent pipelines overlap naturally on the device without explicit scheduling.
4. **Pin on first H2D use**, not universally. Heap buffers allocated via the standard allocator become pinned when they first participate in a transfer. Less total pinned memory; small latency cost on first transfer.
5. **Device memory via `cuMemPool`** (async mempool). Fast allocate/free, reuse across runs.
6. **gpu-smoke stays forever**. It's the canary; removing it would hide regressions.

## Data structures

### CUDA init/teardown

```c
#ifdef DIORAMAP_CUDA
typedef struct cuda_runtime cuda_runtime_t;

cuda_runtime_t* cuda_init(cuda_config_t);
void cuda_shutdown(cuda_runtime_t*);

typedef struct cuda_config {
    int device_id;        // default 0; -1 → auto-select
    size_t mempool_initial_bytes;
    size_t mempool_max_bytes;
    bool verbose;
} cuda_config_t;
#endif
```

Called once at process startup in CUDA builds. On failure (no GPU, driver mismatch): process exits with a clear error (no silent degradation per D12 of arch doc).

### Per-thread stream

Each compute thread calls `cuda_acquire_stream()` on first use; the stream is a thread-local. On thread shutdown, stream is destroyed.

```c
#ifdef DIORAMAP_CUDA
cudaStream_t cuda_acquire_stream(cuda_runtime_t*);
void cuda_release_stream_on_thread_exit(void);
#endif
```

### Transfer primitives

```c
#ifdef DIORAMAP_CUDA
typedef struct cuda_device_buf {
    CUdeviceptr ptr;
    size_t size;
    // internal: pool handle for return
} cuda_device_buf_t;

cuda_device_buf_t cuda_alloc_device(cuda_runtime_t*, size_t);
void cuda_free_device(cuda_device_buf_t);  // returns to pool

result_t cuda_copy_h2d(cuda_device_buf_t dst, const void* src, size_t n, cudaStream_t);
result_t cuda_copy_d2h(void* dst, cuda_device_buf_t src, size_t n, cudaStream_t);
result_t cuda_copy_d2d(cuda_device_buf_t dst, cuda_device_buf_t src, size_t n, cudaStream_t);

void cuda_sync_stream(cudaStream_t);

// Pin an existing heap buffer (registers for fast transfers)
result_t cuda_pin_host(void* buf, size_t size);
void cuda_unpin_host(void* buf);
#endif
```

These are called by the runner's placement transfer logic (§7.4 of arch doc).

### gpu-smoke step shape

```c
// steps/gpu-smoke/gpu-smoke.c — CPU impl (always compiled)
#include "v3/step.h"

static const char* reads[] = {"smoke-input"};
static const step_write_t writes[] = {
    {.name="gpu-identity", .kind=KIND_PER_POINT_ARRAY, .visibility=VIS_DEBUG}
};

#ifdef DIORAMAP_CUDA
extern result_t gpu_smoke_cuda(const artifact_view_t* in, artifact_builder_t* out, cudaStream_t);
#endif

static result_t gpu_smoke_cpu(const artifact_view_t* in, artifact_builder_t* out) {
    // memcpy equivalent: read in.column("x"), write out.column("x")
    return OK;
}

const step_def_t gpu_smoke_step = {
    .name = "gpu-smoke",
    .version = 1,
    .reads = reads,
    .n_reads = 1,
    .writes = writes,
    .n_writes = 1,
#ifdef DIORAMAP_CUDA
    .compute_location = COMPUTE_LOCATION_GPU,
    .exec = gpu_smoke_cuda_entry,  // wraps gpu_smoke_cuda with runner glue
#else
    .compute_location = COMPUTE_LOCATION_CPU,
    .exec = gpu_smoke_cpu_entry,
#endif
    .streaming = false,
};
```

```cu
// steps/gpu-smoke/gpu-smoke.cu — CUDA impl (built only when -DDIORAMAP_CUDA=ON)
__global__ void identity_kernel(const float* in, float* out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = in[i];
}

extern "C" result_t gpu_smoke_cuda(const artifact_view_t* in, artifact_builder_t* out, cudaStream_t stream) {
    int n = in->header->count_n;
    const float* src = artifact_view_column(in, "x");
    // alloc device input + output
    cuda_device_buf_t d_in  = cuda_alloc_device(cuda_rt, n * sizeof(float));
    cuda_device_buf_t d_out = cuda_alloc_device(cuda_rt, n * sizeof(float));
    cuda_copy_h2d(d_in, src, n * sizeof(float), stream);

    dim3 block(256);
    dim3 grid((n + 255) / 256);
    identity_kernel<<<grid, block, 0, stream>>>((float*)d_in.ptr, (float*)d_out.ptr, n);

    float* out_host = artifact_build_add_column_raw(out, "x", DTYPE_F32, n);
    cuda_copy_d2h(out_host, d_out, n * sizeof(float), stream);
    cuda_sync_stream(stream);

    cuda_free_device(d_in);
    cuda_free_device(d_out);
    return OK;
}
```

## Status (2026-04-28)

Only the gpu-smoke canary (commit `bd00bc5`) shipped. All other GPU runtime infrastructure (init, streams, mempool, transfer primitives, startup self-test, fusion docs) is unimplemented. The v3 binary builds in CPU-only mode by default; CUDA targets compile only when `-DDIORAMAP_CUDA=ON` and only the canary kernel exists. No `core/src/cuda/` directory; the `.cu` source lives at `v3/steps/gpu-smoke/gpu-smoke.cu` and uses raw `cudaMalloc` / `cudaMemcpy` directly.

| Task | State | Notes |
|---|---|---|
| C1 CMake CUDA language support | ⚠️ partial | `enable_language(CUDA)` + `find_package(CUDAToolkit)` wired in `core/CMakeLists.txt` behind `-DDIORAMAP_CUDA=ON`. No GPU-equipped CI job; `sm_70` arch flag not yet pinned. |
| C2 CUDA init/shutdown | ❌ not started | No `cuda_init` / `cuda_runtime_t`; canary calls cudaMalloc/cudaFree per invocation. |
| C3 Per-thread stream | ❌ not started | Canary uses default stream + `cudaDeviceSynchronize`. |
| C4 Device mempool | ❌ not started | No `cuMemPool` wrapper. |
| C5 Transfer primitives | ❌ not started | No `cuda_copy_*` / `cuda_pin_host` helpers. |
| C6 gpu-smoke CPU impl | ✅ done (`bd00bc5`) | Host memcpy of input bytes; runs in CPU-only build. |
| C7 gpu-smoke CUDA kernel + wrapper | ✅ done (`bd00bc5`) | `identity_kernel` + `gpu_smoke_cuda_identity` in `steps/gpu-smoke/gpu-smoke.cu`. |
| C8 gpu-smoke integration into runner | ✅ done (`bd00bc5`) | Step registered in both build configs; `batch_gpu_smoke` ctest passes (CPU path). CUDA path validated only at build time — no GPU CI runner yet. |
| C9 Startup self-test | ❌ not started | Canary is run via the normal pipeline; no startup self-test. |
| C10 Kernel fusion readiness | ❌ not started | No documentation pattern shipped. |

## Implementation tasks (ordered)

### C1. CMake CUDA language support ⚠️ partial
- `enable_language(CUDA)` inside `DIORAMAP_CUDA` guard.
- `find_package(CUDAToolkit)`.
- Minimum CUDA arch target (`--gpu-architecture=sm_70` for broad compat, adjustable).
- CI job builds with CUDA on a GPU-equipped runner.

### C2. CUDA init/shutdown ❌ not started
- `cuda_init` picks device, creates mempool, creates a warm-up stream.
- Startup log events emitted.
- Graceful error on no GPU / driver mismatch.

### C3. Per-thread stream ❌ not started
- Thread-local stream handle.
- Acquire/release paired with thread pool worker lifecycle.
- Unit test: multi-thread concurrent stream use.

### C4. Device mempool ❌ not started
- Wrap `cuMemPool` for allocate/free.
- Pool-level stats.
- Unit test: allocation churn, pool reuse.

### C5. Transfer primitives ❌ not started
- `cuda_copy_h2d/d2h/d2d` with async stream.
- `cuda_pin_host` / `cuda_unpin_host`.
- Unit test: round-trip H2D+D2H matches input bytes.

### C6. gpu-smoke CPU impl ✅ done (commit `bd00bc5`)
- Trivial memcpy of input column.
- Golden output written.
- Runs in CPU-only build.

### C7. gpu-smoke CUDA kernel + wrapper ✅ done (commit `bd00bc5`)
- Kernel as shown above.
- Build in CUDA build only.
- Integration test: CUDA build produces bit-identical output to CPU build.

### C8. gpu-smoke integration into runner ✅ done (commit `bd00bc5`)
- Step registration in both build configs.
- Runner's placement transfer table includes device paths (requires C5).
- End-to-end test: batch-mode run of `smoke/three-step-smoke` produces `gpu-identity` correctly.

### C9. Startup self-test (optional) ❌ not started
- In CUDA build, run gpu-smoke once at startup with fixed input.
- If mismatch or error: abort process with loud log.
- Skip in dev builds for fast startup.

### C10. Kernel fusion readiness (infrastructure only, 0 LOC) ❌ not started
- Document pattern: a single native function may fulfill multiple step names if it writes all declared outputs.
- No code needed Phase A; this is a notation for Phase 9+ GPU algorithm ports.

## Dependencies

- **Before**: logging (cuda events), runtime primitives (thread pool with thread-local stream slot), store (gpu-smoke uses store_write), runner (gpu-smoke registered in step registry).
- **After**: Phase 9 GPU algorithm ports follow the gpu-smoke pattern.

## Definition of done

- CUDA build succeeds on CI GPU runner.
- `gpu-smoke` produces bit-identical output in CPU-only and CUDA builds.
- Integration test: full smoke pipeline with gpu-smoke as the third step passes in CUDA build.
- `cuda_init` failure modes tested: no GPU (process exits loudly), OOM during pool init (logs + exits).
- Concurrent multi-thread stream use is TSan-clean.
- Process shutdown releases all CUDA resources cleanly (no leaks reported by `cuda-memcheck`).
- **Integration matrix coverage** (per [docs/quality/README.md](docs/quality/README.md)): no dedicated GPU scenarios in the matrix today (out of scope for the WSL2 dev box and the CPU-only prod TSan validation).  When the CUDA workstream resumes, mirror the existing `serve_concurrent_clients` shape with a CUDA-pipeline equivalent and add it under a new `gpu-cuda` build variant.  Until then, `cuda-memcheck` runs are gated on the dedicated GPU CI runner (docs/quality/README.md doesn't list it because the matrix focuses on CPU-side concurrency / memory).

## Open questions

- **Multi-GPU.** `cudaSetDevice` per thread for multi-GPU boxes. Defer; current design assumes one GPU.
- **Unified memory** (`cudaMallocManaged`). Tempting but performance-unpredictable for our access patterns. Stick to explicit H2D/D2H.
- **CUDA graph capture.** Potential future optimization for small-kernel pipelines. Defer.

## Risk / unknowns

- **CUDA driver / toolkit version skew.** CI runner may have different CUDA version than dev machines. Pin toolkit version in CMakeLists; document min driver.
- **Pinned memory exhaustion.** `cudaHostRegister` can fail if too much memory is pinned. Add monitoring; cap total pinned bytes.
- **Stream sync semantics.** `cudaStreamSynchronize` blocks calling thread; CPU-side concurrent work during GPU work is lost. For gpu-smoke this doesn't matter, but for real GPU steps in Phase 9, design will need to overlap when possible. Not Phase A concern.
