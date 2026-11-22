# Unified Architecture — Processing Core

*Design doc. 2026-04.*

Consolidates the processing layer of `lidar-algos-bench-v2` (algorithm development bench) and `lidar-3d-environment-viewer` (customer-facing 3D viewer SaaS) into a single artifact. The lab UI and the viewer SDK become two consumers of the same core.

---

## 1. Aims

1. **Performance.** Move compute off JavaScript/V8. Current hot paths (kd-tree/PCA normal estimation, Sobel, shadow-cast, rasterization, deshadow) are the bottleneck on both bench iteration speed and viewer request latency.
2. **IP protection.** The algorithms are the product. Shipping readable JavaScript to customer-accessible prod servers leaks the moat. A stripped, statically linked C binary raises the reverse-engineering cost substantially.
3. **Unify the two projects.** Today the bench and the viewer duplicate primitives (kd-tree, PCA, lambert93, ortho helpers), evolve them independently, and drift. The algorithms are the same asset; they should live in one place.
4. **Keep the dev UX.** Algorithm development in bench V2 is fast and visual. The target architecture must preserve sub-second edit-to-visualisation loops and rich artifact inspection.
5. **Be CUDA-ready without committing to CUDA.** The design must accommodate GPU offload later without a redesign.

## 2. Constraints

- **No rewrite from scratch.** The existing 31,500 LOC of JS compute code encodes ~10× that in discarded experiments, calibrated thresholds, and regression fixtures (219 test cases). This is the specification; a from-scratch rewrite would discard it. Port piece by piece; translate, don't redesign.
- **No dual JS/C capability.** An algorithm is in exactly one language at any moment. When ported, the JS version is deleted on the same PR. No long-term bridge; no graduation back-and-forth.
- **Static prod binary.** Prod servers run exactly one process. No Node runtime on the customer box. All outbound I/O (control plane, IGN tile fetch) happens from C.
- **Batch mode is first-class.** The binary must be usable without the WebSocket server — reading a run specification from args or stdin, writing artifacts to a directory. WS is one delivery channel among several, not the framework.
- **Lab UI and Viewer SDK stay JavaScript.** They run in browsers; they're not IP (no compute in either); they're the right tool for their job.

## 3. Decisions (summary)

| # | Decision | Rationale |
|---|---|---|
| D1 | Single C binary for all server-side runtime | Eliminates Node from prod box; unifies WS + HTTP + compute + fetch in one process |
| D2 | libwebsockets for WS + HTTP + static file serving | Mature, statically linkable, server and client in one lib |
| D3 | libcurl + mbedTLS for outbound HTTPS | Control plane, IGN WMS/WFS |
| D4 | jansson for JSON (was yyjson; see §13 Q11), PROJ for coords, laz-perf (C++) for LAZ | Conservative, proven picks |
| D5 | Port algorithms piece by piece; no from-scratch rewrite | Preserves 10+ years of implicit spec in existing code |
| D6 | No dual JS/C per step; JS deleted at port time | Eliminates drift and dual-maintenance tax |
| D7 | Content-addressed artifacts; process-global store | Enables sharing across sessions, cache hits, late subscribe |
| D8 | Pull-based artifact delivery with visibility tiers | Viewer never sees debug artifacts; lab UI sees all |
| D9 | Demand-driven pipeline execution (DAG walk from subscribed) | Viewer skips debug steps automatically |
| D10 | Same binary layout across heap, disk cache, and WS wire frames | One serialization format; zero-copy where possible |
| D11 | Binary ships with a batch mode (`batch` subcommand) | Tests, CI, repros, scripted pipelines don't need WS |
| D12 | Compile-time CUDA flag (`-DDIORAMAP_CUDA=ON`); not runtime-fallback | Two build targets simpler than dual-impl per step |
| D13 | Lab UI moves to WS; same protocol as viewer | Unifies observer model; streaming artifacts during dev |
| D14 | Inbound TLS terminated by nginx; `dioramap-processor` speaks plain WS/HTTP | Keeps libwebsockets built without OpenSSL; one place to manage certs; outbound TLS stays inside the binary via libcurl |

Decisions NOT to do (explicit):

- **Not** N-API / Node native addons. Node ecosystem lock-in; `.node` files ship to prod alongside JS source — weaker IP protection than a stripped ELF.
- **Not** WASM. Easier to reverse than a stripped binary. Defeats the IP goal.
- **Not** shared memory files (`/dev/shm`). Once everything is in one process, no IPC boundary exists. The binary layout that shm was carrying survives as the in-process artifact representation.
- **Not** a dedicated RPC protocol (gRPC, Cap'n Proto) between server and clients. WS binary frames carry the same layout; adding an RPC framework is overhead.

---

## 4. Vocabulary

These nouns are load-bearing. Their definitions are the contract.

### 4.0 Acronyms and abbreviations

| Term | Meaning |
|---|---|
| AoS | Array of Structures (record-interleaved memory layout) |
| API | Application Programming Interface |
| ABI | Application Binary Interface |
| BIL | Band Interleaved by Line (elevation raster format) |
| CDN | Content Delivery Network |
| CI | Continuous Integration |
| CLI | Command-Line Interface |
| COPC | Cloud Optimized Point Cloud (spatially-indexed LAZ) |
| CPU | Central Processing Unit (host) |
| D2D / D2H / H2D | Device-to-Device / Device-to-Host / Host-to-Device (CUDA) |
| DAG | Directed Acyclic Graph (the step dependency graph) |
| DSM | Digital Surface Model |
| ELF | Executable and Linkable Format (Linux binary) |
| FIFO | First-In-First-Out (pipe/queue order) |
| GC | Garbage Collection |
| GPU | Graphics Processing Unit (device) |
| GT | Ground Truth (test case reference output) |
| H2D | Host-to-Device (CPU → GPU memory transfer) |
| HMR | Hot Module Reload |
| HTTP(S) | HyperText Transfer Protocol (Secure) |
| IGN | Institut National de l'Information Géographique (French geographic authority) |
| IoU | Intersection over Union (metric) |
| IP | Intellectual Property |
| JSON(L) | JavaScript Object Notation (Lines) |
| JWT | JSON Web Token |
| KDE | Kernel Density Estimation |
| LAS / LAZ | LiDAR data format (uncompressed / compressed) |
| LiDAR | Light Detection And Ranging |
| LOC | Lines Of Code |
| LOD | Level Of Detail |
| LRU | Least-Recently-Used (eviction policy) |
| MRF | Markov Random Field |
| NIR | Near-Infrared (ortho imagery channel) |
| OBJ / STL | 3D model file formats |
| PCA | Principal Component Analysis |
| PCIe | Peripheral Component Interconnect Express (bus) |
| PR | Pull Request |
| RANSAC | Random Sample Consensus |
| SaaS | Software as a Service |
| SDF | Signed Distance Field |
| SDK | Software Development Kit |
| SIMD | Single Instruction Multiple Data (AVX2, etc.) |
| SLA | Service-Level Agreement |
| SoA | Structure of Arrays (column-oriented memory layout) |
| TLS | Transport Layer Security |
| TTL | Time-To-Live (cache expiration) |
| UUID | Universally Unique Identifier |
| VRAM | Video RAM (GPU memory) |
| WASM | WebAssembly |
| WFS / WMS | Web Feature / Map Service (OGC protocols, IGN endpoints) |
| WS | WebSocket |

### 4.1 Step

A pure function `(context, params) → new artifacts`. Has a metadata record (`meta.json`) declaring:

```json
{
  "name": "compute-normals",
  "version": 7,
  "reads":  ["points", "bbox"],
  "writes": [
    { "name": "normals", "kind": "soa-f32",
      "visibility": "public", "stability": "stable" }
  ],
  "compute_location": "cpu",
  "streaming": false
}
```

- `name` is the canonical identifier, globally unique in the step registry.
- `version` is bumped whenever the computation changes (invalidates cache).
- `reads` / `writes` are artifact names — the DAG. Wiring is **always explicit**; there is no default/implicit reads/writes inference.
- `compute_location` is where the step's code runs: `"cpu"` (host threads) or `"gpu"` (CUDA device). See §4.4.
- `visibility` per write restricts who can subscribe (see §4.5).
- `stability` per write marks API commitment (`stable`, `unstable`).
- `streaming: true` means the step emits multiple partial writes before completing.
- `skip_on_identity: false` is an optional per-write escape hatch for genuinely non-deterministic steps (e.g., wall-clock capture). Default `true`. See HASH-MODEL.md for the unified content-hash model.

All production steps are native C. There is no per-step JS implementation in the production runtime. (The lab UI may, on an ad-hoc basis for specific panels, run small JavaScript prototypes in-browser to interactively tune parameters and re-render without hitting the server — that is a lab-UI feature, not a step property, and is not formalized in `meta.json`.)

**Multi-algorithm steps.** A single step may host a registry of interchangeable algorithm variants — e.g., current `boundary-edges` has 18 algo variants (`champion`, `contour-snap`, `assembler-4d`, `signal-sdf`, ...) selected at runtime via a param. In the target, this pattern survives: one step directory, one `meta.json`, multiple native C implementations internally dispatched by `params.algo`. The step's declared `writes` are the union of any algo's writes — all variants produce the same-named artifacts, differing only in values. The old pattern of "legacy algos stay in JS for comparison" is not architectural; if a legacy algo matters enough to keep, it ports to C under `visibility: "debug"` like anything else.

**Iterative / monolithic steps.** Steps like `shadow-fit` (coarse-to-fine sun search, ~400 internal shadow-cast calls) and `ortho-lidar-register` (multi-pair pyramid search) are **single steps** from the DAG's perspective. Their internal iteration is not decomposed into sub-steps. An iterative step may emit intermediate debug artifacts (e.g., `shadow-fit-candidates`, `registration-pyramid-scores`) with `visibility: "debug"` for lab UI inspection. These debug artifacts have the same content-hash semantics as any other write; they're just not in the public catalog.

### 4.2 Artifact

A named, typed blob with a canonical binary layout (§5). Referenced by:

- **Name** within a pipeline run (`"normals"`).
- **Content hash** globally (`hash(pipeline_input, step_name, step_version, step_params, hash(reads))`). Content-addressed.

Artifacts are immutable once produced. Re-running a step with identical inputs yields the same content hash.

### 4.3 Pipeline

An ordered list of step invocations, declarative (JSON), not code. Pipeline definitions live in `core/pipelines/`. Examples:

- `viewer/full.json` — end-to-end pipeline from tile fetch to assembled 3D buildings.
- `bench/v2-champion.json` — bench's reference pipeline for algorithm comparison.

A pipeline names the steps that can run in a session; it does **not** imply every step will execute. The runner walks the step DAG from subscribed artifacts backward and executes only what's required (§4.6).

**Each pipeline declares its own input schema.** The RUN message's `input` field is typed by the pipeline — one pipeline consumes `{lat, lon, radius}`, another consumes `{obj_path}`, another consumes `{bbox_lambert93, date_range}`, whatever fits. The core runtime has no built-in notion of "place coordinate" or any other specific domain; the pipeline owns its input shape entirely. This genericity is a feature: the same core supports very different processing cases (lidar, imported 3D models, time-series, whatever comes next) without architectural changes. At RUN time the canonicalized input bytes enter the DAG as the root artifact `pipeline-input` (§9.2) and participate in the hash chain from there.

Three kinds of inputs flow into a step, kept distinct:

| Input kind | Source | Where in the hash |
|---|---|---|
| **reads** | upstream artifacts produced by other steps | `sorted_read_hashes` |
| **params** | the pipeline-definition JSON | `step_params_canonical_json` |
| **pipeline args** | the user's RUN payload | *indirectly*, via the `pipeline-input` root artifact read by input-source / normalization steps |

Params are part of the pipeline's contract (fixed per pipeline definition, same for every run). Pipeline args are per-RUN user input. A step's `meta.json` declares which params it accepts; the pipeline definition supplies values; the runner plumbs user input through the root artifact.

### 4.4 Compute location and artifact placement

Two distinct concepts that were previously conflated under "residency."

**Compute location** is a property of a **step**: where its code runs.

- `cpu` — C code running on a host thread in the compute thread pool
- `gpu` — CUDA kernels launched from a host thread, executing on the GPU device

Declared in `meta.json` (§4.1). Fixed at build time.

**Placement** is the transient state of an **artifact's bytes** during a pipeline run: where they currently live. Not declared; managed by the runner.

- `heap` — host RAM, allocated by the process
- `device` — GPU VRAM, allocated via CUDA (only when CUDA build is enabled)
- `disk` — serialized into the on-disk cache file (§9)

Wire (in-flight WS frame) is not a placement — it's a transport event, not a resting state.

**The runner is responsible for placement, not the step.** When the runner dispatches a step:

1. For each read in `reads`: ensure the artifact's bytes are at a placement the step's `compute_location` can access (heap for CPU steps, device for GPU steps). If not, transfer (§7.4). Transfers are ideally cheap: heap→heap is a no-op; heap→device is `cudaMemcpyH2D`; disk→heap is `mmap`.
2. The step runs, reads its inputs where the runner placed them, writes its outputs to the matching placement (heap for CPU, device for GPU).
3. After the step completes, the runner decides what to do with each write: keep it in placement if a downstream step needs it there, flush it to disk cache (always for subscribed outputs), schedule it for WS delivery. Heap/device copies are freed when no longer needed for compute or delivery.

**At rest, artifacts live on disk.** The in-memory placements are strictly execution-time: a heap buffer exists for the duration of its usefulness during a run and is freed afterwards; the authoritative copy is the cache file. Long-term storage is a cache-system responsibility (§9). Nothing lives in heap "just in case."

### 4.5 Visibility

Each write declares one of:

- `public` — visible to both viewer clients and lab UI
- `debug` — visible to lab UI only

The server filters the catalog based on the connecting client's tier. Viewer subscriptions to debug artifacts are rejected. See §8.4.

> **Status (2026-04-20):** catalog entries carry the `visibility` field, but tier enforcement is not wired — every connection is treated as `debug` today. Gating activates in A10 when the control-plane client can resolve a tier from an API key.

### 4.6 Subscription

A client expresses interest in specific artifacts by name at RUN time (or later via SUBSCRIBE). The runner's DAG walker produces only what's needed to satisfy active subscriptions. Artifacts produced but never subscribed to may still be cached (for late subscribers in the same session).

### 4.7 Session

The anchor tying a client's state together across a logical interaction: RUN, incremental SUBSCRIBEs, disconnects, reconnects, and eventual destruction.

**Session lifecycle** (combined with runner and store, as they intertwine):

1. **Open.** Client connects via WS. Server assigns a session token. Tier is established (§4.5). Empty subscription set.
2. **RUN.** Client declares pipeline + input payload (per the pipeline's declared schema) + initial subscriptions. Runner writes the canonicalized input as the `pipeline-input` root artifact (§9.2), then walks the DAG backward from the subscribed artifact names, executes only the necessary steps. As each subscribed artifact is produced, it is written to the on-disk cache and scheduled for WS delivery to this session. Session records disk-cache references (not heap references) to each subscribed artifact.
3. **Quiescent.** Once all subscribed artifacts have been produced, cached, and delivered, **the runner releases all compute resources** for this session — heap buffers freed, compute threads idle, no residual work. The session persists only as a thin record of (session_token, disk_cache_references, tier, last_seen). Nothing is being computed or held in RAM on its behalf.
4. **Late SUBSCRIBE.** If the client later requests a new artifact, the runner re-evaluates the DAG: it walks backward from the newly-subscribed artifact, discovers which ancestors are already cached (disk hit via content hash), and schedules only the uncomputed tail. Cached ancestors are `mmap`'d back to heap/device as needed for the downstream step. After the new artifact is produced, cached, delivered, the session returns to quiescent.
5. **Disconnect.** WS closes. Session enters **grace period** (configurable, default 10 min). Its disk-cache references remain held — those artifacts are pinned against cache eviction.
6. **RECONNECT.** New WS connection; client supplies the session token. Session is rehydrated; disk references still valid; client can re-subscribe (often instant hits on cache) or issue new RUNs.
7. **Grace expires.** Session is destroyed. Its disk-cache references are released. Artifacts whose only ref was this session become eligible for LRU eviction; if they're still referenced by other live sessions or by recent runs, they remain. See §9.3.

**Key property: resource release is eager.** The runner does not hold heap copies of artifacts "just in case" a session might want them again. Heap is for compute in progress; disk is for everything else. Late subscriptions tolerate the disk→heap round-trip cost precisely because it's cheap (`mmap`) and the alternative (hoarding heap) scales badly.

### 4.8 Runner

The process-wide dispatcher that executes steps. Not a thread — it's a set of queues plus a compute thread pool. Receives "produce these artifacts" tasks from the main event-loop thread, walks the DAG backward from requested artifacts, executes uncached steps in topological order, pushes each output to the store. See §7.

### 4.9 Artifact Store

The content-addressed service that answers "give me artifact with hash X." Unifies cache lookup, runner scheduling (on miss), and session reference counting.

**Terminology.** Throughout this doc the **Store** is the whole abstraction; "cache" is the adjective for its disk-backed persistence layer ("cache file", "cache hit", "cache miss"). There is no separate "cache system" — disk persistence is the store's storage.

**Placements tracked by the store:**

| Placement | Role | Created when | Freed when |
|---|---|---|---|
| `heap` | in-flight compute | a step is about to read it or has just written it | no downstream step needs it + already flushed to disk |
| `device` | in-flight GPU compute | a GPU step is about to read / has just written | same logic, GPU-side |
| `disk` | authoritative at-rest copy | step output is subscribed or intermediate ancestor of a subscribed output | LRU eviction under cache budget, with session-ref pinning |

The store looks up an artifact by content hash. On hit, returns a handle pointing at whichever placement currently has it (usually disk). On miss, enqueues a runner task to produce it.

**Why session-aware?** Because the store's eviction policy otherwise has no reason to keep disk entries around once they're delivered. Session refs are what tell the store "this disk entry has a live interested client — don't evict for now, even if cache budget is pressing." Without session refs, the store would evict freshly-delivered artifacts the moment cache budget filled up, breaking the "reload/late-subscribe is fast" property (§4.7 step 4). Session refs are the only reason the store cares about sessions — everything else (content addressing, disk layout, mmap) is session-agnostic.

### 4.10 SDK API stability (distinct from artifact stability)

Artifact `stability` (§4.1) refers to the schema of a binary payload. The **viewer SDK itself** (`DioramapViewer.init(...)`, control-panel API, event callbacks) has its own API surface, versioned separately under semver. SDK stability commitments live in `apps/viewer-sdk/CHANGELOG.md`; artifact stability lives in each step's `meta.json`. These are independent contracts — the binary payload can change shape (artifact version bump) while the SDK wrapper absorbs the difference internally for one minor release before exposing it.

The shared rendering package **`@dioramap/scene-base`** has its own semver lane independent of both the SDK and any artifact (`apps/scene-base/CHANGELOG.md`). Composite-visualizer additions or behaviour changes that don't break the `RenderContext` API are minor; removed or renamed composite-visualizers are major. An artifact-shape bump can be absorbed inside scene-base for one minor release, leaving the SDK's CHANGELOG silent — only when the SDK itself surfaces something new does its own version move. Same pattern applies to `@dioramap/protocol`. See PLAN-VIEWER-SDK-PORT §7 for the package contract details and [VISUALIZERS.md](VISUALIZERS.md) for the visualizer subtype contract.

### 4.11 Lazy fetch / lazy compute pattern

A consequence of §4.6 (subscription) and the backward-DAG-walk: anything expensive is lazy for free as long as it's its own step.

**Rule of thumb: one decision point = one step.** In particular, **one fetch = one step**, one artifact per fetch. Don't bundle heterogeneous fetches into a single input-source step, because the DAG walker can't prune inside a step — only around it.

Concrete decomposition for the viewer's current monolithic "fetch everything" entry:

```
fetch-lidar-tile        reads: [bbox]            writes: [raw-lidar-tile]
fetch-ortho-tile        reads: [bbox]            writes: [raw-ortho-tile]
fetch-elevation-tile    reads: [bbox]            writes: [raw-elevation-tile]
fetch-cadastre-tile     reads: [bbox]            writes: [raw-cadastre-tile]
fetch-photo-tile        reads: [bbox, date]      writes: [raw-photo-tile]
```

Each fetch is pruned independently by the runner when its output isn't transitively needed.

**Downstream consequences:**

- **Lab UI iterating on boundary detection.** Subscribes only to `boundary-signal-2d`. DAG walks back through `boundary-edges` → `segment` → `compute-normals` → `extract-denoise` → `raw-lidar-tile` → `fetch-lidar-tile`. No ortho, no elevation, no cadastre, no photo fetched. Fast loop, no wasted HTTPS.
- **Viewer subscribes to `buildings-lod2` + `color-raster` + `shadows-map`.** Union of three backward walks: lidar + ortho + elevation + photo fetched, cadastre skipped.
- **Shared fetches are deduplicated automatically.** Two subscriptions both reaching `raw-lidar-tile` → fetched once (content hash), reused.

**LOD progression via late-subscribe:**

Same pattern extends to multi-resolution data:

```
fetch-ortho-lod0        writes: [raw-ortho-lod0]    # 2m/px
fetch-ortho-lod1        writes: [raw-ortho-lod1]    # 0.5m/px
fetch-ortho-lod2        writes: [raw-ortho-lod2]    # 0.2m/px
```

Viewer initially subscribes to a `color-raster-lod0`-dependent branch. User zooms in → SDK sends `SUBSCRIBE { artifact: "color-raster-lod2" }` → store sees it's cold → runner walks backward, reuses cached lod0 fetch where relevant, executes `fetch-ortho-lod2` on demand. Higher-resolution data was never fetched until the zoom requested it.

**When decomposition is wrong:**

If a single HTTPS call atomically returns multiple related artifacts (e.g., an IGN WFS query returning both building polygons and parcel IDs in one response), splitting into two steps would issue two identical calls. Two acceptable options:

1. **Keep as one step with multiple writes.** Step runs if any of its writes is subscribed. Produces all writes unconditionally. Slightly less lazy but avoids redundant fetches. Default for atomic multi-output fetches.
2. **One "raw response" step + N "unpack" steps.** Raw response is a cached artifact; cheap unpackers split it. Cleaner lazy story, more steps. Use when the atomic response is expensive enough that redundant-fetches would hurt noticeably.

**The anti-pattern to avoid:**

Do not write a `fetch-all-tiles` step with conditional branches inside that consults `params.needOrtho`, `params.needCadastre`, etc. Steps' `writes` declarations must be static for the DAG walker; conditional-writes break the subscription contract. If you catch yourself wanting this, decompose.

### 4.12 Observer / Delivery Channel

Where produced artifacts go. At least three:

- **WS binary frame** — interactive client (viewer SDK, lab UI)
- **File on disk** — batch mode (`batch --output-dir`)
- **Nowhere** — artifact produced only to feed downstream steps, never externalised

Store emits "artifact available" events; each session (or batch job) decides whether to consume.

---

## 5. Data model: the artifact binary layout

The single source of truth for artifact representation. Used in heap, on disk, over the wire.

```
┌──────────── header (128 B, aligned to 64) ────────────┐
│ magic[4]          "DMAP"                              │
│ version           u32                                 │
│ artifact_kind     u32   (see kind table)              │
│ count_N           u64                                 │
│ n_columns         u32                                 │
│ flags             u32   (reserved)                    │
│ metadata_offset   u64   (JSON blob offset, or 0)      │
│ metadata_len      u64                                 │
│ reserved          [... padding to 128 B ...]          │
├──────────── column descriptors (n_columns × 32 B) ────┤
│ col[i].name_hash  u32                                 │
│ col[i].dtype      u8   (f32, f64, u8, u16, u32, i32…) │
│ col[i].stride     u8                                  │
│ col[i].flags      u16                                 │
│ col[i].offset     u64                                 │
│ col[i].length     u64                                 │
│ col[i].reserved   [u8; 8]                             │
├──────────── optional metadata JSON blob ──────────────┤
│ …utf8 JSON…                                           │
├──────────── column data, dtype-aligned ───────────────┤
│ column 0 bytes                                        │
│ column 1 bytes                                        │
│ …                                                     │
└───────────────────────────────────────────────────────┘
```

Kinds (`artifact_kind`):

| kind | meaning | example |
|---|---|---|
| 0 | `pointcloud-soa` | x, y, z, classification, intensity, return |
| 1 | `per-point-array` | normals (nx, ny, nz, confidence) |
| 2 | `geometry-primitives` | `{primitive: "lines" | "polylines" | "cylinders" | ...}` as metadata + item columns |
| 3 | `raster-2d` | W × H grid, pixel columns |
| 4 | `polygons3d` | variable-length polygons, offsets column |
| 5 | `plane-records` | normal, d, indices, tags |
| 6 | `mask-bitset` | packed bits |
| 7 | `opaque-json` | everything under 64 KB that doesn't fit above |
| 8 | `array-inline` | Ordered collection whose elements live inline in the artifact's bytes. Bytes = `[N, stride, item₀, item₁, …]`. Variable-length items (`stride==0`) carry a per-item header `[u32 len][u32 zero_pad]` and are followed by trailing padding so each payload starts at an 8-byte boundary (Phase 5.1d). Typical payload: small JSON objects (URL list, metadata list), per-building plane records, kd-tree node arrays. See §5.3. |
| 9 | `array-by-hash` | Ordered collection whose elements live as separate store entries. Bytes = `[N, hash₀, hash₁, …]` (32 B per hash). Each element is independently content-addressed. See §5.3. |
| 10 | `point-column` | Single per-point column. Raw bytes (F32/F64/U8/U16); the step name + metadata JSON carry the schema. Used by `laz-extract-disc` for `lidar-positions`, `lidar-classifications`, etc. |
| 11 | `raster-f32` | Float32 grid, row-major, raw bytes. Metadata (cols, rows, bbox, cellSize) lives in step params / sibling artifacts. Used for BIL elevation. |
| 12 | `image-jpeg` | Compressed JPEG bytes. Pass-through payload from ortho WMS fetches. |

For small artifacts (<64 KB, typically `opaque-json` kind), the header + metadata blob is the whole artifact; no column data.

Design properties:

- **Endian-fixed.** Little-endian. Assert at header parse; refuse if mismatched.
- **Append-only.** Columns are written in order; readers index by `col[i].offset`.
- **Schema-in-file.** Column names are hashed in the header; the metadata JSON blob carries human-readable column names and per-kind extras.
- **Zero-copy.** `mmap` a cache file or receive a WS binary frame → typed array views via `col[i].offset` → direct access.

### 5.1 Why `mmap` for cache reads

Three reasons:

1. **Zero-copy into address space.** `mmap(PROT_READ)` gives you a pointer into the kernel's page cache. No `malloc` + `fread`; no double-buffering. Data is loaded on demand as pages are touched.
2. **Shared cache across processes / restarts.** Page cache survives process restart; a warm cache stays warm across deploys. Same file can be mmapped by any number of concurrent reader contexts.
3. **Partial access is free.** SoA layout means a single column can be read by touching only its pages. A point cloud that contains x/y/z + classification + intensity can be processed reading only x/y/z if that's all the current step needs. The unused columns never page in. This is especially useful for wide artifacts where downstream steps consume subsets.

Writes are not mmapped — they go through a plain write path (`write()` + `fsync` batched by the janitor). Append-only + content-addressed means writes never update in place; a new artifact is a new file.

### 5.2 Layout for complex structured artifacts

Most artifacts fit cleanly: point clouds are SoA columns; masks are bit arrays; rasters are 2D pixel grids. But some artifacts are heterogeneous compositions — categorized layers of varying primitive types, each with its own items. These need more care.

**Worked example: `boundary-3d` layers.**

The current `boundary-3d` step produces output shaped roughly like:

```
{
  layers: [
    { id: 'rg-3-merged-edges',        primitive: 'lines',     category: 'Roof Graph',
      subcategory: '3-merged', label: 'Edges (42)', defaultVisible: true,
      items: [{from:{x,y,z}, to:{x,y,z}, color, type}, ...] },
    { id: 'rg-3-merged-shared-edges', primitive: 'cylinders', ...items: [...] },
    { id: 'oa-grid-p0',               primitive: 'points',    ...items: [...] },
    ...
  ]
}
```

Hundreds of layers, thousands of items per layer, mixed primitive types. Two viable encodings:

**Option A: flat columnar with layer index** (kind `geometry-primitives`, kind=2).

One artifact holds all items across all layers in flat SoA columns, with a `layer_id` column tagging each item. Layer definitions live in the metadata JSON blob.

```
kind = 2 (geometry-primitives)
count_N = 12_345   (total items across all layers)
columns:
  layer_id       u32[N]        # which layer each item belongs to
  primitive      u8[N]         # 0=line, 1=point, 2=cylinder, 3=polyline
  from_x,y,z     f32[N] ×3     # start point (NaN for primitives that don't have one)
  to_x,y,z       f32[N] ×3     # end point
  color_rgb      u8[N] ×3      # per-item color
  type_code      u32[N]        # enum for per-primitive type discriminator

metadata JSON:
{
  "layers": [
    { "id": "rg-3-merged-edges", "category": "Roof Graph", "subcategory": "3-merged",
      "label": "Edges (42)", "default_visible": true, "primitive_label": "lines" },
    ...
  ],
  "type_enum": {"1": "ridge", "2": "eave", "3": "valley", ...}
}
```

Pro: one artifact, one binary frame, one `mmap`. Layer membership is just a column. SoA-friendly on GPU for batch transforms.
Con: variable-length primitives (polylines) don't fit a fixed AoS row.

**Option B: one artifact per primitive type, sharing a manifest.**

Split boundary-3d into, e.g., `boundary-3d-lines` (kind 2), `boundary-3d-cylinders` (kind 2), `boundary-3d-polylines` (new kind), all subscribed together. Lab UI composes them visually via a shared metadata descriptor.

Pro: each artifact is internally homogeneous; polylines (variable-length) get their own kind with `polyline_offsets u64[]` + flat point columns.
Con: more artifacts, more delivery overhead, more subscription complexity.

**Recommendation: Option A with a side-channel for variable-length kinds.** Use flat columns for points/lines/cylinders (~95% of `boundary-3d` items in practice). Polylines and any other variable-length primitives get an additional set of columns (`polyline_offsets`, `polyline_vertex_xs/ys/zs`) also referenced by `layer_id`; the item-row's `primitive` field tells readers whether to look in the flat row or the variable-length side arrays.

**For `roof-graph` specifically** (graph of ridges and eaves with typed edges): a typed-edge graph fits kind 2 cleanly. Vertices and edges both live in the same artifact with distinct `layer_id`s (`rg-vertices`, `rg-edges`), `primitive` set to `"points"` or `"lines"`, type codes enumerated in metadata.

**When none of the above fit**, fall back to kind 7 (`opaque-json`). Acceptable if the artifact is small and its structure is genuinely idiosyncratic (hundreds of KB, bounded complexity). The cost: no columnar access, no zero-copy to GPU. `opaque-json` is an escape hatch, not a default.

### 5.3 Array artifacts (kinds 8 and 9)

Some steps produce a collection of items whose count is data-dependent — e.g. "the list of COPC tiles that cover this bbox" is 1–4 URLs at query time. Array artifacts are the typed shape for "ordered N-element output" without asking the runner to fan out.

**When to use which.**

- `array-inline` (kind 8) for small element payloads that are worth serializing inline: URL lists, small JSON config objects, index manifests, per-building plane records, kd-tree node arrays. The whole array is one store entry; reading element `i` is a pointer arithmetic into the bytes — variable-length records carry a fixed 8-byte header (`[u32 len][u32 zero pad]`) and are followed by enough trailing pad zeros that the next record starts on an 8-byte boundary, so element bytes can be safely typed-pointer-cast even if the record contains a `double`. Fixed-stride records (stride > 0) skip the header — caller picks an alignment-friendly stride.
- `array-by-hash` (kind 9) for large element payloads where each element deserves its own store entry. The array's bytes are a manifest `[N, hash₀, hash₁, …]` of 32-byte hashes; the elements themselves are ordinary artifacts living elsewhere in the store. Two different manifests that share an element reuse the element's store entry — no duplication.

**Ordering contract.** The producing step is responsible for emitting elements in a deterministic canonical order (typically: sort by a stable key derived from the element, like URL string). Two runs with the same logical inputs must produce byte-identical manifests → the same content hash → downstream cache hits. Non-canonical order is a correctness bug, not a performance issue.

**How steps read them.** Step registration declares per-read `array: true`; the `step_context_t` exposes `step_array_len()`, `step_array_read_bytes()` (inline), `step_array_open()` (by-hash, returns `store_handle_t`). Step authors write ordinary for-loops. **The runner does not fan out** — a step with an array read is still a single task. Steps that want parallel per-element work use libcurl multi-handle (I/O-bound) or borrow a thread pool (CPU-bound).

**Cache behaviour.** The array artifact's content hash is the hash of its serialized bytes (manifest bytes for by-hash, item bytes for inline). A step reading an array is identity-hashed with that array's hash in its read set — same array → same identity → cache hit. Elements in a by-hash array are independently cached; changing one element changes the manifest hash (the consuming step re-runs) but the unchanged elements' store entries stay live.

**Writing.** Steps emit arrays explicitly: `step_write_array_inline(name, items, n, stride)` or `step_write_array_by_hash(name, hashes, n)`. No runner-side assembly phase.

**What this does NOT add.** No cross-product (step with two array reads → N × M). No nested arrays. These can be added strictly on top if ever needed.

---

## 6. Components

Physical layout of the system.

```
┌─────────────────────────────────────────────────────────────┐
│                          BROWSERS                           │
│  ┌───────────────────┐        ┌─────────────────────────┐   │
│  │ Lab UI            │        │ Viewer SDK              │   │
│  │ - Three.js        │        │ - Three.js              │   │
│  │ - SVG inspectors  │        │ - Controls panel        │   │
│  │ - Artifact viewer │        │ - Compass gizmo         │   │
│  │ - Timeline, DAG   │        │ - Model upload          │   │
│  │ static files      │        │ static JS bundle        │   │
│  └─────────┬─────────┘        └──────────┬──────────────┘   │
└────────────┼────────────────────────────┼───────────────────┘
             │ WS + HTTP (static, catalog) │ WS + HTTPS
             ▼                             ▼
┌─────────────────────────────────────────────────────────────┐
│                    dioramap-processor                       │
│                 (single C binary, stripped)                 │
│                                                             │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Main event-loop thread (libwebsockets)             │    │
│  │  - accepts WS sessions                             │    │
│  │  - serves static files                             │    │
│  │  - parses RUN / SUBSCRIBE / RECONNECT              │    │
│  │  - emits ARTIFACT frames                           │    │
│  └───────────────┬────────────────────────────────────┘    │
│                  │                                          │
│                  │ auth (for viewer only)                   │
│                  ▼                                          │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Control-plane client (libcurl + mbedTLS)           │    │
│  │  - verifies API keys (cached 60s)                  │    │
│  │  - batches usage events                            │    │
│  └────────────────────────────────────────────────────┘    │
│                                                             │
│                      tasks + refs                           │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Runner + Artifact Store                            │    │
│  │  - content-addressed hashmap                       │    │
│  │  - hot/warm/cold tiers                             │    │
│  │  - session refcounts                               │    │
│  │  - DAG walker for demand-driven execution          │    │
│  │  - dispatches step tasks to thread pool            │    │
│  └───────────────┬────────────────────────────────────┘    │
│                  │                                          │
│                  ▼                                          │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Compute thread pool (N ≈ cores)                    │    │
│  │  - executes step DAGs                              │    │
│  │  - CPU steps run in-thread                         │    │
│  │  - [CUDA steps: launch to own stream, sync]        │    │
│  │  - outbound fetch (IGN tiles) via libcurl          │    │
│  │  - writes results to store                         │    │
│  └────────────────────────────────────────────────────┘    │
│                                                             │
│                   heap artifacts + GPU (opt)                │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ LRU demotion / promotion
                          ▼
                  /var/cache/dioramap/<hash>.bin
                  (disk cache, mmap'd on read)
```

### 6.1 Binary modes

The same ELF supports multiple entry modes:

```
dioramap-processor serve   [--config cfg.toml]     # default: WS + HTTP server
dioramap-processor batch   [--pipeline <id>] [--input …] [--produce …]
                           [--output-dir <path>]   # one-shot, no WS
dioramap-processor cli     [ad-hoc subcommands: cache-stats, artifact-dump, etc.]
```

`batch` and `serve` share every piece of the processing stack: same runner, same store, same step registry. They differ only in the *delivery channel* (file sink vs WS).

### 6.2 External processes

- **Dioramap control plane** — separate Node SaaS backend at `dioramap.com/internal/*`. Handles API key verification, billing, usage aggregation. Not on customer boxes. Out of scope for this doc.
- **Build toolchain (Vite)** — builds browser JS bundles for lab UI and viewer SDK. Dev-machine / CI only.

---

## 7. Threading & execution model

### 7.1 Threads

One persistent C process, multiple threads:

| Thread | Count | Role |
|---|---|---|
| Service (libwebsockets) | 1 | WS I/O, protocol parsing, artifact frame emission, RECONNECT swap. Drives `lws_service()`. |
| Run (per active RUN/SUBSCRIBE) | 0..N_sessions | Detached thread that calls `runner_run()` for one session at a time. |
| Compute pool | N ≈ CPU cores | Step execution — shared across all sessions, owned by the runner. |
| Fetch | 2–4 | Outbound libcurl for IGN tile fetch (I/O-bound, separate pool). *(Not yet implemented — A9.)* |
| Store janitor | 1 | Background LRU eviction + disk demotion under `store`. |
| Session janitor | 1 | Sweeps grace-expired disconnected sessions (default grace 600s). |

### 7.2 Request flow (serve mode)

1. Client opens WS.
2. If viewer tier: main thread calls control-plane client to verify the key (cached 60s).
3. Client sends `RUN { pipeline_id, input, subscribe: [...] }`.
4. Main thread creates a Session object, enqueues a "produce these artifacts" task to Runner.
5. A compute thread picks up the task, walks the step DAG backward from subscribed artifacts, executes each uncached step in topological order.
6. As each step produces a write, it's inserted into the Artifact Store (content-hashed). Store notifies Main thread, which emits a WS `ARTIFACT` frame to each subscribed session.
7. When runner is done, it sends `RUN_COMPLETE`. Session retains refs to subscribed artifacts.
8. Client may `SUBSCRIBE` more artifacts (late subscribe). If already in store, served immediately; else a new compute task is scheduled, executing only the tail of the DAG not yet computed.
9. On disconnect, session enters grace period. On `RECONNECT {token}`, session is rehydrated; artifacts still referenced.

### 7.3 Request flow (batch mode)

1. Binary invoked with `batch --pipeline X --input Y --produce A,B,C --output-dir D/`.
2. Main-thread-equivalent starts up, no WS listener.
3. Same Runner + Store.
4. DAG walk, compute, produce.
5. Each produced artifact written to `D/<name>.bin` (binary layout) or `D/<name>.json` (metadata-only kinds). Also: `D/manifest.json` listing everything produced with content hashes.
6. Process exits.

No TLS, no auth, no control plane. Batch mode is for local use, CI, testing, support repros.

### 7.4 Placement transfer table

When a step needs a read artifact placed where its `compute_location` can access it, and the bytes are currently elsewhere, the runner consults this table. Transfers are synchronous for CPU steps; GPU steps use async copies on the compute thread's stream.

| from \ to | heap | device | disk |
|---|---|---|---|
| heap | — | `cudaMemcpyH2D` (from pinned buffer) | `write()` to cache file |
| device | `cudaMemcpyD2H` | `cudaMemcpyD2D` | via heap |
| disk | `mmap(PROT_READ)` | via heap | — (already there) |

"Wire" is not a placement — it's the event of serializing an artifact into a WebSocket binary frame. The bytes being sent live in heap or are read from disk (via `mmap`) at send time.

Enforce "no direct device ↔ disk hop" — always route via heap. Keeps the transfer code small and predictable. `disk → heap → device` is the GPU's inbound path; `device → heap → disk` the outbound. For CUDA builds, heap buffers used on the H2D/D2H fast path are pinned (`cudaHostAlloc`).

### 7.5 CUDA integration (when enabled)

- Compile-time: `-DDIORAMAP_CUDA=ON` produces `dioramap-processor-cuda` binary.
- CUDA context initialized once at process start; persists for lifetime.
- Each compute thread owns its own CUDA stream; concurrent pipelines overlap on the GPU.
- Device buffer pool for artifact allocations (via `cuMemPoolCreate`); reuse across requests.
- Heap-side artifacts that may transfer to GPU are allocated via `cudaMallocHost` (pinned) for fast H2D.
- No runtime CPU-fallback: CPU-only and CUDA-enabled are separate builds. Simpler than per-step dual impl.
- Kernel fusion is allowed: a single native function may fulfill multiple step names if it writes all their declared outputs.

### 7.6 CUDA-readiness canary: the `gpu-smoke` step

To keep CUDA-readiness a **build-time invariant** rather than an aspirational design claim, the repo ships a no-op canary step from Phase 0 onward, whether or not any real algorithm is on the GPU yet.

```
core/steps/gpu-smoke/
├── meta.json
├── native/
│   ├── gpu-smoke.c       # CPU impl (always compiled)
│   └── gpu-smoke.cu      # CUDA impl (compiled only if -DDIORAMAP_CUDA=ON)
└── golden/
    └── identity.json     # fixed input, expected byte-identical output
```

**What the step does.** Takes a small Float32 column (N values), writes the same column back. Mathematically the identity function.

**What it exercises.** The CPU impl is trivial (memcpy). The CUDA impl exercises:

1. `cudaMallocHost` / pinned-memory allocation path for the input artifact
2. `cudaMemcpyAsync` H2D onto the compute thread's stream
3. A trivial kernel launch (e.g. `__global__ void identity(const float* in, float* out, int n) { int i = blockIdx.x*blockDim.x + threadIdx.x; if (i < n) out[i] = in[i]; }`)
4. `cudaMemcpyAsync` D2H
5. `cudaStreamSynchronize`
6. Residency transition `heap → gpu-device → heap` via the §7.4 transfer table
7. Artifact binary layout (§5) survives the round trip byte-for-byte

**What this forces.** Existence of the step means the build system, CMake CUDA detection, linking, kernel compilation, residency-aware dispatcher, and the pinned-host-allocator path all have to be wired up from Phase 0. You can't defer them to a hypothetical Phase 9 because the smoke step won't build without them. If CUDA-readiness ever regresses, CI catches it immediately.

**How it runs.**

- In CPU-only builds, `gpu-smoke` is a registered step with the CPU impl only. Golden test passes trivially.
- In CUDA-enabled builds, both impls compile; CUDA impl is selected by `preferred_residency: "gpu-device"`. Golden test passes if and only if the whole CUDA path works end-to-end.
- Optional startup self-test: CUDA builds may invoke `gpu-smoke` once at process init; failure to complete aborts startup (no silent degradation, consistent with the "no runtime fallback" decision).

**Phase 9 becomes "replace the canary pattern with real algorithms."** Adding CUDA implementations to `compute-normals`, `shadow-cast`, Sobel, rasterization, etc. follows the exact shape of `gpu-smoke`: second `.cu` file alongside the `.c`, preferred_residency flipped to `gpu-device`, golden test validates parity with the CPU reference. No new infrastructure, because `gpu-smoke` already proved the infrastructure exists.

**Why this is cheap to maintain.** The step is ~40 LOC total (CPU + CUDA + kernel + golden test). It runs in under a millisecond. It never changes unless the core GPU contract changes — in which case it's the first place you'd want a test breakage.

### 7.7 Error propagation

- Step failure → `ERROR` message to all sessions with a subscription blocked by that step. Run terminates.
- Native crash (segfault) in serve mode is a bug; process restarts under systemd. Session tokens persist (tied to disk cache survival). Clients `RECONNECT` and resubscribe; cached artifacts remain accessible.
- Resource limits: hard cap on heap budget, disk budget, concurrent runs. Exceeded → `ERROR` with retryable code.

---

## 8. Protocols

### 8.1 WebSocket protocol

Binary WS frames carry artifact bytes; text WS frames carry JSON control messages. One connection per session.

**Client → server (text/JSON):**

```jsonc
// Bare form lists pipeline names. With a `pipeline` field, lists the
// pipeline's writes (name + producer + visibility).
{ "t": "CATALOG" }
{ "t": "CATALOG", "pipeline": "demo-chain" }

// Start a pipeline run.
{ "t": "RUN",
  "pipeline": "demo-chain",
  "input": { "base": 5 },
  "subscribe": ["report"] }

// Late-subscribe to an additional artifact from the same pipeline + input.
// Requires at least one prior RUN on this session. The session replays the
// pipeline with subscribe=[artifact]; cached steps are skipped.
{ "t": "SUBSCRIBE", "artifact": "scaled" }

// Cancel the active run on this session.
{ "t": "CANCEL" }

// Reattach to a disconnected session within its grace window.
{ "t": "RECONNECT", "session_token": "s_abc123" }
```

**Server → client (text/JSON):**

```jsonc
{ "t": "SESSION_STARTED", "session_token": "s_abc123", "tier": "debug" }

// Reply to a RECONNECT. Only sent when the token was preserved within grace.
{ "t": "RECONNECTED",     "session_token": "s_abc123", "tier": "debug" }

// Reply to CATALOG (bare form: pipeline list). Reply to CATALOG with a
// "pipeline" arg: same envelope with an "artifacts" array of
// {name, kind, producer, visibility}.
{ "t": "CATALOG_RESPONSE", "pipelines": ["demo-chain", "gpu-smoke"] }
{ "t": "CATALOG_RESPONSE", "pipeline": "demo-chain",
  "artifacts": [{ "name": "report", "kind": 0, "producer": "report",
                  "visibility": "debug" }] }

{ "t": "RUN_STARTED",  "run_id": "r_1", "pipeline": "demo-chain",
  "step_plan": ["scale-by-factor", "add-bias", "multiply", "report"] }

{ "t": "STEP_STARTED",  "run_id": "r_1", "step": "scale-by-factor" }

{ "t": "ARTIFACT_HEADER", "run_id": "r_1", "artifact": "report",
  "size": 124, "hash": "<64-hex>" }
// ... followed by one binary WS frame carrying the artifact's bytes

// Streaming artifact: one or more STREAM_CHUNK messages precede a trailing
// ARTIFACT_HEADER. seq is zero-based; final:true marks the last chunk and
// carries the finalized artifact hash. After the final chunk, the server
// ALSO emits a standard ARTIFACT_HEADER + binary carrying the full bytes
// — this "dual delivery" means clients that don't speak STREAM_CHUNK (late
// subscribers, batch-mode callbacks, simple consumers) still get the
// artifact via the legacy path with no special-casing. Streaming-aware
// clients consume the STREAM_CHUNK pairs for low-latency rendering and
// ignore the trailing ARTIFACT_HEADER (they already have the bytes). See
// PLAN-VIEWER-PORT §7.3 for the rationale and cost analysis.
{ "t": "STREAM_CHUNK", "run_id": "r_1", "artifact": "lidar-positions",
  "seq": 3, "final": false, "size": 65536, "hash": null }
// ... followed by one binary WS frame of `size` bytes
{ "t": "STREAM_CHUNK", "run_id": "r_1", "artifact": "lidar-positions",
  "seq": 7, "final": true, "size": 40960, "hash": "<64-hex>" }
// ... followed by the final binary chunk of `size` bytes, then:
{ "t": "ARTIFACT_HEADER", "run_id": "r_1", "artifact": "lidar-positions",
  "size": 2435664, "hash": "<64-hex>" }
// ... followed by one binary WS frame of the full artifact bytes.

{ "t": "STEP_COMPLETE", "run_id": "r_1", "step": "report",
  "cache_hit": false, "duration_us": 812 }

{ "t": "RUN_COMPLETE",  "run_id": "r_1",
  "summary": { "duration_us": 3210, "steps_executed": 4, "cache_hits": 0 } }

// Terminator for late SUBSCRIBE flows (in place of RUN_STARTED/RUN_COMPLETE —
// late subscribe reuses the session's existing run_id).
{ "t": "SUBSCRIBE_ACK",  "artifact": "scaled", "duration_us": 120,
  "steps_executed": 0, "cache_hits": 1 }

{ "t": "ERROR", "run_id": "r_1", "step": "ortho-fetch",
  "code": "IGN_TIMEOUT", "msg": "..." }
```

**Not yet emitted (documented for future land):**

- `STREAM_CHUNK` — ✅ landed in B9 (runner `step_write_output_stream` + in-process accumulator + WS plumbing). First consumer: `laz-extract-disc` streams `lidar-positions` in 20k-point chunks. See PLAN-RUNNER T14 and PLAN-VIEWER-PORT §7.
- `PROGRESS` (in-step percentage updates) — reserved for long-running algorithmic steps; lands alongside the first step that actually wants it.
- `SUBSCRIBE_DENIED { artifact, reason: "visibility" }` — depends on tier enforcement, which depends on A10 (control-plane / auth).

**Binary frames:**

Each binary frame carries one artifact's bytes in the canonical layout (§5). Frame is preceded by an `ARTIFACT_HEADER` control message so the client knows which artifact it's about to receive. For a large artifact, libwebsockets' fragmented-message support is used transparently.

**TLS is out of scope for `dioramap-processor`.** Inbound traffic terminates
at an nginx reverse proxy in front of the binary; the proxy handles the
certificate, HTTP/2, and WSS upgrade. The processor itself speaks plain
WS/HTTP on a loopback or private-network port, which also lets us keep
libwebsockets built without OpenSSL/mbedTLS and keeps the binary smaller.
Outbound TLS (control plane, IGN tile fetch) is a separate concern handled
by the fetchers subsystem (§7.1, A9) via libcurl + its own TLS stack.

### 8.2 HTTP endpoints

Served by libwebsockets on the same port:

```
GET  /                          → lab UI or viewer SDK HTML (role-detected)
GET  /dist/*                    → static JS/CSS bundles
GET  /api/catalog               → same as WS CATALOG, for polling clients
GET  /api/schema/<kind>         → kind schema JSON (column specs, units)
GET  /healthz                   → 200 OK, for k8s / monitoring
```

Static file serving is dev-focused and debug. Production viewer SDK is typically hosted on a CDN; the binary serves a minimal index for localhost / self-hosted deployments.

### 8.3 Batch-mode CLI

```bash
dioramap-processor batch \
  --pipeline viewer/full \
  --input-json '{"kind":"lat-lon-radius","lat":48.8,"lon":2.3,"radius":100}' \
  --produce buildings-lod2,color-raster \
  --output-dir ./out/ \
  [--include-debug]           # overrides visibility filter (debug build only)
  [--params key=value ...]

# or stdin (JSONL, one run per line)
cat runs.jsonl | dioramap-processor batch --output-dir ./out/
```

Output layout:

```
out/
  manifest.json                  # { runs: [...], artifacts: [{name, kind, path, hash}] }
  <run-id>/
    buildings-lod2.bin           # binary layout
    buildings-lod2.json          # metadata sidecar (kind, size, column names)
    color-raster.bin
    color-raster.json
```

### 8.4 Visibility enforcement

Per-connection tier is established at connect time:

- Lab UI: localhost or dev auth → tier `debug`
- Viewer SDK: API key verified with control plane → tier `public`
- Batch mode (CLI): tier `debug` by default; `--include-debug` is redundant but documented

The catalog is filtered server-side before the response is built; a `debug` artifact is invisible to a `public`-tier client — they don't even know it exists.

`SUBSCRIBE` for an artifact not visible to the tier → `SUBSCRIBE_DENIED { reason: "visibility" }`. The artifact name is echoed (client already asked for it).

---

## 9. Artifact store lifecycle

### 9.1 Content-addressed model — summary

Every artifact in the store is keyed by `hash(bytes)`. Cache-skipping on step reruns is handled by a separate persistent structure, the **intent map**, which maps `step_identity → content_hash`. A single primitive `cached_compute(identity, fn)` orchestrates the whole flow: lookup in intent map, run `fn` on miss, content-hash the output, store, record.

**Authoritative spec**: see [`HASH-MODEL.md`](HASH-MODEL.md). That doc owns the full model (step identity formula, intent map behaviour, cascade recovery, etc.). This section lists only the architectural properties that matter at the system-design level.

Key properties:

- **Step identity** = `hash(step_name, step_version, canonical_params, sorted_read_hashes)`. Pure function of declared inputs; no hidden state.
- **Cache hit** = `intent_map.get(step_identity)` returns a content hash; the step is skipped.
- **Convergence** = different identities that produce the same output bytes share the store entry automatically (content-hashed).
- **Cross-pipeline sharing** = any step's output referred to by the same canonical identity chain is shared, regardless of which pipeline triggered it.
- **Determinism required** = step outputs must be pure functions of their declared inputs. The `skip_on_identity: false` flag exists as an escape hatch for non-deterministic steps but is almost never used.

The previously-documented `output_hash: DERIVATION | CONTENT` dual model (superseded 2026-04-21) conflated two concerns — whether the runner could skip execution, and whether downstream could share cache across chains. The new model separates them: all writes are content-hashed (convergence is automatic), and the intent map handles skip. See `HASH-MODEL.md` §15 (Appendix A) for the chronology.

### 9.2 Pipeline input as a root artifact

The RUN payload becomes an artifact at the head of the DAG, written by the runner itself:

```
on RUN { pipeline, input, subscribe } received:
  bytes = canonicalize_json(input)                     # stable serialization
  hash  = hash_namespaced("pipeline-input-root-v1", bytes)   # xxh3-128 under the hood, see HASH-MODEL.md §10
  store.write(hash, kind=opaque-json, bytes)
  → artifact "pipeline-input" is now in the store with that hash
```

Input-source and normalization steps then have `reads: ["pipeline-input"]` and consume it like any other artifact. This unifies the model: everything flowing through the DAG is an artifact, including the user's request.

Two RUNs with byte-identical `input` produce the same `pipeline-input` hash → downstream steps cache-hit. Different `input` bytes → different root hash → downstream steps miss, unless a content-hashed normalization step converges them later.

### 9.3 Funnel convergence — automatic under the unified model

*(This subsection previously described the `output_hash: "content"` opt-in mechanism, retired 2026-04-21 when the model was unified under content-hash-everywhere. The behaviour it provided — different pipeline-input bytes canonicalizing to the same 20m grid cell sharing downstream cache — still works, but it's now the default, not an opt-in.)*

Under the unified model:

- Every step's output is content-hashed. Two different upstream chains that produce the same output bytes automatically land on the same store key. No `output_hash` field in `step_def_t`.
- Cache-skipping on exact-identity reruns is handled by the intent map, not by pre-computable derivation keys. A step whose identity has been seen before skips execution via intent-map lookup.
- "Funnel points" (canonicalize-location, find-copc-tiles, fetch-copc-tiles, laz-extract-disc) and non-funnel steps (plain fetchers, pure transforms) use the same machinery. The step author doesn't choose a hash mode.

**20m-rounding example**, restated under the unified model:

```
RUN A: input = {lat: 48.800, lon: 2.350}
RUN B: input = {lat: 48.801, lon: 2.351}    (same 20m cell)

pipeline-input hashes:   H_A ≠ H_B
step canonicalize-location:
  A:  identity_A = hash("canonicalize-location", 1, {}, [H_A])
      intent_map miss → execute → bytes are deterministic for this cell
      content_hash = hash(canonical_bytes) = H_canon
      store_put(H_canon, canonical_bytes)
      intent_map[identity_A] = H_canon
  B:  identity_B different from identity_A
      intent_map miss → execute → SAME bytes → SAME content_hash H_canon
      store_put idempotent (bytes already present)
      intent_map[identity_B] = H_canon   ← new intent entry, same target

downstream fetch-ortho-rgb, fetch-elevation, etc.:
  Read "canonical-location" → H_canon in both A and B.
  Their identity = hash(step_name, version, params, [H_canon]) — SAME in A and B.
  → intent_map hit → skip entirely.
```

Both chains converge at `canonical-location`. Downstream is single-execution. No opt-in required.

See `HASH-MODEL.md` §7 for full worked examples, §11.2 for the manifest eviction cascade policy.

### 9.4 Two tiers, zero-copy inter-step transfer

*(Updated 2026-04-21. The earlier "disk is canonical, heap is only transient" model was revised during the hash-model refactor to introduce an explicit hot tier for zero-copy between steps in the same pipeline.)*

The store has two tiers:

- **Hot** (heap-resident): the bytes produced by a step live in a heap buffer whose ownership was transferred to the store. Reads return a direct pointer via a ref-counted handle. **Zero copies** between the producing step and any consuming step in the same pipeline.
- **Warm** (disk-resident at `<root>/<hex[0:2]>/<hex[2:4]>/<full-hex>.bin`): bytes persisted via atomic temp+rename. Reads via `mmap`, zero-copy from the OS page cache.

An entry lives in exactly one tier at a time. Demotion (hot → warm) happens under memory pressure; the entry's store key doesn't change. Eviction (warm → deleted) is the disk-LRU janitor.

**Lifecycle of a produced artifact:**

```
Cold ──produce (malloc + fill)──► Hot tier (heap, ownership in store)
                                    │
                                    │ downstream step_lookup:
                                    │   direct pointer (zero copy)
                                    │
                                    │ hot budget pressure or shutdown:
                                    ▼
                                  Warm tier (disk, mmapped on demand)
                                    │
                                    │ downstream step_lookup:
                                    │   mmap pointer (zero copy from page cache)
                                    │
                                    │ disk budget pressure (janitor):
                                    ▼
                                  Deleted (live mmaps survive via kernel refs)
```

**Ownership semantics.** `store_put(key, bytes, len)` takes ownership of the heap buffer — the store calls `free()` later. Callers that need to preserve their buffer do `malloc + memcpy` at the call site before the put. Uniform one-function API; no `_owned` / `_copy` split.

**No hot-tier budget means no hot tier.** `hot_budget = 0` in config degrades cleanly to disk-only. All puts go straight to warm; all reads mmap. Same semantics, higher per-step latency due to the kernel round-trip.

See `HASH-MODEL.md` §5 for the full specification and `PLAN-STORE.md` S12–S15 for the implementation plan.

### 9.5 Cache budgets — hot tier + warm tier

Two budgets, each configurable per deployment:

- **Hot budget** (default 2 GB). Caps heap-resident bytes. Synchronous demotion on `store_put` under hard-cap pressure; janitor maintains a soft cap (80% default) to leave headroom for bursts. Entries larger than the hot budget skip hot entirely.
- **Warm budget** (default 50 GB dev, TB-scale prod). Caps disk use. Disk janitor evicts LRU warm entries with `ref_count == 0` and `pin_count == 0` until within budget.

**Eviction policy** (both tiers): LRU by last-access time, skipping entries with any live handle or session ref. No TTL beyond LRU.

**Startup**: scan `<root>/xx/yy/*.bin`, populate warm-tier index (hot tier starts empty — first `store_put` after startup repopulates). Malformed files unlinked. Orphaned `.tmp` unlinked. Intent-map log scanned to rebuild in-memory hashmap.

**On-disk path layout** is content-hash prefix-sharded (`xx/yy/<full-hex>.bin`) to keep any directory under a few thousand entries.

**Intent map** (separate file, `<root>/intent.log`): append-only log of 65-byte records. Independent of the store's CAS layout. See `HASH-MODEL.md` §6 for details.

### 9.6 Session refs as eviction guards

A session ref is a pair `(session_token, artifact_hash)`. It **pins** the disk entry against LRU eviction. Nothing else.

- A session holds one ref per artifact it has subscribed (explicitly or transitively via current RUN).
- On disconnect: refs stay for the grace period (default 10 min).
- On RECONNECT: refs continue.
- On grace expiry (§4.7 step 7): session destroyed, refs released. If no other session refs the same artifact, it's eligible for LRU eviction next time the janitor runs and budget is exceeded.

**Session refs don't touch the hot tier.** Session pins apply to disk (warm) eviction only. The hot tier's residency is governed by its own LRU + per-handle ref-counts (§9.4), not by session state. Heap usage remains a function of in-flight compute + hot-tier budget, not of session count.

### 9.7 Crash recovery

- Process crash mid-run: in-flight heap is lost. Disk entries written before the crash are still there (content-addressed, immutable). On restart, the index rebuild picks them up. Clients reconnect and re-run; cached ancestors are disk-hit; only unfinished tail recomputes.
- Partial file (write interrupted by crash): detected on header parse at startup, deleted. Because files are written atomically (write-then-rename from a temp file), this should be rare.

### 9.8 Audit of existing cache systems (to inform migration)

The current viewer has an L1/L2 cache (`backend/cache/lru-cache.js`, `backend/cache/cached-fetch.js`). Bench has a step-chain cache (`lib/pipeline/cache.js`). Both are subsumed by the single store design above:

- **L1 (raw IGN tiles)**: becomes the output of `fetch-*-tile` steps. Content-hash by the request params (bbox, resolution, date). The cached raw-tile artifact is reused directly by all downstream steps via the store.
- **L2 (scene snapshot)**: replaced by per-artifact caching. Hit rate changes from "whole scene identical" to "any subset identical" — generally better, occasionally worse if the whole scene was cached and now individual artifacts miss separately.
- **Bench step-chain cache**: drops out; the content-hash model captures step chain + versions + inputs already.

**On COPC partial fetching.** The current viewer walks COPC indices and fetches sub-ranges for spatial queries inside a COPC tile. This is efficient for bandwidth but complex (multi-range HTTP, COPC octree walk) and the cache granularity is awkward. **Recommendation for the target**: fetch the whole COPC tile as the artifact (one content hash, one cache entry), parse in-process when needed. Tile sizes are bounded; bandwidth is amortized by caching; the complexity payoff of partial fetches is low once we're not re-fetching. Revisit if prod data shows us fetching tiles too large to justify whole-fetch.

---

## 10. Use-case walkthroughs

### 10.1 Viewer: new customer request

1. SDK in browser opens `wss://viewer.dioramap.com/` with `Authorization: Bearer <pub-key>`.
2. Main thread: libwebsockets accepts. Control-plane client verifies key (cached → instant if recent). Session opened with tier `public`.
3. Server sends `SESSION_STARTED`, `CATALOG_RESPONSE` (public artifacts only).
4. SDK sends `RUN { pipeline: "viewer/full", input: {lat: 48.8, lon: 2.3, radius: 100}, subscribe: ["buildings-lod2", "color-raster", "shadows-map"] }`.
5. Runner walks DAG: `tile-fetch` → `extract-denoise` → `compute-normals` → `segment` → `boundary-edges` → `boundary-3d` → `assemble-buildings-lod2`; and in parallel `ortho-fetch` → `deshadow` → `shadow-cast` → `assemble-color-raster` → `assemble-shadows-map`.
6. Some steps hit cache (same lat/lon was processed recently). The ones that don't get dispatched to compute threads.
7. As each subscribed artifact is written to the store, main thread sends `ARTIFACT_HEADER` + binary frame to this session.
8. SDK decodes binary frames using the catalog schema, builds Three.js objects, renders progressively.
9. `RUN_COMPLETE`. Usage event (compute_ms, bytes_out) batched to control plane.

### 10.2 Viewer: repeat request, warm cache

1. Same request, 10 min later from a different customer.
2. All artifact hashes match a recent run. Every subscribed artifact is in hot or warm tier.
3. Runner skips compute entirely; artifacts delivered from store within milliseconds.
4. `RUN_COMPLETE` with `steps_executed: 0, cache_hits: N`.

### 10.3 Viewer: page reload

1. User reloads. New WS connection. SDK sends `RECONNECT { session_token }`.
2. Main thread finds session (within grace period). Rehydrates.
3. Sends `CATALOG_RESPONSE` with current tier status of session's refs.
4. SDK re-subscribes to the same artifacts; hot tier hits → instant delivery.

### 10.4 Lab UI: algorithm iteration

1. Developer opens `http://localhost:8080/lab`. Served by binary, tier `debug`.
2. Selects pipeline `bench/v2-champion` and test case `real-042`.
3. Clicks "Run" → SDK sends `RUN` with broad subscription (all public + debug artifacts for the pipeline).
4. Runner executes every step (demand-driven, but dev asked for everything).
5. Artifact panel populates incrementally as each step completes; user sees point cloud appear, then segmentation, then boundaries.
6. User clicks on `boundary-signal-2d` for plane 3 in the artifact list → SDK sends `SUBSCRIBE { artifact: "boundary-signal-2d-plane-3" }`. Already in store (subscription was implicit), delivered instantly.
7. User tweaks a `params` field and re-runs. Step version unchanged, but params hash differs → cache miss, re-execution. Bench DB records run with all step_versions and metric_versions for traceability.

### 10.5 Late subscription, 20 minutes later

1. Continuation of 10.4. Developer stares at the 3D view, then clicks a "show PCA eigenvalue heatmap" control.
2. SDK sends `SUBSCRIBE { run_id: "r7", artifact: "pca-eigenvalue-heatmap" }`.
3. Session still alive (grace-protected, user on page). Artifact was not in the initial subscription → not computed.
4. Runner schedules a compute task: walk DAG from `pca-eigenvalue-heatmap` → find that its inputs (`points`, `normals`, `eigenvalues`) are all in store from the earlier run.
5. Execute only `pca-eigenvalue-heatmap-generate` step. Fast — inputs are warm.
6. `ARTIFACT_HEADER` + binary frame delivered.

### 10.6 Batch CI regression test

```bash
for case in test-cases-v2/real-*; do
  dioramap-processor batch \
    --pipeline bench/v2-champion \
    --input-laz "$case/input.laz" \
    --produce buildings-lod2,planes-final \
    --output-dir "results/$case/" \
    --params gt_path="$case/gt.json"
done
```

- No WS, no auth.
- Each run produces output files; test harness compares against golden fixtures.
- Metric versions recorded in `manifest.json` for traceability.

### 10.7 Customer support repro

Customer reports wrong building count. Support engineer:

```bash
dioramap-processor batch \
  --pipeline viewer/full \
  --input-json "$(cat customer-report-input.json)" \
  --produce buildings-lod2,plane-segmentation-iters \
  --output-dir /tmp/repro/ \
  --include-debug
```

Re-run customer's request with debug artifacts enabled. Inspect intermediate results to diagnose.

---

## 11. Repo layout (target)

**Core principle: the existing projects stay put and keep evolving. Everything new lives under a sibling `v3/` directory, built from scratch.** This avoids any risk of destabilizing current bench or viewer workflows during the long migration, and it preserves the existing code as a permanent reference for legacy algorithms we choose not to port.

```
lidar-algos/                                     (monorepo root)
│
├── lidar-algos-bench-v2/                        ← UNCHANGED. Keeps evolving independently.
│                                                  Legacy algos stay here as reference; we will not
│                                                  port unmaintained algos, only cite them.
│
├── lidar-3d-environment-viewer/                 ← UNCHANGED. Keeps evolving independently until
│                                                  the new viewer SDK (under v3/apps/) replaces it.
│
├── control-plane/                               ← Out of scope for this doc. Expected to move to
│                                                  its own dedicated repo. SaaS backend; API keys,
│                                                  billing, usage aggregation.
│
├── v3/                                          ← NEW. All unified architecture work lives here.
│   │                                              Nothing in v3/ touches files outside v3/.
│   │
│   ├── core/                                    ← Execution machinery only.
│   │   │                                          No step code, no pipeline definitions.
│   │   ├── CMakeLists.txt
│   │   ├── src/
│   │   │   ├── main.c                           (batch / serve subcommand dispatch)
│   │   │   ├── artifact/                        (binary layout codec)
│   │   │   ├── hash/                            (content hashing: xxh3 now, blake3 later)
│   │   │   ├── log/                             (structured logging; see §17)
│   │   │   ├── mmap/                            (mmap helper for zero-copy reads)
│   │   │   ├── pipeline/                        (pipeline JSON loader + validation)
│   │   │   ├── runner/                          (DAG walk, step dispatch, cancellation)
│   │   │   ├── runtime/                         (thread pool, refcount primitives)
│   │   │   ├── server/                          (libwebsockets, sessions, WS protocol)
│   │   │   ├── step/                            (step registry)
│   │   │   └── store/                           (content-addressed disk cache + janitor)
│   │   │   # Future (not yet present): cuda/ for GPU runtime, fetch/ for libcurl.
│   │   ├── include/v3/                          (public headers consumed by steps)
│   │   └── tests/                               (unit + integration + WS-protocol tests)
│   │
│   ├── steps/                                   ← Step implementations.
│   │   │                                          One directory per step.
│   │   │                                          All step impls are native C; no native/ subfolder.
│   │   ├── gpu-smoke/                           (the CUDA-readiness canary; §7.6)
│   │   │   ├── meta.json
│   │   │   ├── gpu-smoke.c                      (CPU impl)
│   │   │   ├── gpu-smoke.cu                     (compiled only if -DDIORAMAP_CUDA=ON)
│   │   │   └── golden/identity.json
│   │   ├── fetch-lidar-tile/
│   │   │   ├── meta.json
│   │   │   └── fetch-lidar-tile.c
│   │   ├── compute-normals/
│   │   │   ├── meta.json
│   │   │   ├── compute-normals.c                (step entry)
│   │   │   ├── kd-tree.c
│   │   │   ├── pca.c
│   │   │   └── golden/
│   │   ├── boundary-edges/
│   │   │   ├── meta.json
│   │   │   ├── boundary-edges.c                 (step entry, dispatches to algo)
│   │   │   ├── algo-champion.c
│   │   │   ├── algo-contour-snap.c
│   │   │   ├── viz/                             (OPTIONAL — step-visualizers; lab-ui only)
│   │   │   │   ├── boundary-signal-2d.viz.js    (custom inspector for one artifact)
│   │   │   │   └── package.json                 (lab-ui dynamic-imports per VISUALIZERS.md §5)
│   │   │   └── golden/
│   │   └── …
│   │
│   ├── pipelines/                               ← Pipeline definitions (JSON, declarative).
│   │   ├── smoke/                               (tiny pipelines for infra validation)
│   │   │   └── three-step-smoke.json
│   │   ├── bench/                               (migrated bench pipelines)
│   │   │   └── v2-champion.json
│   │   └── viewer/                              (migrated viewer pipeline)
│   │       └── full.json
│   │
│   ├── apps/                                    ← Browser frontends + their shared packages.
│   │   ├── protocol/                            (@dioramap/protocol — WS client + artifact decoder)
│   │   │   └── src/                             (ws-client.js, artifact-decoder.js;
│   │   │                                         consumed by lab-ui + viewer-sdk via file: deps)
│   │   ├── scene-base/                          (@dioramap/scene-base — Three.js scene + composite-visualizers)
│   │   │   └── src/
│   │   │       ├── scene/                       (create-scene, sun, scene-state, render-context, EDL)
│   │   │       ├── utilities/                   (leaf-sprite, gltf-loader)
│   │   │       └── viz/                         (composite-visualizers — single + multi-artifact
│   │   │                                         + tree-emissive; see VISUALIZERS.md §1)
│   │   ├── lab-ui/                              (WS-based lab UI; consumes protocol + scene-base)
│   │   │   ├── src/                             (Three.js app, DAG inspector, timeline,
│   │   │   │                                     builtin-visualizers for kinds 0-6)
│   │   │   ├── viz-loader/                      (loads visualizers from three locations per VISUALIZERS.md §5:
│   │   │   │                                     - apps/lab-ui/src/renderers/builtin/*.js (builtin-visualizer, kind dispatch)
│   │   │   │                                     - v3/steps/<step>/viz/*.viz.js (step-visualizer, dynamic import)
│   │   │   │                                     - @dioramap/scene-base/viz/* (composite-visualizer, Vite glob))
│   │   │   ├── vite.config.js
│   │   │   └── fixtures/                        (test cases migrated as pipelines evolve)
│   │   ├── viewer-sdk/                          (production embed; consumes protocol + scene-base)
│   │   │   ├── src/                             (orchestrator + controls-panel DOM + GLB models)
│   │   │   ├── vite.config.sdk.js
│   │   │   └── CHANGELOG.md                     (SDK semver; see §4.10)
│   │   ├── location-picker/                     (@dioramap/location-picker — MapLibre picker)
│   │   └── seller-demo/                         (static HTML demo)
│   │
│   ├── tools/
│   │   ├── artifact-inspect/                    (open a .bin cache file, dump layout/metadata)
│   │   ├── cache-stats/                         (disk cache budget, hit rates, age)
│   │   └── bench-cli/                           (wraps batch mode for CI and scripts)
│   │
│   └── docs/                                    ← All v3 architecture + subsystem docs.
│       ├── UNIFIED-ARCHITECTURE.md              (this doc)
│       ├── plan/
│       │   ├── MAIN-IMPL-PLAN.md                (top-level implementation plan)
│       │   ├── PLAN-STORE.md
│       │   ├── PLAN-RUNNER.md
│       │   ├── PLAN-PROTOCOL.md
│       │   ├── PLAN-FETCHERS.md
│       │   ├── PLAN-CUDA.md
│       │   ├── PLAN-LOGGING.md
│       │   ├── PLAN-TESTING.md
│       │   ├── PLAN-VIEWER-PORT.md              (Phase B algorithm-port milestones B1–B10)
│       │   ├── PLAN-LAB-UI-PORT.md              (Phase C1 lab-ui milestones L1–L11)
│       │   └── PLAN-VIEWER-SDK-PORT.md          (Phase C2 viewer SDK + scene-base milestones)
│       └── STEP-CATALOG.md                      (every step, its meta, its status)
│
├── lidar-third-parties/                         ← fixtures, reference data. Unchanged.
├── registration-lab/                            ← separate experiment area. Unchanged.
└── docs/                                        ← root-level docs (launch plan, etc.)
```

**Layout principles:**

1. **Separation of concerns.** `core/` is pure execution machinery — runner, store, cache, transport, GPU runtime. It has no knowledge of specific algorithms or pipelines. `steps/` is algorithm implementations. `pipelines/` is declarative data. This separation means core can be tested and evolved independently of the step library.

2. **Zero changes to existing directories.** Legacy bench/viewer code stays where it is, continues to be maintained, continues to serve as reference for algorithms we may or may not port. The existence of a file in `lidar-algos-bench-v2/lib/pipeline/steps/foo.js` does NOT imply we'll port it — we port only what earns it.

3. **Three visualizer subtypes, two on-disk locations outside lab-ui.** Visualizer = the JS module that turns artifact bytes into user-visible output (`.viz.js`). Subtypes: **builtin-visualizer** (`apps/lab-ui/src/renderers/builtin/`, kind-dispatched fallback), **step-visualizer** (`v3/steps/<step>/viz/<artifact>.viz.js`, lab-ui dynamic-imports per-artifact), **composite-visualizer** (`apps/scene-base/src/viz/`, both lab-ui and SDK import — SDK via closed allow-list, lab-ui via Vite glob). Rule: if the SDK uses it → composite (scene-base); else step or builtin depending on whether it customises one specific artifact or covers a whole kind. **See [VISUALIZERS.md](VISUALIZERS.md) for the canonical contract, lifecycle, and UI mapping.**

4. **Control plane in a separate repo (planned).** Currently at `control-plane/` but expected to move to its own repository, decoupled from the processing core entirely. For this doc, it's out of scope.

---

## 12. Migration path

**Principle: build the machinery fully, prove it with dummy steps, then port algorithms. Existing code never moves — it stays where it is, keeps evolving, and is cited as a reference for any algorithm we choose to port.**

Three macro-phases: **foundations → algorithms → frontends**. No back-and-forth bridge, no dual-language steps, no per-step migration drama. At each phase boundary, the previous phase stands on its own.

### 12.1 Phase A: Foundations (the hard part)

Goal: a working `dioramap-processor` binary with every piece of the core in place. At the end of this phase, nothing real is being computed, but every machinery — threading, store, cache, WS, batch, CUDA — is shipped, tested, and validated end-to-end through a smoke pipeline.

**A1. Scaffolding.** `v3/` directory, CMake, CI setup, clang-format, test harness skeleton. Empty binary that just prints "dioramap-processor v0". No dependencies linked yet.

**A2. Logging (§17).** Before anything else that emits events. Event schema, subsystem routing, ring-buffered drain thread, stdout JSON-lines sink. Every subsequent subsystem codes against this API from day one — no retrofitting.

**A3. Runtime primitives.** Thread pool, work queue, state machines, mutex/condvar discipline, atomic counters, race-safe reference counting. Unit tests for concurrency invariants (e.g., TSan-clean under contention). Clear ownership model documented.

**A4. Artifact binary layout (§5).** Encoder + decoder + validation. Golden test files at well-known versions. mmap helper. Content-hash computation (xxh3-128 via `dioramap_hash`). Round-trip tests across the layout.

**A5. Store and cache subsystem (§9).** Content-addressed hashmap (hot heap tier + warm disk tier, see HASH-MODEL.md §5). Disk persistence with atomic write-then-rename, mmap on read, startup index rebuild, LRU janitor thread, session-ref pinning. Budget enforcement. Cache directory sharding. Persistent intent map (`step_identity → content_hash`) for cache-skip on rerun (PLAN-STORE S14, HASH-MODEL.md §6).

**A6. Runner subsystem.** Step registry, DAG walk backward from subscribed artifacts, topological step dispatch, placement transfer table (§7.4), step-slot allocation across compute threads. Pipeline-input root artifact injection (§9.2). Runner can be driven by a test harness that feeds it subscribe requests and inspects produced artifacts.

**A7. WebSocket + HTTP transport.** libwebsockets integration, session token issuance, message framing (text + binary), RECONNECT, SUBSCRIBE/CANCEL handling. Static file serving for a trivial lab UI landing page.

**A8. Batch mode.** `batch` subcommand with stdin/args input parsing, output-dir writer, manifest.json generation. Shares runner and store with serve mode.

**A9. Fetchers.** libcurl + mbedTLS, outbound HTTPS utilities. Not hooked to real endpoints yet.

**A10. Control-plane client skeleton.** Key verification with caching, usage event buffering. Pointed at a local stub during A phase.

**A11. CUDA runtime + `gpu-smoke` canary (§7.6).** `-DDIORAMAP_CUDA=ON`, context init, stream-per-compute-thread, pinned host allocator, device buffer pool. The `gpu-smoke` step is the canary; it validates the whole GPU path end-to-end. CI builds both CPU-only and CUDA variants.

**A12. Smoke pipeline.** A three-step pipeline that exists purely to exercise every piece of machinery:

```
smoke/three-step-smoke.json
├── fetch-dummy          → produces "raw-dummy" (reads: [input])
├── transform-dummy      → produces "transformed-dummy" (reads: [raw-dummy])
│                          runs on CPU, uses SoA columns
└── gpu-smoke            → produces "gpu-identity" (reads: [transformed-dummy])
                           runs on GPU, validates H2D/D2H round-trip
```

Assertion battery for the smoke pipeline:

- Fresh run produces all three artifacts, delivered over WS in order
- Repeat run hits cache on all three (no compute)
- Batch mode produces three files in output dir with correct headers + content
- Session disconnect + reconnect within grace period → cached artifacts re-served instantly
- Session destroyed after grace → LRU eventually evicts (under budget pressure)
- Late SUBSCRIBE (in Lab UI flow) to an unsubscribed artifact triggers single-step recompute
- CUDA build produces bit-identical `gpu-identity` to CPU build's reference
- Logs contain every expected event at appropriate levels

**The smoke pipeline stays forever as the architecture's integration test.** It runs in CI on every PR. If any machinery regresses, it breaks here.

Phase A definition-of-done: smoke pipeline passes all assertions in both CPU-only and CUDA builds. Runner, store, WS, batch, CUDA, logging all exercised.

### 12.2 Phase B: Algorithm ports

Goal: real pipelines and steps landed in v3, producing real artifacts from real inputs. Existing bench and viewer continue operating independently and unchanged throughout.

**Status: ✅ complete (B1–B10).** Canonical milestone list lives in [`PLAN-VIEWER-PORT.md`](plan/PLAN-VIEWER-PORT.md) §11 — do not duplicate it here. One-line summary: B1 (fetcher foundations) → B2 (coords + PROJ) → B3 (array artifacts + COPC) → **B3.5 hash-model refactor** ([`HASH-MODEL.md`](HASH-MODEL.md), commit `f359b50`) → B4 (compute-normals) → B5 (registration chain) → B6 (classification) → B7 (ground + parcels) → B8 (shadow + deshadow) → B9 (streaming retrofit) → B10 (surround fetchers + subscription gating).

Phase B definition-of-done achieved: `viewer-full.json` runs end-to-end against IGN producing the full artifact catalog. All IP-critical algorithms are native C in v3.

### 12.3 Phase C: New frontends

Goal: replace the existing lab UI and viewer SDK with new ones speaking the v3 WS protocol. Two new shared sibling packages — **`@dioramap/protocol`** (WS client + artifact decoder) and **`@dioramap/scene-base`** (Three.js scene + viz composers + SceneState surface) — are introduced so both consumers (lab UI and viewer SDK) draw from one source of truth. The viewer SDK becomes a paper-thin production extract of lab-ui via these packages. Detailed plans:

- [`PLAN-LAB-UI-PORT.md`](plan/PLAN-LAB-UI-PORT.md) — C1 lab-ui milestones L1–L11.
- [`PLAN-VIEWER-SDK-PORT.md`](plan/PLAN-VIEWER-SDK-PORT.md) — C2 viewer SDK + scene-base + protocol packages, S1–S7 + V2–V9 milestones.

**C1. New Lab UI.** Same feature set as current bench UI (pipeline selection, test case runner, artifact inspector, DAG view, SVG panels, Three.js 3D view) but speaking the v3 WS protocol. Visualizer dispatch per [VISUALIZERS.md](VISUALIZERS.md). **Status**: all L1–L11 shipped (L9-followup cancelled `ff25e3c`).

**C2. New Viewer SDK.** Matching v3 WS protocol. Semver major bump. Imports `@dioramap/scene-base` (closed allow-list of composers) and `@dioramap/protocol` rather than owning rendering or transport. Existing customer integrations migrate at their next integration window; existing viewer backend continues to serve old SDKs until cutover. When cutover happens (no customers to break yet per §15.3 N2): old viewer backend is decommissioned; new SDK is the only one in production. **Status**: S1–S5 + S7 + V2–V8 shipped; S6 (mega-composite) cancelled; V9 (production cutover) pending.

**C3. Location picker + seller demo.** Mostly as-is (they're thin static assets). Point them at the new SDK. Serve from v3's static file path. **Status**: location-picker shipped as `@dioramap/location-picker` (commit `f379e37`); seller demo shipped under A16 (commit `4602dc5`, see [`MAIN-IMPL-PLAN.md`](plan/MAIN-IMPL-PLAN.md) Phase A status table).

Phase C definition-of-done: real customers (when they appear) only see the v3 system. Old bench UI and viewer backend are decommissioned. `lidar-algos-bench-v2/` remains in the repo as a reference library for legacy algorithms.

### 12.4 What never happens

- No big-bang rewrite. Every phase leaves a working system in place.
- No dual-language steps. Every step is C.
- No moving existing files. `lidar-algos-bench-v2/` and `lidar-3d-environment-viewer/` retain their current structure indefinitely.
- No dual endpoints / version negotiation. Clean breaks at phase boundaries.

---

## 13. Deferred decisions / open questions

Flagged here so they're not lost. Each needs a decision before its blocking phase.

| # | Question | Needed by | Status |
|---|---|---|---|
| Q1 | Hash function: blake3 vs xxh3 vs sha256? | Phase A3 | **Resolved 2026-04-20.** xxh3 128-bit via libxxhash-dev. Non-cryptographic but content-addressing doesn't need crypto (no adversarial input). Faster than blake3 and available as a standard apt package. `hash_t` is 32 bytes wide so a blake3 swap later would keep the interface and on-disk layout unchanged. |
| Q2 | Disk cache path layout (sharded by prefix?) | Phase A4 | **Resolved.** Prefix-sharded per §9.3. |
| Q3 | Can the compute thread pool share CUDA streams across requests safely? | Phase A10 | Open. Current plan: one stream per compute thread, no sharing. |
| Q4 | Pinned memory allocator strategy (always-pinned vs pin-on-transfer) | Phase A10 | Open. Start with pin-on-transfer via `cudaHostAlloc` at heap-buffer allocation. |
| Q5 | Streaming step protocol semantics (multiple PARTIAL vs one STEP_COMPLETE)? | Phase A6 | **Resolved** in §15.3 N4 (seq + final flag). |
| Q6 | Session token format: opaque random vs signed claim (JWT-lite)? | Phase A6 | Open. Recommend opaque-random + server-side lookup for simplicity. |
| Q7 | Bench DB: port from SQLite to the new system, or keep Node-side? | — | **Resolved.** Not migrated. Scorer/metrics become steps producing subscribed artifacts in batch mode (§12 B4). Any run-history tooling is downstream of the binary, not inside it. |
| Q8 | Does the control plane stay Node, or eventually move to a simpler Go/Rust SaaS? | N/A | Out of scope. Separate repo (§11). |
| Q9 | Where do SVG visualization files (`*.svg.js`) live once their step is native? | Phase B | **Resolved.** Three visualizer subtypes — `v3/steps/<name>/viz/*.viz.js` (step-visualizer) for per-artifact custom views, `apps/scene-base/src/viz/` (composite-visualizer) for shared scene composition, lab-ui builtins for kind-dispatched fallbacks. See [VISUALIZERS.md](VISUALIZERS.md). |
| Q10 | Real-buildings importer (OBJ/STL) — is it a step or a CLI utility? | Phase B | **Resolved.** Not needed. The unified architecture + location picker provides the feature naturally (a "real buildings" input source step can read OBJ via a cached-fetch wrapper, exposed as a regular step). No special tooling. |
| Q11 | JSON library: yyjson vs jansson? | Phase A6 | **Resolved 2026-04-20.** jansson. yyjson isn't packaged in Ubuntu apt; jansson is, is well-maintained, and performance is not the bottleneck for pipeline/config/message JSON (small documents, parsed once). If JSON perf ever does matter on the hot path, swap to yyjson via the same `json_parse()` wrapper. |

---

## 14. Non-goals

Explicitly out of scope for this design. Reopen deliberately if context changes.

- **Distributed processing across multiple boxes.** One binary per box; load-balancers in front if scaling horizontally. No inter-node artifact sharing *implemented*. However, abstraction boundaries must leave the door open for future distribution — see §19.
- **Multi-language compute.** Everything native is C (or C++, for laz-perf). No Rust, no Python, no Julia.
- **Custom language runtimes.** No embedded Lua / Duktape / V8. The binary is C, period.
- **Database for artifacts.** Content-addressed filesystem is the cache. No Postgres, no Redis, no S3 for caching in the initial implementation. (The control plane is free to use whatever; that's outside this boundary.)
- **Live algorithm hot-reload.** Rebuild + restart. `ninja` is fast enough.
- **Feature flags / A/B testing in the binary.** Version the pipeline or the step, ship a new binary. No runtime flag registry.
- **Implicit / auto-wiring of steps.** Every step's `reads` and `writes` are explicit in `meta.json`. No conventions like "reads any artifact whose name contains 'points'" or "writes whatever the function returns." Wiring mistakes are loud at pipeline-load time, not silent at run-time. This is a deliberate rejection of magic.

---

## 15. Validation findings (bench V2 + viewer audits, 2026-04-18)

After drafting this doc, both existing projects were audited against it. Key refinements and resolved questions recorded here.

### 15.1 Resolved (doc updated)

- **Multi-algorithm steps** — bench's `boundary-edges/algos/*` pattern (18 variants + registry) survives as a single step with multiple native C algo implementations dispatched by params. Documented in §4.1.
- **Iterative steps** — `shadow-fit`, `ortho-lidar-register` remain monolithic from the DAG's POV. Internal iteration may emit `debug` intermediates. Documented in §4.1.
- **SDK stability ≠ artifact stability** — §4.10 now separates these. SDK follows semver (`CHANGELOG.md`); artifacts carry per-write stability in `meta.json`.
- **Location picker, seller demo** — added to §11 repo layout as browser-JS apps under `v3/apps/`. No backend coupling.
- **Bench 223 test cases stay in bench-v2.** Not migrated. v3 uses its own small set of real test cases (JSON pipeline-args files) running against live fetches; synthetic case loaders are out of scope.
- **Visualizers (`.viz.js`)** — three subtypes (builtin / step / composite), two on-disk locations outside lab-ui (`v3/steps/<name>/viz/` for step-visualizers, `apps/scene-base/src/viz/` for composites). Lab-ui has built-in kind-dispatched fallbacks. Canonical reference: [VISUALIZERS.md](VISUALIZERS.md).
- **Generic pipeline args** — no hardcoded notion of "place coordinate"; each pipeline declares its own input schema (§4.3). Enters the DAG as the `pipeline-input` root artifact (§9.2).
- **Content-hashing universal** — every step output is content-hashed; cache-skip on rerun handled by a persistent `step_identity → content_hash` intent map (§9.1). `output_hash` flag retired (see N9); see HASH-MODEL.md and ADR-001 for the full design.

### 15.2 Deferred with clearer rationale

- **Q7 bench.db** — not migrated. Scorer/metrics become steps producing subscribed artifacts; batch mode + log events covers any downstream run-history tooling. Bench v2's SQLite stays in bench v2 for as long as bench v2 lives.
- **Q10 real-buildings importer** — not a separate concern. The unified architecture handles it through a normal input-source step chain (picker provides coords/references, fetch step gets the model). No special tooling.

### 15.3 Decisions

- **N1 Execution model (subscription + DAG walk).** Subscription-driven backward DAG walk with late-subscribe support is implemented as part of Phase A A6 (runner subsystem), not retrofitted to bench/viewer. Validated by the smoke pipeline.
- **N2 Protocol migration — clean break. (Decided 2026-04-18.)** No backward compatibility. No current customers to preserve. The v3 binary speaks the §5 + §8.1 protocol only; old bench/viewer protocols remain in their respective legacy codebases, not touched. SDK is rewritten, semver starts at 1.0.
- **N3 Control plane unavailability.** Cached keys remain valid for the cache TTL; beyond that, a configurable grace window (default: continue serving up to 10 min after last successful verify, with usage events buffered locally). Fail closed after grace expires. Implemented in Phase A A10.
- **N4 Streaming step semantics (Q5).** Resolved via `STREAM_CHUNK` in B9 (not via overloaded `ARTIFACT_HEADER` as originally sketched). A write declared `streaming: true` may be produced via `step_write_output_stream(ctx, name, bytes, len, final)`. Each call emits a `STREAM_CHUNK` text frame + one binary frame; `seq` is zero-based per artifact; exactly one call has `final: true`. After the final chunk the server *also* emits a regular `ARTIFACT_HEADER` + binary for the finalised bytes (dual delivery — see PLAN-VIEWER-PORT §7.3). Implemented in B9; first consumer is `laz-extract-disc`.
- **N5 Visibility — debug by default. (Decided 2026-04-18.)** Every new write declares `visibility: "debug"` unless a concrete caller requires public access. Promotion to `public` happens per write, when a real product need appears. Composes naturally with subscription + DAG walk: the viewer SDK only subscribes to artifacts its current version consumes; the runner only computes the DAG reaching those; everything else stays internal. No bulk public-surface audit.
- **N6 Performance baseline — skip. (Decided 2026-04-18.)** No current customers, no SLAs. Measure the new architecture against an absolute "feels responsive" bar when it's running.
- **N7 Hash formula correction. (Decided 2026-04-20.)** Removed erroneous `pipeline_input_canonical_bytes` from the derivation hash formula (§9.1). Transitive state propagates through `sorted_read_hashes_joined`; the pipeline identity does not belong in per-step hashes.
- **N8 Content-hashed outputs for normalization steps. (Decided 2026-04-20, SUPERSEDED by N9 on 2026-04-21.)** Added `output_hash: "derivation" | "content"` per write in step meta.json. Derivation was default; content was opt-in for funnel/canonicalization steps. This solved 20m-rounding cache convergence but introduced a false dichotomy (§9.3 pre-refactor). Superseded by N9 which unifies content-hashing everywhere and adds the intent map for cache-skip.
- **N9 Hash-model unification — content-hash everywhere + intent map + `cached_compute` primitive. (Decided 2026-04-21.)** Retired the `output_hash: DERIVATION | CONTENT` enum. All step outputs are content-hashed (same bytes → same store key, convergence automatic). Cache-skipping on step reruns is handled by a new persistent data structure, the intent map: `step_identity → content_hash`. A single runtime primitive `cached_compute(identity, fn)` orchestrates the cache-or-compute flow at any scope (step level, sub-step level for per-URL / per-element caching). `fetch_and_cache` becomes a thin wrapper over `cached_compute`. The `skip_on_identity: bool` flag (default `true`) remains as an escape hatch for non-deterministic steps. Also in this decision: store becomes two-tiered (hot RAM + warm disk) for zero-copy producer→consumer transfer within a pipeline run. See [`HASH-MODEL.md`](HASH-MODEL.md) for the full model, `PLAN-STORE.md` for the tiered-store implementation plan, `PLAN-RUNNER.md` T15–T16 for the runner work.

---

## 16. References

- `docs/LAUNCH-PLAN.md` — business rollout timeline
- `lidar-algos-bench-v2/CLAUDE.md` — metric versioning rules (preserved in target)
- `lidar-algos-bench-v2/lib/pipeline/steps/index.js` — current step registry
- `lidar-3d-environment-viewer/backend/pipeline/pipeline.js` — current viewer pipeline
- `lidar-3d-environment-viewer/docs/COPIED-FROM-BENCH.md` — the duplication this doc eliminates
- [`quality/README.md`](quality/README.md) + [`docs/quality/README.md`](docs/quality/README.md) — validation matrix design + how-to (cross-ref'd from §18.6)
- [`plan/PLAN-THIRD-PARTY-RESEARCH.md`](plan/PLAN-THIRD-PARTY-RESEARCH.md) — research-track umbrella plan (gated on `-DDIORAMAP_RESEARCH=ON`, never enters the prod binary). First track: roof-recon ([`research/roof-recon-survey.md`](research/roof-recon-survey.md)).

---

## 17. Logging

Logging is a first-class subsystem, not an afterthought. It is the primary tool for debugging production incidents, auditing compute resource use, and understanding runtime behavior. Built first in Phase A (§12.1 A2), before any subsystem that emits events — every subsequent subsystem codes against this API from day one. Must be:

- **Structured**, machine-parseable. One event per line, JSON.
- **Timestamped** with monotonic + wall-clock. `(ts_wall_iso, ts_mono_ns, ...)`.
- **Categorized** by subsystem and event kind.
- **Leveled** (debug/info/warn/error/fatal), with runtime config.
- **Low-overhead** enough that info-level is always on in prod.

**Scope: flat events with durations, no distributed-tracing machinery.** We deliberately do **not** adopt OpenTelemetry, Jaeger, Zipkin, or any tracing library. Those tools solve cross-service correlation in a multi-process, multi-host system; we are one process. Flat events with a `duration_ms` field on "complete"-type events cover 95% of the debugging value at a tiny fraction of the complexity.

The event schema reserves `span_id` and `parent_span_id` fields (unused in Phase A). If we later want hierarchical spans for deep perf debugging inside a single step, we add a per-thread span stack (~150 LOC) without a schema break. The JSON-lines output format is compatible with any OpenTelemetry collector that can ingest JSON, should that ever matter.

### 17.1 Event schema

Every log line is a JSON object:

```json
{
  "t":         "2026-04-19T12:34:56.789Z",
  "mono_ns":   12345678901234,
  "level":     "info",
  "subsystem": "runner",
  "event":     "step_complete",
  "run_id":    "r7",
  "session":   "s_abc123",
  "step":      "compute-normals",
  "duration_ms": 243,
  "cache_hit": false,
  "thread_id": 4,
  "...":       "additional event-specific fields"
}
```

- `event` is a **stable string enum** — grepable, analyzable. Documented per subsystem. Examples: `run_started`, `run_complete`, `step_started`, `step_complete`, `cache_hit`, `cache_miss`, `cache_evict`, `session_open`, `session_destroyed`, `grace_period_expired`, `subscription_denied`, `cuda_kernel_launch`, `cuda_stream_sync`, `fetch_attempt`, `fetch_retry`, `fetch_failure`, `control_plane_verify_fail`.
- Fields beyond the common ones are event-specific. Keep them stable once defined; a new event kind is cheap to add.
- **No free-text messages as the primary signal.** A `msg` field may carry a human note but should not carry semantics; the semantics live in structured fields.

### 17.2 Subsystems

Fixed set, extended only by explicit decision:

| subsystem | owns events around |
|---|---|
| `runner` | DAG walk, step dispatch, step_started/complete, errors |
| `store` | content-hash lookups, cache hits/misses, evictions, writes, mmap ops |
| `session` | open/close/reconnect/grace expiry, ref changes |
| `transport` | WS accept, WS close, frame send, HTTP request served |
| `fetch` | outbound HTTPS attempts, retries, failures, IGN quotas |
| `controlplane` | key verify, usage batch flush, grace-mode entry/exit |
| `cuda` | context init, stream ops, kernel launches, transfer timings |
| `log` | self: rotation, backpressure, dropped lines |

### 17.3 Output

- **Stdout** in a structured JSON-lines format by default. Systemd / container platforms pick it up.
- **Optional file sink** for local deployments, with rotation (size-based, capped count).
- **No stderr duplication.** Stderr is for hard crashes / panic traces only.

### 17.4 Sampling and backpressure

- Debug-level events can be heavy; default level is info in prod. Debug enabled via config signal (`SIGUSR1` or admin HTTP endpoint). Time-bounded; auto-revert after N minutes.
- If the log sink can't keep up (disk full, pipe blocked), logs are dropped after a bounded internal queue fills, and a single `log_dropped_count` event is emitted when flow resumes. We never block compute on logging.

### 17.5 Correlation

- Every event involving a run carries `run_id`.
- Every event involving a client carries `session`.
- Every event from a compute thread carries `thread_id`.
- Tools can reconstruct a full trace by filtering on `run_id`.

### 17.6 Tooling

Out of scope for the binary itself, but expected downstream:

- Logs fed to an analysis pipeline (ELK, Loki, or plain jq scripts during dev).
- Golden trace fixtures for smoke pipeline: the exact sequence of event kinds from a successful run becomes a test artifact. Regressions in the event stream fail CI.

---

## 18. Testing

Three layers of tests, each running in CI.

### 18.1 Unit tests

Close to the code they test. One test binary per subsystem of `core/`. Framework: a minimal custom harness (~200 LOC, no external dep) exposing `TEST(name)`, `ASSERT_*`, parallel test execution, xfail support. Drives:

- **Runtime primitives** (threads, queues, atomics): TSan + ASan builds in CI, race detection on stress loops.
- **Binary layout encode/decode** (§5): fuzz the decoder, round-trip arbitrary columns.
- **Store + cache**: in-memory only, no disk; integration tests below cover disk.
- **Hash + content addressing**: property tests (same input → same hash, different input → different hash with overwhelming probability).
- **DAG walker**: unit tests on synthetic step graphs.

### 18.2 Golden / parity tests (migration phase only)

Goldens exist for one specific purpose: **validating that a C port matches the JS reference during migration**. They are a transitional tool, not a permanent test layer.

Scope strictly limited to:

- **A small set of real test cases** (Dahouet primary; 2–3 others selected case-by-case). Not the 223 bench fixtures.
- **A real test case in v3 is tiny**: a JSON file with the pipeline args (coords, tile size, whatever the pipeline needs). Data flows through the normal fetch chain at run time — no special fixture-loader step, no embedded LAZ/OBJ payloads.
- **Synthetic test cases from bench-v2 are out of scope.** They need a dedicated loader for their in-memory format; not a Phase A/B priority and easy to bolt on later if needed.

Per-step layout during migration:

```
steps/compute-normals/golden/
├── dahouet.input.json          # pipeline args for this case
├── dahouet.expected.bin        # expected output (from JS reference)
└── dahouet.meta.json           # { step_version, params, epsilon }
```

The golden harness runs the full chain (not just the step in isolation — inputs are whatever the pipeline produces upstream) and compares the step's output within ε.

**Post-migration**: goldens can be retired. The C implementation becomes the reference; there's no longer a JS version to diff against. Unit tests and smoke integration stay; individual step goldens graduate out. The harness itself stays around as infrastructure for any future pinning-down of algorithm behavior, but the per-step golden directories can be pruned once a port is considered stable.

### 18.3 Integration tests (smoke pipeline)

Driven by the batch-mode CLI. Launches `dioramap-processor batch` against the smoke pipeline (§12 A12) and verifies:

- Manifest contents match expected artifacts
- Artifact files exist with correct kinds, sizes, column layouts
- Second run hits cache (zero compute, measured by log event stream)
- CUDA build produces bit-identical `gpu-identity` vs CPU reference

Also driven by a WS test client for serve-mode flows:

- Connect, RUN, receive expected artifact stream, disconnect
- Reconnect within grace, verify cached delivery
- Late SUBSCRIBE flow
- Session destruction after grace

### 18.4 Test data

All fixtures live in `v3/tests/fixtures/` and are small — JSON files with pipeline args, possibly a handful of tiny reference artifacts. No large LAZ/OBJ payloads in the repo. Real-world data for goldens flows through the normal fetch chain at test time (and is cached by the store after first fetch, so subsequent runs are offline-fast).

### 18.5 Running tests

```
# All tests
ninja -C build test

# Only unit
ninja -C build test-unit

# Only goldens for a step
ninja -C build test-golden-compute-normals

# Smoke pipeline
./build/dioramap-processor batch --pipeline smoke/three-step-smoke --output-dir /tmp/smoke-out
./v3/tests/smoke/verify.sh /tmp/smoke-out
```

All three layers must pass before a PR merges. CI matrix covers CPU-only and CUDA builds.

### 18.6 Variant matrix

The three layers above describe *what* gets tested. The
**validation matrix** describes *how the same tests get exercised
under every form of runtime instrumentation we have*: release for
perf baseline, ASan+UBSan for heap correctness, TSan+UBSan for
data-race detection, llvm-cov for coverage gaps, libFuzzer for
input-handling bugs, valgrind helgrind/drd as a complementary race
detector running on uninstrumented binaries. Each variant catches
a class of bug the others miss; running the same workload across
all of them is the canonical way to validate concurrency + memory
invariants.

Mutually exclusive: ASan vs TSan vs MSan (shadow memory layout).
clang `-Wthread-safety` auto-applies on any clang build (validates
the annotations in `core/include/v3/thread_safety.h`).

Variants the dev box can't run (e.g. TSan on WSL2 — kernel won't
permit the address-space control TSan needs) are built locally and
shipped to a real-Linux host as a self-contained tarball. See
`v3/docs/quality/README.md` §4 for the bundle build /
scp / run / cleanup procedure used for the May 4 prod TSan
validation.

Full design + governance: `v3/docs/quality/README.md`.
Quick reference: `v3/docs/quality/README.md`.

---

## 19. Multi-node scaling: abstraction, not implementation

The initial implementation is single-node. But storage and session state will eventually need to scale across multiple machines, and the abstraction boundaries are set up now so that future distribution is a matter of swapping implementations, not redesigning the runtime.

### 19.1 Two abstractions to preserve

**Store backend.** The artifact store exposes a narrow interface:

```c
// conceptual; actual in C headers
store_t* store_open(store_config_t);
store_result_t store_lookup(store_t*, const hash_t*);        // cache hit / miss
store_handle_t store_begin_write(store_t*, const hash_t*);   // allocate for producer
void store_commit_write(store_handle_t, size_t n_bytes);
store_handle_t store_mmap_read(store_t*, const hash_t*);     // read handle
void store_release(store_handle_t);
void store_pin(store_t*, const hash_t*, ref_owner_t);        // eviction guard
void store_unpin(store_t*, const hash_t*, ref_owner_t);
```

Implementation v1: local filesystem + mmap. Possible future implementations: shared NFS mount (trivial — same interface), S3 with local disk as write-through cache, distributed KV (Cassandra, FoundationDB) for the hash index with object storage for bytes.

The constraint: **no code outside `core/cache/` knows how the store persists.** Everything interacts through the interface above.

**Session state backend.** Sessions are a map of `session_token → { tier, disk_refs, last_seen }`. Currently in-process. Interface:

```c
sessions_t* sessions_open(sessions_config_t);
session_t  sessions_create(sessions_t*, tier_t);
session_t  sessions_lookup(sessions_t*, const token_t*);
void       sessions_add_ref(session_t, const hash_t*);
void       sessions_touch(session_t);                         // update last_seen
void       sessions_destroy_if_expired(sessions_t*, ttl_t);
```

Implementation v1: in-process hashmap with a mutex. Future: Redis or Postgres for cross-node session sharing, so a client reconnecting to a different node via a load balancer finds its session.

### 19.2 What to avoid now

- Do not add distributed-aware code paths. No "if clustered" branches. No leader election. No gossip. Premature distributed complexity is catastrophic.
- Do not choose network serialization formats for the store interface. Interface is in-process C calls; if it later crosses a network, a shim implementation handles it without leaking into callers.
- Do not pre-allocate a clustering config schema. Config is for a single node.

### 19.3 What to do now

- Keep the two interfaces above clean — no implementation leakage outside `core/cache/` and `core/session/`.
- Write the cache directory layout (§9.3) such that a shared filesystem implementation would work unchanged. Specifically: no machine-local paths in content hashes, no assumption of exclusive filesystem ownership (lock files or atomic rename for write coordination should be portable to NFS).
- Write session-ref accounting such that refs are individually identified by `(session_token, artifact_hash, owner)` — so a future remote session backend can update shared refs without losing track.

This buys us the option to scale later without paying for it now. A reasonable future project would swap in S3-backed store + Redis sessions, with all other code unchanged.

---

*End of unified architecture doc. This is a living document; update as decisions land.*
