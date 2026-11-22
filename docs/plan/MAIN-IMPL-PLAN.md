# Main Implementation Plan — v3 Foundations

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). Focused on Phase A (foundations). Subsystem-level detail lives in sibling `PLAN-*.md` files in this directory.*

---

## Goal of Phase A

Ship `dioramap-processor v0` — a single C binary with the complete runtime machinery, validated end-to-end by a smoke pipeline that exercises every concept. **No real algorithms.** Three dummy steps whose only purpose is to prove the plumbing works.

At Phase A completion, the following must be true:

- A fresh `batch`-mode run of the smoke pipeline produces expected artifacts, matches byte-for-byte a committed golden.
- A fresh `serve`-mode WS session executes the same pipeline, streams artifact frames to the client, closes cleanly.
- A second run (either mode) is a cache hit — zero compute, measured via log events.
- Disconnect + reconnect within the grace period rehydrates the session and re-delivers cached artifacts instantly.
- Late SUBSCRIBE to an unsubscribed artifact triggers a single-step recompute (all upstream cached) and delivers.
- `gpu-smoke` runs in the CUDA build; output bit-identical to CPU reference.
- CI passes: unit tests, golden tests, smoke integration tests, TSan/ASan runs.

Only when all seven are true does Phase A close.

---

## Subsystem inventory

The binary is organized into eight subsystems. Each has its own plan doc:

| Subsystem | Source dir | Plan doc | Responsibility |
|---|---|---|---|
| Runtime primitives | `core/src/runtime/`, `runner/`, `step/`, `pipeline/` | `PLAN-RUNNER.md` | Threads, queues, refcounts, runner, DAG walker, session, placement management |
| Store + cache | `core/src/store/`, `intent_map/` (post-B3.5), `artifact/`, `hash/`, `mmap/` | `PLAN-STORE.md` + [`HASH-MODEL.md`](../HASH-MODEL.md) | Content-addressed artifact store with hot (RAM) + warm (disk) tiers, LRU eviction, session refs, zero-copy reads (direct pointer hot / mmap warm). Persistent intent map (`step_identity → content_hash`) handles cache-skip across restarts. |
| Protocol | `core/src/server/` | `PLAN-PROTOCOL.md` | Binary artifact layout codec, WS message framing, HTTP endpoints, batch-mode CLI |
| Fetchers | `core/src/fetch/` (TBD) | `PLAN-FETCHERS.md` | libcurl wrap, TLS, outbound HTTPS, retry/backoff, control-plane client |
| CUDA runtime | `core/src/cuda/` (TBD) | `PLAN-CUDA.md` | Context, streams, pinned allocator, device buffer pool, gpu-smoke canary |
| Logging | `core/src/log/` | `PLAN-LOGGING.md` | Structured event emission, subsystem routing, backpressure |
| Testing | `core/tests/` | `PLAN-TESTING.md` | Unit framework, golden harness, smoke integration |
| Build + CI | `core/CMakeLists.txt`, `.github/` | (in this doc) | CMake, Ninja, clang-format, CI matrix |
| Viewer port | `steps/**` (incoming) | `PLAN-VIEWER-PORT.md` | JS→C port of the `lidar-3d-environment-viewer/backend` algorithms: step inventory, pipeline DAG, external data sources, port-order milestones B1–B8 |

Some subsystems physically share a C source directory; the plan split follows conceptual boundaries, not directory boundaries.

---

## Dependency graph between subsystems

```
         logging
            │ (used by all)
            ▼
    ┌────────────────────────┐
    │  runtime primitives    │
    │  (threads, queues)     │
    └──┬──────────────┬──────┘
       │              │
       ▼              ▼
    ┌─────────┐    ┌───────────┐
    │  store  │    │  protocol │
    │         │◄───┤           │
    └────┬────┘    └─────┬─────┘
         │               │
         │         ┌─────┴──────┐
         │         │  fetchers  │
         │         └────────────┘
         ▼
    ┌─────────┐
    │  runner │ (uses store, protocol via callbacks)
    └────┬────┘
         │
         ▼
    ┌─────────┐
    │  cuda   │ (optional; runner dispatches GPU steps)
    └─────────┘
```

Reading the graph: logging is leaf-level, used everywhere. Runtime primitives come next (locks, thread pools). Store and protocol use the primitives. Runner depends on store and protocol. CUDA is a late extension used only by the runner when compiled in.

---

## Phase A status snapshot (2026-04-20)

| # | Task | State | Notes |
|---|---|---|---|
| A1 | Scaffolding | ✅ done | CMake/Ninja/clang-format/CI (CI is disabled, see note below) |
| A2 | Logging | ✅ done | Core + ring buffer + drain thread (L2/L3) + runner/store events |
| A3 | Runtime primitives | ✅ done | Thread pool + refcount. Pool not yet consumed by runner. |
| A4 | Artifact binary layout | ✅ done | Encoder + decoder + hash + mmap |
| A5 | Store + cache | ✅ done | Index, disk, LRU, janitor, pins, scan skips malformed files |
| A6 | Runner | ✅ done (single-threaded) | DAG walk, topo sort, cache skip, callbacks, log events, content-hashed outputs tested |
| A7 | WS + HTTP transport | ✅ done (A7) | Full protocol: SESSION_STARTED, CATALOG (bare + per-pipeline), RUN, SUBSCRIBE (late, cached), CANCEL, RECONNECT with grace + store pins, disconnect cancels active run. `serve_smoke` + `serve_full` ctests green; full suite ASan-clean. HTTP endpoints + static file serving deferred to Phase B (not blocking viewer flow). TLS: terminated upstream at nginx (D14). |
| A8 | Batch mode | ✅ done | `batch` subcommand + demo pipeline + manifest.json |
| A9 | Fetchers | ✅ done (B1) | libcurl + mbedTLS + retry/backoff + per-host token bucket + IGN wrappers (`fetch-ortho-rgb`, `fetch-ortho-irc`, `fetch-elevation`, `fetch-cadastre-parcels`, `fetch-cadastre-buildings`). Surround variants added in B10. |
| A10 | Control-plane client (C side) | ✅ done | Shipped 2026-04-28 across 3 commits (+1 CP-side). **A10.1** (`d0efd5e`): new `core/src/fetch/cp_client.{c,h}` — libcurl wrapper, in-memory verify-key cache (60s TTL, 300s grace), ring-buffered record-usage with dedicated flush thread (5s/100-event), disabled mode when `cp_url` empty. `cp_client_test.c` exercises 14 assertions against `cp_stub_server.py` (Python http.server) — passes in <150ms. **A10.2** (`3200810` + CP `f29bcac`): `LWS_CALLBACK_ESTABLISHED` reads `?key=` and Origin, calls `cp_verify_key`, sends `SESSION_STARTED{tier}` on success or `ERROR{code:AUTH_FAILED, msg:<reason>}` on failure. `server_config_t.cp_client` plumbing; `session_t.api_key/org_id/tier` capture. `main.c` adds `--control-plane-url` + `--control-plane-secret` flags + `DIORAMAP_CP_URL` / `DIORAMAP_CP_SECRET` env. CP-side: `seed-dev-key.js` accepts comma-list `SEED_ORIGIN`; `dev-up.sh` defaults the list to dashboard + lab-ui + sdk + future seller-demo and passes the CP URL + secret to the processor. **A10.3** (`fefe8be`): `cb_on_run_complete` posts a `scene_load` event with `l2_hit` (fully-cached marker), `compute_ms`, and best-effort `lat/lon`. Validated end-to-end: fresh run → `l2_hit=false`, cached re-run → `l2_hit=true`, `/api/usage` totals reflect both. Unblocks tier enforcement and is a prerequisite for A14. |
| A11 | CUDA runtime + gpu-smoke | ⚠️ canary shipped | Step + pipeline + CTest. CUDA toolkit not locally available; sm_70 wiring only under `-DDIORAMAP_CUDA=ON`. |
| A12 | Smoke pipeline | ⚠️ partial | `demo-chain` + `gpu-smoke` pipelines exercised by batch mode + serve mode (incl. reconnect + late subscribe) CTests. Missing DoD items: CUDA bit-identical parity (needs real GPU runner) and the "cache-hit on second run via log events" assertion (needs a log-introspection test). |
| A14 | Control-plane run telemetry (extends usage_events) | ✅ done (2026-04-28, processor `06b35c2` + CP `a8cbfe3`) | Depends on **A10 + L6**. Single-table design: extend `usage_events` with the columns needed for support reproduction. **Why one table, not two**: scene_load events are 1:1 with `cb_on_run_complete` (same row of data, different metadata projection); separate-table justifications evaporated when we kept the same retention. Migration `008-extend-usage-events.sql` adds `(run_id, processor_node_id, pipeline_name, input_hash, test_case_label, duration_us, success, error_kind, error_message, n_executed, n_cache_hits, bytes_out)` as nullable columns + 2 indexes (`(processor_node_id, run_id)` and `(pipeline_name, input_hash)`). Existing aggregate queries (`/api/usage`, dashboard counters) untouched. **Late-subscribe fix**: today `cb_on_run_complete` skips the journal/usage record when `s->late_subscribe_active`. That's wrong — a late-subscribe spawns a real runner.run with its own run_id and may execute fresh steps; both billing and support care about it. Drop the skip in: cb_on_run_started's started_at re-stamp, the L6 journal record, and the cp_record_usage call. Use `event_type='late_subscribe'` to distinguish on the dashboard. **Processor side**: extend `cp_usage_event_t` with the new fields; populate them in `cb_on_run_complete` from existing session state (run_id, journal_pipeline_name, journal_input_hash, etc.). New `processor_node_id` UUID written once to `<cache_root>/node_id`, reused across restarts. **CP side**: extend `POST /internal/record-usage` to read the new fields (additive, backwards-compat with old payloads). New `GET /api/admin/runs` admin-only endpoint with filters `(org_id, pipeline, run_id, since, until, success)` for support flow. **Privacy invariant preserved**: only `input_hash` reaches the cloud, never `input_json`. ~80 LOC CP + ~80 LOC processor. View over usage_events for support-shaped queries can be added later if the column count makes ad-hoc queries awkward — not blocking. |
| A15 | Control-plane v3 migration | ✅ done | Shipped 2026-04-28 across 6 control-plane commits + 1 v3 commit. **A15.1+A15.2** (`d150daa`): replaced legacy `dioramap-sdk.js` / `dioramap-picker.js` with v3 UMD bundles (`dioramap-viewer.js`, `dioramap-location-picker.iife.js` + CSS); rewrote `viewer.ejs` to load via `<script src>` + global `window.DioramapViewer.init` instead of ES dynamic import; reconciled `backendWs` env-var → `backendUrl` (SDK frozen surface); added `?v=<sdkVersion>` cache-bust (env `SDK_VERSION` or `Date.now()` in dev). **A15.3** (`aa3209d`): rewrote `scripts/dev-up.sh` — dropped Server B + seller-demo orchestration; new `start_processor` runs `cmake --build` if needed then launches `dioramap-processor serve --port 7801 --pipelines-dir ... --cache-dir ...`; TCP-open health probe; `LIDAR_ALGOS_DIR` replaces `VIEWER_DIR`. **A15.4+A15.5** (`1aef005`): generalised `/api/usage` to surface `other_events` count + per-type breakdown so non-`scene_load` events aren't silently undercounted; new admin-only `GET /api/admin/processor-health` does a 2-second TCP-open probe against `VIEWER_WS_URL`. **A15.6** (`672d5b8`): two-stage `Dockerfile` for `dioramap-processor` (Ubuntu 24.04 builder → slim runtime, non-root user, healthcheck via `nc -z`). **A15.7** (`2a31eda`): `control-plane/docs/SDK-HOSTING.md` documents the in-tree → npm → CDN progression. P2 items (`processor_nodes` table, cross-origin CORS) deferred until customer count justifies them. Pre-existing 2/75 dashboard tests stayed pre-existing — no regressions introduced. |
| A16 | Seller-demo + end-to-end auth flow | ✅ done (2026-04-28) | New v3 app at `apps/seller-demo/` (Vite, port 5184) — single-page seller-style UI: header, product sidebar (3 GLBs from `/api/products`), map picker for location, embedded viewer, ghost-place models from the catalog. Real API key flow: hardcoded test key passed to `DioramapViewer.init({apiKey})` → SDK appends to WS URL → processor calls `/internal/verify-key` → tier returned end-to-end. Validates the production embed path: bundle hosting (A15) + auth (A10) + scene rendering (S1–V8) all proven on a seller-shaped surface. **Does NOT include scene persistence** — the SCENES-DESIGN.md feature stays paper-only until a customer asks (separate future milestone, not in this workstream). ~500 LOC. |

**CI**: `.github/workflows/v3-ci.yml.disabled` — renamed to not consume Actions minutes. Re-enable by restoring the `.yml` extension.

**Deferred decisions that have resolved**: Q1 (xxh3), Q2 (prefix-sharded paths), Q5 (streaming — now committed as PLAN-RUNNER T14 + PLAN-PROTOCOL P10 with the B9 milestone), Q7 (bench.db not migrated), Q9 (viz placement), Q10 (no real-buildings importer), Q11 (jansson).

## Sequence of implementation (within Phase A)

Not strictly serial — some subsystems can parallelize with independent PRs. But the critical path is:

1. **A1 Scaffolding** — CMake, Ninja, empty `main.c`, CI pipeline shell, clang-format.
2. **A2 Logging** — before anything else that emits events. Subsystem interface + stdout JSON-lines sink. Every subsequent subsystem is coded against this API from day one, never retrofitted.
3. **A3 Runtime primitives** — thread pool, work queue, atomic refcount, state machines. TSan-clean. Property tests.
4. **A4 Binary artifact layout** — encoder, decoder, header validation, round-trip tests. Depends on A2/A3.
5. **A5 Store + cache** — depends on A3 (threading), A4 (layout), A2 (logging). Phase A shipped warm-tier (disk) only with a single `store_put_with_hash` path. The hash-model refactor (B3.5) adds the hot tier, the intent map, and the unified `cached_compute` primitive. See [`HASH-MODEL.md`](../HASH-MODEL.md) and PLAN-STORE S12–S15.
6. **A6 Runner** — DAG walker (with pipeline-input root artifact injection), step registry, step dispatch, placement transfers (CPU-only first). Requires A3, A5. Protocol-independent — runner is driven by a callback interface that either WS-sends or batch-writes.
7. **A7 Protocol** — binary layout codec done in A4; now add WS framing (libwebsockets) and HTTP static serving. Requires A3 for connection handling.
8. **A8 Batch mode** — connects runner to filesystem sink. Thin wrapper over already-working runner.
9. **A9 Fetchers** — libcurl + mbedTLS. Initially: a dummy fetcher step that reads a file; real HTTPS later in Phase B1.
10. **A10 Control-plane client skeleton** — simple cached verify, batched usage emit. Points at a local stub.
11. **A11 CUDA runtime + gpu-smoke** — `-DDIORAMAP_CUDA=ON`, context init, streams, pinned allocator, gpu-smoke step. This is the first step in the smoke pipeline that has a GPU impl.
12. **A12 Smoke pipeline** — the three-step dummy pipeline + its goldens + integration tests. Only green when A1-A11 are all present.

A1→A4 is strict prerequisite for everything else. A6 (runner) and A7 (protocol) can proceed in parallel. A11 (CUDA) can slot in anywhere after A3.

The control-plane / infrastructure lane (A10, A14, A15, A16) is a self-contained workstream gated on dioramap.com integration rather than the smoke-pipeline critical path. See "Control-plane workstream" appendix below.

---

## Cross-cutting concerns

These don't belong to any single subsystem but must be decided early.

### Concurrency discipline

- **Ownership over sharing.** Most data structures are owned by one thread. When sharing is necessary, use atomic refcount + mutex; document the ownership chain.
- **No blocking operations on the main event-loop thread.** All HTTP/FS/compute offloaded to worker threads. Main thread is for I/O dispatch only.
- **Refcounting is pervasive.** Artifacts, sessions, runs all have explicit refcounts. `acquire/release` paired. Destructor is a function of count reaching zero.
- **State machines are explicit.** Session lifecycle, run lifecycle, artifact lifecycle — each has a documented set of states and valid transitions. Invalid transitions assert in debug, log-error in release.
- **Encapsulate shared mutable state.** Define structs whose fields are touched by multiple threads in a single `.c` file; expose an opaque-pointer accessor API that takes the internal mutex.  Direct field access from outside the owning file is impossible — the compiler enforces what was previously convention-only.  Anti-exemplar: pre-2026-05-04 `session_t` was a struct-bag where every callsite had to remember the locking rule; drift caused the BUG-cache-hit-runner-cleanup.md race.  Exemplars: `store_t`, `runner_t`, `intent_map_t`, post-refactor `session_t` / `session_mgr_t` / `batch_ctx_t`.  See CONCURRENCY.md invariant 10 + anti-pattern 14.
- **TSan + ASan builds in CI.** Every PR runs both. Neither can be dirty.

### Error handling

- C has no exceptions. All fallible functions return `result_t { ok: bool, code: u32, detail: const char* }` or an out-parameter for the payload.
- Errors never panic. Panic is for invariant violations (null deref, use-after-free in debug).
- Logged errors carry enough structured context to reconstruct the situation: which run, session, step, artifact, upstream source.

### Memory allocation

- One global allocator (system default) with optional jemalloc swap via link flag.
- Per-subsystem arena allocators for short-lived objects (per-run scratch, per-step transient data) — reset at boundary rather than individual frees.
- No calls to `malloc` in the hot path of a running step. Step work buffers are acquired from the arena or the device pool, never fresh-allocated per point.

### Configuration

- Single TOML config file, parsed at startup. No runtime reload for most keys.
- Environment variables override config (twelve-factor style).
- CLI flags override env.
- Config schema documented and validated on parse; unknown keys warn (to avoid typos being silent).

### Feature flags

None. As stated in of the architecture doc: to change behavior, bump a step version or ship a new pipeline. Runtime flags introduce too many failure modes.

---

## Build and CI

### Build system

- **CMake 3.24+** with Ninja generator. Build in seconds, parallel by default.
- **C standard: C11.** Portable, widely supported. No GCC-specific extensions outside isolated files under `#ifdef`.
- **Warnings as errors.** `-Wall -Wextra -Werror -Wpedantic -Wshadow`. No exceptions.
- **Sanitizer builds in CI**: `-fsanitize=address,undefined` and a separate `-fsanitize=thread` job.
- **Static linking preferred** for production binary: easier to ship, simpler deployment. Dynamic for development builds only.

### CMake options

```
-DDIORAMAP_CUDA=ON|OFF        # build GPU backend (adds NVCC pass)
-DDIORAMAP_ASAN=ON|OFF        # AddressSanitizer + UBSan
-DDIORAMAP_TSAN=ON|OFF        # ThreadSanitizer + UBSan (excl. ASAN)
-DDIORAMAP_COVERAGE=ON|OFF    # llvm-cov instrumentation (clang only)
-DDIORAMAP_FUZZ=ON|OFF        # libFuzzer targets (clang only, +ASan)
-DDIORAMAP_USE_JEMALLOC=ON|OFF
-DDIORAMAP_BUILD_TESTS=ON|OFF
-DDIORAMAP_OBFUSCATE=ON|OFF   # tokenize identifiers via t() rewriter
-DDIORAMAP_RESEARCH=ON|OFF    # build research-track libs / candidate
                              # pipelines; default OFF (third-party
                              # dependencies stay out of the prod
                              # binary). See PLAN-THIRD-PARTY-RESEARCH.md.
# Release hardening (strip + -fvisibility=hidden + no build-id) is driven by
# CMAKE_BUILD_TYPE=Release, not a dedicated flag.
# clang -Wthread-safety -Werror=thread-safety auto-applies on any clang
# build (validates the annotations in core/include/v3/thread_safety.h).
# See docs/quality/README.md for which variants pair with which
# instrumentation flags, and docs/quality/README.md for example
# build commands per variant.
```

### CI matrix

| Job | OS | Compiler | CUDA | Sanitizer | Tests |
|---|---|---|---|---|---|
| linux-gcc-cpu | Ubuntu 24 | gcc-13 | no | none | full |
| linux-clang-cpu | Ubuntu 24 | clang-17 | no | none | full |
| linux-clang-asan | Ubuntu 24 | clang-17 | no | ASAN+UBSAN | unit + golden + smoke |
| linux-clang-tsan | Ubuntu 24 | clang-17 | no | TSAN | unit + smoke |
| linux-gcc-cuda | GPU runner | gcc-13 | CUDA 12.x | none | full incl. gpu-smoke |

Every PR runs at minimum the `linux-gcc-cpu`, `linux-clang-asan`, and `linux-clang-tsan` jobs. The CUDA job runs on a dedicated runner; PRs touching `core/src/cuda/` or `steps/**/*.cu` gate on it.

### Dependency management

- **Vendored, minimal, explicit.** No Conan / vcpkg / Hunter. Dependencies live in `core/third_party/` as git submodules or source tarballs. One `CMakeLists.txt` per dep declaring build flags.
- **Pinned versions**, not floating. Bumping a dep is a deliberate PR with changelog.

Initial deps:

| Lib | Version | Purpose | Status |
|---|---|---|---|
| libwebsockets | 4.3+ | Inbound WS (libwebsockets built **without** TLS; nginx terminates per §D14) | ✅ linked (A7) |
| jansson | 2.14 | JSON parsing (step params, WS messages, GeoJSON payloads from WFS). Replaces the original yyjson pick for one-fewer-dep. | ✅ linked |
| libxxhash (xxh3 128-bit) | latest | Content hashing — final choice. Hardware-accelerated, ~20-30 GB/s. Cryptographic strength not needed (v3 doesn't ingest adversarial content). `hash_t` is sized at 32 bytes so a future blake3-256 swap remains a header-only change if cryptographic properties are ever required. | ✅ linked |
| libcurl | 8.x | Outbound HTTPS (A9) | ✅ linked (B1) |
| mbedTLS | 3.x | TLS for libcurl (outbound only; not used for inbound since nginx terminates) | ✅ linked (B1) |
| libjpeg-turbo | 3.x | JPEG encode/decode for ortho photos (Phase B) | ✅ linked (B5b) |
| PROJ | 9.x | WGS84 ↔ Lambert-93 + future projections (Phase B) | ✅ linked (B2) |
| laz-perf | latest ([hobu/laz-perf](https://github.com/hobu/laz-perf)) | LAZ decompression (Phase B); vendored via CMake submodule | ✅ linked (B3) |
| copc-lib | latest ([rockrobotic/copc-lib](https://github.com/rockrobotic/copc-lib)) | COPC hierarchy parsing (Phase B); vendored | ✅ linked (B3) |
| ~~blake3~~ | — | Originally flagged as the hash choice, then as a future upgrade. Retired 2026-04-21: xxh3-128 is strictly sufficient for our threat model (non-adversarial content-addressing). `hash_t` size is preserved so a blake3 swap remains possible in the future if requirements change (e.g., cryptographic integrity across untrusted transfers), but this is no longer a tracked work item. | ❌ not planned |

#### Research-track deps (NOT in production)

The following are linked only when CMake is configured with
`-DDIORAMAP_RESEARCH=ON`. The flag defaults to OFF; dev sets it via
`PROCESSOR_REQUIRED_FLAGS` in `dev-up.sh`, prod (`v3/Dockerfile`)
leaves it OFF. They exist to support the third-party-evaluation
tracks documented in [PLAN-THIRD-PARTY-RESEARCH.md](./PLAN-THIRD-PARTY-RESEARCH.md).

| Lib | Source | Used by | Status |
|---|---|---|---|
| CGAL 5.6+ | `apt install libcgal-dev libgmp-dev libmpfr-dev` (header-only kernel + GMP/MPFR transitive). Wired into `.devcontainer/Dockerfile`. | `dioramap_steps_polyfit` (Region growing / RANSAC); future KSR / Roofer ports | ✅ linked when `DIORAMAP_RESEARCH=ON` |
| PolyFit | gitignored clone at `v3/third_party/PolyFit/` (commit pin + clone command in `v3/third_party/.gitignore`). Bundles its own SCIP + SoPlex. | `dioramap_steps_polyfit` only | ✅ linked when `DIORAMAP_RESEARCH=ON` |

These libs **never** propagate to production. With
`DIORAMAP_RESEARCH=OFF` the PolyFit `add_subdirectory` chain isn't
configured, the polyfit step lib isn't added, the processor binary
contains zero PolyFit / SCIP / SoPlex symbols, and the build needs
neither `libcgal-dev` apt-installed nor a PolyFit clone present.

---

## Phase A definition-of-done (expanded)

Repeating the seven assertions from the architecture doc A12 with measurable criteria:

1. **Fresh batch smoke run produces expected artifacts.**
   - `./build/dioramap-processor batch --pipeline smoke/three-step-smoke --output-dir /tmp/out`
   - `/tmp/out/manifest.json` matches golden.
   - `/tmp/out/*/raw-dummy.bin`, `transformed-dummy.bin`, `gpu-identity.bin` exist with correct headers.

2. **Fresh serve smoke run delivers artifacts.**
   - Test WS client connects, RUNs the smoke pipeline, receives three `ARTIFACT_HEADER` + binary frames in order.
   - Final `RUN_COMPLETE` is received.

3. **Cache hits on second run.**
   - Log event stream shows `cache_hit` for all three artifacts.
   - No `step_started` events emitted.

4. **Reconnect within grace.**
   - Disconnect WS. Wait 1 minute (config grace = 10 min default). Reconnect with the same session token.
   - Catalog response lists all three artifacts as present. Resubscribe delivers from disk in <100 ms each.

5. **Late SUBSCRIBE triggers single-step recompute.**
   - Initial RUN subscribes only to `gpu-identity`. Later SUBSCRIBE requests `raw-dummy`.
   - Log stream shows it was already computed (DAG ancestor); delivered from cache. For a debug-only artifact never produced: verify runner re-walks from it and fires only the necessary step.

6. **CUDA bit-identical.**
   - Parallel runs on CPU-only and CUDA builds, same input, same seed. Output artifacts byte-for-byte equal.

7. **All CI matrix green.**
   - Including TSan on concurrent stress tests.

---

## Post-Phase-A (pointers)

Phase B is the "port real algorithms + extend runtime primitives" phase. **All of Phase B has shipped (B1–B10).** Canonical milestone list lives in [`PLAN-VIEWER-PORT.md`](PLAN-VIEWER-PORT.md) — do not duplicate it here, that breeds drift. One-line summary: B1 (fetcher foundations) → B2 (coords) → B3 (array artifacts + COPC) → **B3.5 hash-model refactor** ([`HASH-MODEL.md`](../HASH-MODEL.md) + PLAN-RUNNER T15/T16 + PLAN-STORE S12–S15, commit `f359b50`) → B4 (compute-normals) → B5 (registration chain) → B6 (classification) → B7 (ground + parcels) → B8 (shadow + deshadow) → B9 (streaming retrofit, T14) → B10 (surround fetchers + subscription gating). Phase B definition-of-done achieved: `viewer-full.json` runs end-to-end against IGN producing the full artifact catalog.

Phase C ("new frontends") is detailed in UNIFIED with three workstreams:

- **C1 — New lab UI.** All milestones shipped (PLAN-LAB-UI-PORT L1–L11). L9-followup cancelled (`ff25e3c`).
- **C2 — New viewer SDK + shared rendering package.** See [`PLAN-VIEWER-SDK-PORT.md`](PLAN-VIEWER-SDK-PORT.md) — covers both `@dioramap/scene-base` and `v3/apps/viewer-sdk/` since the two are tightly coupled. S1–S5 + S7 + V2–V8 shipped; S6 (mega-composite) cancelled; V9 (production cutover) pending.
- **C3 — Location picker + seller demo.** Map picker shipped (commit `f379e37`); seller demo shipped (A16, commit `4602dc5`).

Plus a **deferred-infrastructure** bucket distinct from the frontend ports: reverse-index cascade tracking ([`HASH-MODEL.md`](../HASH-MODEL.md)), distributed session lookup, cross-process hot-tier sharing, additional CUDA ports. Not scoped — pulled from the backlog when evidence demands.

A separate **research-track** workstream (gated on `-DDIORAMAP_RESEARCH=ON`, default OFF) evaluates third-party algorithms as candidate v3 step ports without ever entering the production binary or the SDK's subscribe list. Currently active: roof-reconstruction (4 candidates, see [`research/roof-recon-survey.md`](../research/roof-recon-survey.md) for the side-by-side comparison plan). Umbrella plan: [`PLAN-THIRD-PARTY-RESEARCH.md`](PLAN-THIRD-PARTY-RESEARCH.md).

### 2026-05-04 cross-cutting refactor: session/session_mgr/batch_ctx encapsulation

Triggered by the [BUG-cache-hit-runner-cleanup.md](../BUG-cache-hit-runner-cleanup.md) journal-acc race (caught by ASan in dev 2026-05-01).  Root cause was structural: `session_t` was a struct-bag of fields where every callsite had to remember the locking convention; the convention had drifted because the journal-acc fields were added weeks after the original session_t design without updating the rule.

The refactor (commit `d91f0ab`) made `session_t`, `session_mgr_t`, `batch_ctx_t` opaque types defined in dedicated `.c` files.  All field access goes through accessor/mutator functions that take the internal mutex; outside callers never see the lock.  The compiler now enforces what was previously convention-only.  clang `-Wthread-safety` annotations (via `core/include/v3/thread_safety.h`) catch lock-pairing violations at compile time — auto-enabled on any clang build.

Same change closed two other latent bugs caught by the audit: `parse_produced_json` realloc-UAF on partial OOM, `cp_verify_key` cache-slot reuse across the unlocked HTTP-roundtrip window.

Validation infrastructure shipped in commit `5bee4cb`: see [docs/quality/README.md](docs/quality/README.md) and [docs/quality/README.md](../docs/quality/README.md).  10 integration scenarios pass on prod under TSan with zero warnings (TSan won't run on the WSL2 dev box; bundle/scp/run procedure documented in of the how-to).

---

## Open infrastructure decisions (to resolve during Phase A, not blocking)

- Hash function: xxh3-128 (decided; see deps table above and [`HASH-MODEL.md`](../HASH-MODEL.md)).
- Config format: TOML (tomlc99 or similar; no JSON for config — JSON has no comments).
- JSON serialization: yyjson (fastest, simple C API).
- Allocator: system default; revisit if profiling shows allocator pressure.
- Test framework: hand-rolled ~200 LOC (no C testing framework is worth the weight for our needs).

None of these block Phase A start; they can be finalized in the relevant subsystem PRs.

---

## Control-plane workstream (A10 + A14 + A15 + A16)

Self-contained lane that takes the v3 stack from "runs locally on a dev box" to "runs in production behind dioramap.com with billing, auth, and a seller-shaped end-to-end demo." Decoupled from the smoke-pipeline critical path; gated on integration with `~/control-plane/` (the Node Fastify + Postgres SaaS backend) rather than on any C-side subsystem.

### Dependency graph

```
        ┌──────────────────────────────┐
        │ A15 — CP v3 migration        │ ← independent, ships first
        │ (SDK swap, deploy, dashboard)│
        └──────────────────────────────┘
                       │
        ┌──────────────│───────────────┐
        ▼              ▼               │
        ┌──────────────────────────────┐
        │ A10 — CP client (C side)     │ ← independent of A15;
        │ libcurl: verify-key + record │   ships in parallel
        └──────────────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────┐
        │ A14 — Run telemetry          │ ← needs A10 + L6
        │ pipeline_runs + record-run   │   (PLAN-LAB-UI-PORT)
        └──────────────────────────────┘
                       │
        ┌──────────────│
        ▼              ▼
        ┌──────────────────────────────┐
        │ A16 — Seller-demo            │ ← needs A15 (bundle in CP)
        │ end-to-end auth flow         │   + A10 (verify-key works)
        └──────────────────────────────┘
```

### Audit findings driving A15 (2026-04-27)

The control-plane already has substantial infrastructure (Postgres schema, auth, orgs, api_keys, billing via Stripe, usage events, internal endpoints, EJS dashboard). What needs adjustment for v3 is mostly migration mechanics, not net-new features.

**SDK bundle hosting** — `web/static/sdk/dioramap-sdk.js` is the legacy v2 build (~30k LOC). Referenced via ES dynamic import at `web/static/viewer-page.js:90`; v3's UMD `window.DioramapViewer.init` requires a different load pattern. No SRI checksums, no version-pinning query string, no cache headers in `src/server.js`. Decision: replace in-place + add `?v=<hash>` cache-bust. CDN-publishing deferred until customer count justifies it.

**Internal endpoints** — `/internal/verify-key` and `/internal/record-usage` (`src/internal/routes.js:38-150`) shape is stable; A10's C client wraps them as-is. A14 adds `/internal/record-run` for run-level telemetry (separate table from `usage_events` because retention + access patterns differ).

**Usage event schema** — `usage_events.event_type` is `TEXT` (no CHECK), but every dashboard query in `src/usage/routes.js:35-47` hardcodes `WHERE event_type = 'scene_load'`. Migration `007-event-types.sql` (under A15) generalises this so v3-emitted events don't get silently undercounted.

**Config naming** — `src/web/routes.js:60` injects `backendWs` into the EJS template; v3 SDK's frozen `init({backendUrl})` surface (PLAN-VIEWER-SDK-PORT) accepts `backendUrl`. Reconciled by renaming the EJS-side template variable (cheaper than breaking the SDK's public contract).

**Deploy story** — control-plane has a clean Fastify Node app (Docker-ready); `dioramap-processor` (the C binary) has neither a Dockerfile nor a systemd unit. A15 ships at minimum a Dockerfile so production deploys aren't a manual `make install`.

**Out of scope for this lane** — `control-plane/docs/SCENES-DESIGN.md` describes a save/load/share scene feature that's currently paper-only. A16 deliberately excludes it: end-to-end auth flow doesn't need scene persistence to validate. SCENES-DESIGN gets its own milestone if and when a customer asks.

### Dev-up script (`control-plane/scripts/dev-up.sh`) — local-only

The existing `dev-up.sh` (~280 LOC) orchestrates the full local stack with `up` / `down` / `restart` / `status` / `logs` subcommands. It is **explicitly a developer-only tool, not a production deploy mechanism** — port-ownership checks against `/proc/$pid/cwd` + cmdline regexes, log-tailing via `tail -F`, hardcoded localhost ports, etc. Production uses Docker / systemd (added in A15's P0). The dev-up script stays a separate, lighter-weight piece used for local end-to-end infra tests.

The script is currently v2-shaped and needs modernisation as part of A15:

- **Server B → `dioramap-processor`.** Drop the `$BACKEND/server.js` Node startup path; replace with the v3 C binary (`./build/dioramap-processor serve --config dev.toml` or equivalent). No `npm install` for it — the binary is built via `cmake --build build`, which the script should invoke if the binary is missing or stale.
- **Cmdline ownership signal.** `SB_CMD_RE="server\.js"` no longer matches; replace with a regex that uniquely identifies `dioramap-processor`.
- **Port.** Server B's `:13001` → whatever the v3 processor listens on (lab-ui pins `:7801`); keep it parameterised via env var so it tracks future changes.
- **Health probe.** Server B exposes `/health` HTTP today; the v3 processor's HTTP surface is deferred (per UNIFIED-ARCHITECTURE). Until that's resolved, the script's `wait_for_url` for the processor falls back to `wait_for_port` (TCP open) — the helper is already in the script.
- **Env emission.** The `.dev-server-b.env` writer (Node-style env vars) gets dropped or replaced with a TOML config emitter (`dioramap-processor serve --config cfg.toml` per UNIFIED).
- **Seller-demo path.** `$VIEWER/seller-demo` (the legacy demo's path) becomes `$ROOT/../lidar-algos/v3/apps/seller-demo` once A16 lands. Until then, the script can keep launching the legacy demo for parallel-stack comparison.

What stays the same: Postgres bootstrap, migrations + seed flow, `seed-dev-key.js` → `?apiKey=` URL pattern, the up/down/status/logs subcommand surface, the port-ownership safety pattern. The script's overall shape is good; only the Server B section is obsolete.

### Workstream commit order

Suggested: **A15 → A10 → A14 → A16**. A15 and A10 can run in parallel (different repos, no shared touchpoints). A14 must follow both. A16 is the validation surface — proves the entire chain works.

### Privacy / data flow invariant (preserved across A14 + A16)

The processor's local SQLite journal (L6 — `<cache_root>/runs.sqlite`) is the source of truth for full-fidelity run details (input JSON, artifact hashes, error stacks). The control-plane stores only a denormalised aggregate (input *hash*, not input bytes) so support reproduction always requires the processor box to be reachable. Customer input data never leaves the customer box. This is by design and applies to every milestone in this lane.
