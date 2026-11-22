# Runner + Runtime Primitives Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). Subsystem plan for the runtime and step-execution machinery.*

## Scope

- **Runtime primitives**: thread pool, work queue, atomic refcount, state machines, mutex/condvar discipline.
- **Runner**: step registry, DAG walker, step dispatch, placement transfer orchestration.
- **Session management**: session object lifecycle, ref tracking, grace period handling.
- **Concurrency invariants**: ownership rules, race-freedom, TSan-clean.

## Key decisions

1. **Compute threads own one run at a time.** A compute thread executing steps for a run does not switch to another run mid-way. Simpler to reason about; slightly suboptimal for pipelines with wildly different step parallelism. Revisit if profiling shows compute-thread underutilization.
2. **Runner is not a thread.** It's a collection of queues and a dispatch function. The main event-loop thread pushes "produce X" tasks; compute threads pop and execute. No dedicated "runner thread" blocking on queues.
3. **DAG walk is lazy.** When a task comes in to produce artifacts `[A, B, C]`, walk backward from each. Collect the union of needed steps. Topologically sort. Skip any step whose outputs are all already in store (content-hash hit).
4. **Placement transfers happen on the compute thread**, not in a separate "transfer thread." Simpler. CUDA transfers use the thread's own stream.
5. **Session state lives in-process** (Phase A). Interface designed to be swappable for remote state later (§19 of architecture doc).
6. **Refcount semantics are explicit.** Every shared object documents its refcount owners. No implicit copies, no ambiguous lifetimes.

## Data structures

### Thread pool

```c
typedef struct thread_pool thread_pool_t;

thread_pool_t* pool_create(int n_threads);
void pool_submit(thread_pool_t*, void (*work)(void*), void* arg);
void pool_shutdown(thread_pool_t*);  // blocks until all work drained
```

Work queue is bounded (default 1024) with condvar signaling. Bounded queue acts as backpressure.

### State machines

Session, run, step-execution, and artifact each have an explicit state enum:

```
session_state: NEW → ACTIVE → DISCONNECTED (grace) → DESTROYED
run_state:     PENDING → PLANNING → EXECUTING → COMPLETE | FAILED | CANCELLED
step_state:    QUEUED → RUNNING → WRITING → DONE | FAILED
artifact_state: PRODUCING → AVAILABLE (→ EVICTED)
```

Transitions documented with a table; invalid transitions `abort()` in debug, log-error in release.

### Runner API (C)

```c
typedef struct runner runner_t;
typedef struct run_spec {
    pipeline_id_t pipeline;
    input_ref_t input;
    params_t params;
    const char* const* subscribe;
    size_t n_subscribe;
    session_t session;
} run_spec_t;

typedef struct run_handle run_handle_t;

runner_t* runner_create(runner_config_t);
run_handle_t* runner_start_run(runner_t*, const run_spec_t*);
void runner_cancel(runner_t*, run_handle_t*);
void runner_late_subscribe(runner_t*, run_handle_t*, const char* artifact_name);
void runner_shutdown(runner_t*);
```

Events flow out via a callback interface passed at `runner_create`:

```c
typedef struct runner_callbacks {
    void (*on_run_started)(void* ctx, run_handle_t*);
    void (*on_step_started)(void* ctx, run_handle_t*, const char* step);
    void (*on_progress)(void* ctx, run_handle_t*, const char* step, float pct);
    void (*on_artifact_ready)(void* ctx, run_handle_t*, const char* artifact, hash_t);
    void (*on_step_complete)(void* ctx, run_handle_t*, const char* step);
    void (*on_run_complete)(void* ctx, run_handle_t*);
    void (*on_error)(void* ctx, run_handle_t*, error_t);
    void* ctx;
} runner_callbacks_t;
```

Both the WS transport and the batch-mode sink implement these callbacks, so the runner is transport-agnostic.

### Step registry

```c
typedef struct step_def {
    const char* name;
    uint32_t version;
    const char* const* reads;
    size_t n_reads;
    const step_write_t* writes;
    size_t n_writes;
    compute_location_t compute_location;
    bool streaming;
    step_fn_t exec;
} step_def_t;

void step_register(const step_def_t*);  // called at startup by each step's registration
const step_def_t* step_lookup(const char* name);
```

Step registration happens via a linker-section trick (or a simple startup-list pattern): each step compiles a `step_def_t` into a known section; at startup a loop walks the section and calls `step_register`.

### DAG walker

Input: a pipeline definition (loaded from JSON) + a set of subscribed artifact names.

Output: a topologically-sorted list of steps to execute (skipping cached ones).

Algorithm: BFS from subscribed artifacts backward through the `reads` of each step's writer. Mark every touched step. Topo-sort by `writes → reads` dependency. Filter out steps whose `writes` are all cached (store lookup).

Errors:
- Cycle detection: step DAG must be acyclic; cycle is a config error at pipeline load.
- Missing producer: subscribed artifact has no step in the pipeline that writes it → error, reject RUN.
- Ambiguous producer: more than one step writes the same artifact name → reject at pipeline load.

### Session API (C)

> Sketch — early shape from the Phase A design.  The actual shipped
> API (post 2026-05-04 encapsulation refactor, commit `d91f0ab`)
> lives in `core/include/v3/session.h` + `core/include/v3/session_mgr.h`
> and uses opaque-pointer accessors with internal locking — see
> PLAN-RUNNER T9 status row + CONCURRENCY.md §12.6.

```c
typedef struct session_mgr session_mgr_t;
typedef struct session session_t;

session_mgr_t* session_mgr_create(session_config_t);
session_t* session_create(session_mgr_t*, tier_t);
session_t* session_lookup(session_mgr_t*, const token_t*);
void session_add_ref(session_t*, const hash_t*);  // pin an artifact
void session_touch(session_t*);  // update last-seen
void session_disconnect(session_t*);  // enter grace period
void session_reconnect(session_t*);   // leave grace period
void session_destroy(session_mgr_t*, session_t*);  // release all refs
```

Background janitor task walks sessions, destroys any past grace expiry.

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| T1 Bare thread pool | ✅ done (A3 + wired into runner, `11b1d9d` + `0a17665`) | Used for intra-pipeline step dispatch. |
| T2 Atomic refcount | ✅ done (A3, `11b1d9d`) | State-machine helper skipped; subsystems manage state inline. |
| T3 Step registry | ✅ done (A6, `17e932c`) | Static 256-entry array; thread-unsafe by design (startup-only). |
| T4 Pipeline loader | ✅ done (A6+, `16a83ab`) | JSON via jansson, with err_buf diagnostics. |
| T5 DAG walker | ✅ done (A6, `17e932c`) | Backward BFS + Kahn's topo sort. Cycle detection verified. |
| T6 Basic runner | ✅ done (A6 + intra-pipeline parallel dispatch, `17e932c` + `0a17665`) | Independent DAG branches execute concurrently on the runner's thread pool. |
| T7 Placement transfers — CPU-only | ✅ done | heap↔disk via store mmap. |
| T8 Placement transfers — CUDA | ⚠️ canary only (A11, `bd00bc5`) | gpu-smoke runs end-to-end; real GPU steps come later. No `cuda_copy_*` primitives yet (PLAN-CUDA C5). |
| T9 Session manager | ✅ done (A7 `0b18787`; refactored to opaque types 2026-05-04 `d91f0ab`) | session_t with token + out-queue + run thread + pin list + grace state. Janitor thread sweeps expired sessions (default grace 600s). RECONNECT reattaches by token; disconnected sessions keep pins so reconnect+late-SUBSCRIBE serves from cache. **2026-05-04 refactor**: factored into opaque types — `session_t` lives in `core/src/server/session.c` (`v3/session.h`); `session_mgr_t` (sessions[] table + janitor + reattach) lives in `core/src/server/session_mgr.c` (`v3/session_mgr.h`).  `batch_ctx_t` got the same treatment (`core/src/batch/batch_ctx.c`). All field access goes through accessors that take the internal mutex; outside callers never see the lock — drift is now a compile error.  Driven by the journal-acc race fix (BUG-cache-hit-runner-cleanup.md) closing the structural drift surface that caused that bug. |
| T10 Late subscribe | ✅ done (A7, `0b18787`) | SUBSCRIBE message reuses last_pipeline + last_input stored on the session; runner's cache-skip handles the "only the tail recomputes" invariant. SUBSCRIBE_ACK terminates the flow (no RUN_STARTED/RUN_COMPLETE echo). |
| T11 Cancellation | ✅ done (A7, `0b18787`) | `runner_cancel(runner, run_id)` flips rs->cancel under rs->lock + broadcasts; step_task_fn short-circuits; runner_run emits on_error(NULL, "cancelled") and returns -1. Wired to WS CANCEL message and to disconnect-mid-run. |
| T12 Error handling end-to-end | ⚠️ partial | step_failed/run_failed events + on_error callback; cancellation wired (T11) so the original "no cancellation" gap is closed. Remaining gap: structured `result_t {ok, code, detail}` propagation surface — current callbacks pass plain `const char* msg`. |
| T13 Array artifacts | ✅ done (B3 prerequisite, `f7afe5d`) | Shipped with T13 work pre-B3. Two new kinds + four context helpers. No scheduling changes. |
| T14 Streaming writes | ✅ done (B9, `bd10425`) | Context helper `step_write_output_stream(ctx, name, bytes, len, final)` + in-process accumulator in `step_context`. One `store_put` call at `final=true` (no separate store append helper — PLAN-STORE S11 deferred). `on_stream_chunk` callback fires before commit so the server can forward chunks live. First consumer: `laz-extract-disc` (20k-point chunks). WS plumbing + dual-delivery documented in PLAN-VIEWER-PORT §7.3. |
| T15 `cached_compute` + intent map | ✅ done (B3.5, `f359b50`) | Unified cache primitive from `HASH-MODEL.md`; `cached_compute.h` + `intent_map.h` live in `core/include/v3/`. `step_output_hash_t` retired; `skip_on_identity` is the only cache-related knob. |
| T16 Step-output manifests + validate-on-use | ✅ done (B3.5, `f359b50`) | Runner wraps exec in `cached_compute`; manifests populate artifact_map; stale-manifest detection triggers step re-run. Subsequent milestones B4-B10 all run on this. |

## Implementation tasks (ordered)

### T1. Bare thread pool
- `pool_create`, `pool_submit`, `pool_shutdown`.
- Bounded queue with condvar.
- Unit tests: concurrent submits, shutdown drains, panic safety.
- TSan clean under stress (10k submits × 100 threads).

### T2. Atomic refcount + state machines
- `atomic_refcount_t` with `acquire/release`.
- `state_machine_t` helper with transition table + assertion.
- Unit tests: concurrent acquire/release race-freedom.

### T3. Step registry
- Linker-section registration or explicit init list.
- `step_register`, `step_lookup`.
- Two dummy steps registered for testing.

### T4. Pipeline loader
- JSON parser (yyjson) reads pipeline definition.
- Validates: all step names resolve, DAG is acyclic, all artifacts have exactly one producer.
- Builds an in-memory representation.

### T5. DAG walker
- BFS backward from subscribed artifacts.
- Topological sort of touched steps.
- Cache-hit filtering (calls into store).
- Unit tests on synthetic DAGs.

### T6. Basic runner
- Accepts `run_spec_t`, creates `run_handle_t`.
- Walks DAG, dispatches steps to thread pool.
- Emits callback events.
- CPU-only; no placement transfers yet.
- Integration test with two dummy steps.

### T7. Placement transfers — CPU-only
- Heap → heap (no-op for identity).
- Disk → heap via store mmap.
- Heap → disk via store write.
- Unit tests against synthetic artifacts.

### T8. Placement transfers — add CUDA paths (later, after CUDA subsystem exists)
- Heap ↔ device via cudaMemcpyAsync on the compute thread's stream.
- Enforced "no direct disk ↔ device" via-heap routing.
- Integration test with gpu-smoke step.

### T9. Session manager
- `session_create`, `_lookup`, `_add_ref`, etc.
- Grace period tracking with a timer thread.
- Unit tests for ref lifecycle, grace expiry.

### T10. Late subscribe
- `runner_late_subscribe` schedules a DAG re-walk rooted at the newly-subscribed artifact.
- Reuses in-store ancestors (content-hash hits).
- Integration test: full run, disconnect, reconnect, subscribe an unsubscribed artifact, verify single-step recompute.

### T11. Cancellation
- `runner_cancel` flags the run; each step checks periodically.
- Partially-written artifacts are not committed.
- Integration test: mid-run cancel, verify no partial output.
- Lock-ordering invariant: `runner::active_lock → rs->lock` (strict LIFO). See [CONCURRENCY.md §12.5](../CONCURRENCY.md) — "Cancel invariant" — for the canonical statement of how runner_cancel interacts with worker callbacks and the post-cancel cleanup.

### T12. Error handling end-to-end (ongoing)
- All fallible ops return `result_t`.
- Errors propagate through callbacks to transport.
- Test: forced step failure, verify ERROR message delivered.

### T13. Array artifacts ✅ done (B3 prerequisite)
- Driven by PLAN-VIEWER-PORT §6. Lands with milestone B3.
- Two new `artifact_kind_t` enum entries: `ARRAY_INLINE`, `ARRAY_BY_HASH` (kinds 8 and 9 per arch doc §5.3).
- Four new `step_context_t` helpers: `step_array_len`, `step_array_read_bytes` (inline), `step_array_open` (by-hash, returns `store_handle_t`), `step_write_array_inline`, `step_write_array_by_hash`.
- Declaration-flag validation: step registration gains `array: bool` on reads and writes; registry rejects mismatches between declaration and actual step behaviour.
- No scheduling changes. No DAG-walker changes. No identity-hash changes. A step is still a single task.
- Unit tests: (a) round-trip inline array write/read; (b) round-trip by-hash array write/read with element reuse across two arrays; (c) cache-hit on identical manifests; (d) manifest-hash-changes-on-reorder to enforce the ordering contract.

### T14. Streaming writes ✅ done (B9)
- Driven by PLAN-VIEWER-PORT §7. Landed in milestone B9.
- Context helper: `step_write_output_stream(ctx, name, bytes, len, bool final)`. Step may call multiple times before returning; must call with `final=true` exactly once per streamed write. Mixing streaming and non-streaming helpers for the same artifact is rejected.
- Write declaration: new `step_write_t.streaming: bool` (per-write, not per-step). The pre-existing `step_def_t.streaming` is unrelated and untouched.
- **Accumulator: in-process**, inside `step_context` — one growing heap buffer per streaming write (64 KiB initial, geometric realloc). No separate store helper. On `final=true` the runner calls the existing `store_put` once with the concatenated buffer (ownership-transfer, same path as `step_write_output_owned`). Rationale: our current streams (≤2.4 MB for 100k points × 24 B) fit trivially in RAM, and avoiding fsync-per-chunk keeps the store API surface stable. PLAN-STORE S11 (temp-file variant) is deferred until a stream outgrows RAM.
- Runner callback: new `on_stream_chunk(run_id, artifact, seq, final, bytes, len, hash?)` fires BEFORE commit so the server can forward chunks live. `hash` is non-NULL only on the final chunk. Batch mode leaves the cb pointer `NULL` and the runner skips the chunk-emission path entirely — batch still streams internally, just delivers the final artifact via `on_artifact_ready` like any other write.
- WS protocol: `STREAM_CHUNK` text message + binary frame; see arch doc §8.1 for the schema. Final chunk carries the finalised artifact hash. After the final chunk, the server *also* emits a standard `ARTIFACT_HEADER` + binary for the bytes (dual delivery — see PLAN-VIEWER-PORT §7.3).
- Under the unified hash model (HASH-MODEL.md / T15), streaming writes are **not eligible for intent-map skip** at the individual-chunk level — the content hash is known only at `final=true`. The whole streaming sequence is one `cached_compute` invocation that produces the accumulated bytes as its result. Mid-stream cancel/error drops the accumulator in the body-trampoline cleanup loop; no `store_put`, no intent-map entry. A second identical run is a full cache hit: the step's `exec` is not called, no `on_stream_chunk` fires, the stored artifact is re-delivered via `ARTIFACT_HEADER`.
- Tests: `runner_test` (+2) verifies chunk concatenation + full-cache-hit on re-run (zero chunk callbacks); `serve_stream.py` drives 5-chunk `stream-demo` over WS and asserts chunk ordering, deterministic byte pattern, `STREAM_CHUNK final.hash == ARTIFACT_HEADER.hash` (protocol-level invariant proving the stream finalised to the same bytes the store committed — xxh3-128 zero-padded; no Python stdlib recomputation), and first-chunk-within-500ms (measured ~0.2 ms on the bench).

### T15. `cached_compute` primitive + intent map ✅ done (B3.5)
*Driven by [`HASH-MODEL.md`](../HASH-MODEL.md). Lands before B4 as a unified refactor.*

- New files:
  - `core/include/v3/intent_map.h` — public API (get / put / invalidate / lifecycle).
  - `core/src/intent_map/intent_map.c` — append-only log + in-memory hashmap. See PLAN-STORE S14.
  - `core/include/v3/cached_compute.h` — `cached_compute(identity, fn, ctx, out)`.
  - `core/src/runner/cached_compute.c` — primitive implementation. ~50 LOC.
- Hot-tier + intent-map additions in the store — see PLAN-STORE S12–S15.
- Step-write API changes:
  - Remove `step_write_t.output_hash` field (and the `step_output_hash_t` enum).
  - Add `step_write_t.skip_on_identity: bool` (default `true`). Non-deterministic escape hatch only.
  - `store_put_with_hash` → `store_put`: single signature that takes ownership of the heap buffer. Callers that need to preserve bytes do `malloc + memcpy` at the call site.
- Unit tests:
  - `intent_map_test`: round-trip, tombstone invalidation, concurrent put/get, partial-tail recovery, compaction.
  - `cached_compute_test`: hit, miss, compute_fn error propagation, idempotent re-put, concurrent calls with same identity.
  - `store_test` additions: hot-tier LRU, demotion under pressure, zero-copy handle from hot tier.

### T16. Step-output manifests + validate-on-use ✅ done (B3.5)
*Lands with T15. See [`HASH-MODEL.md`](../HASH-MODEL.md) §11.2.*

- `execute_step` in `runner.c` is rewritten:
  - Compute `step_identity = hash(name, version, params, sorted_read_hashes)`.
  - Call `cached_compute(step_identity, body_trampoline, step, &manifest_hash)`.
  - On return, mmap the manifest (from the store), parse `{write_name → content_hash}` entries, populate the run-local `artifact_map`.
  - **Validate on use**: for each entry in the step-output manifest, `store_contains(content_hash)`. If any miss, invalidate the intent-map entry and retry the step (bounded to one retry). Also validate array-typed reads before dispatching the next step.
- `body_trampoline(step)` runs the step's `exec_fn`, collects each write's bytes, hashes each, calls `store_put(content_hash, bytes, len)` (ownership transferred), builds a manifest listing `{write_name: content_hash}`, returns the manifest bytes.
- Removes all derivation-hash-specific code: `compute_derivation_write_hash`, the `all_derivation` check, pre-exec cache-skip via `store_contains(derivation_key)`. All replaced by a single `cached_compute` call.
- Unit tests:
  - Pipeline rerun: all steps cache-hit via intent map; zero `body_fn` executions.
  - Evicted leaf in manifest: pipeline rerun re-executes the producing step and succeeds transparently.
  - Non-deterministic step with `skip_on_identity: false`: always executes, cache never populated.

## Dependencies

- **Before**: logging subsystem (T1 logs pool events), store subsystem (T5 depends on `store_lookup`).
- **After**: protocol subsystem uses runner callbacks; CUDA subsystem adds GPU step dispatch.

## Definition of done

- T1–T12 complete, unit + integration tests green (T1–T12 close out Phase A runner work).
- T13 shipped pre-B3.
- T14 landed with B9 (streaming retrofit for `laz-extract-disc`).
- T15–T16 landed with B3.5 (hash-model refactor) before B4 (commit `f359b50`). Runner's `execute_step` was rewritten around `cached_compute`; `step_output_hash_t` is gone; `skip_on_identity` is the only cache-related knob.
- TSan clean under stress on T1 (thread pool), T10 (late subscribe), T15 (concurrent `cached_compute` calls with same identity).
- Runner correctly dispatches a three-step dummy pipeline (smoke pipeline precursor).
- Session grace-period semantics verified by integration test.
- Post-T16 integration test: `batch_array_demo` second run produces zero body_fn executions (all intent-map hits).
- **Integration matrix coverage** (per [docs/quality/README.md](docs/quality/README.md), 2026-05-04): runner concurrency exercised by `serve_concurrent_clients` (sessions_lock stress), `serve_all_cache_hit_burst` (worker fan-out on cache-hit path — the journal-acc race repro), `serve_late_subscribe` + `serve_rehydrate_run` + `serve_invalidate_then_rerun` (re-entry into `execute_step` / `cached_compute` from the second run), `serve_cancel_during_run` (T11 cancel races worker callbacks), `serve_disconnect_during_run` (T11 cancel via session teardown), `serve_reconnect_within_grace` (T9 reattach + grace + pin lifetime — exercises the documented `sessions_lock → session_t::lock` nesting from CONCURRENCY.md §12.3).  All 10 scenarios pass under TSan on prod 2026-05-04.

## Open questions

- **Priority scheduling.** Runner currently FIFO. Should viewer requests preempt lab-UI requests? Probably yes eventually, but out of scope for Phase A.
- **Per-run resource limits.** Should a runaway step be killable? Watchdog? Phase A: no; step authors are trusted. Phase B or later: add soft timeouts with cancellation.
- **Distributed session lookup.** Interface is already shaped for it (§19). Phase A: in-process only.

## Risk / unknowns

- C11 threading is portable but bare-bones. Non-reentrant condvars on some platforms; test on target (Linux) early.
- Linker-section step registration works on GCC and Clang but is non-standard. Fallback: explicit init list in a generated header. Decide during T3.
