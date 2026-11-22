# Steps audit — 2026-04-21

> **Note: snapshot.** Findings below reflect the state on 2026-04-21, predating the **B3.5 hash-model refactor** (commit `f359b50`). The B3.5 refactor retired `step_output_hash_t` entirely and reset most visibility / kind issues that were flagged here. Most B-series violations below are now resolved by the refactor itself; spot-check against current code before acting on any specific item. The audit is retained for historical context.

## Summary
16 steps audited across demo/gpu-smoke/fetchers/coords/copc.
**12 breaking violations**, 3 missing-items, **5 robustness issues**, plus numerous correctly-followed patterns.

Dominant class of breaking finding: `STEP_VIS_PUBLIC` used as default in demo/fetch/coords/copc steps even though N5 (UNIFIED §15.3) mandates `debug` unless a concrete caller needs public, and PLAN-VIEWER-PORT §4.9 names the concrete public set.

---

## Breaking (doc says X, code says not-X)

### B1 — Visibility default violated (N5 / PLAN-VIEWER-PORT §4.9/§4.10)
UNIFIED §15.3 N5: *"Every new write declares `visibility: 'debug'` unless a concrete caller requires public access."* PLAN-VIEWER-PORT §4.9 lists the concrete public artifacts; anything not on it stays debug.

Non-public writes incorrectly marked `STEP_VIS_PUBLIC`:

- `steps/coords/canonicalize-location.c:129` — `canonical-location` (never in §4.9 public list, §4 calls it a DAG internal wiring artifact).
- `steps/copc/find-copc-tiles.c:158` — `copc-tile-urls` (internal plumbing; not in §4.9).
- `steps/copc/fetch-copc-tiles.c:78` — `copc-raw-tiles` (internal plumbing; not in §4.9).
- `steps/fetchers/fetch-ortho-rgb.c:103` — acceptable per §4.9 (`ortho-rgb-jpeg` is public), but see B7 re: kind/content.
- `steps/fetchers/fetch-elevation.c:97` — the write name `elevation-bil` is NOT in §4.9; §4.9 lists `elevation-f32`. See B8.
- `steps/demo/*` (scale-by-factor, add-bias, multiply, report, split-range, fan-process, reduce-sum) — all `STEP_VIS_PUBLIC`. Demo steps should default debug per N5; no concrete caller requires `public`.

### B2 — Kind mismatch for laz-extract-disc (UNIFIED §5 / PLAN-VIEWER-PORT §9)
`steps/copc/laz-extract-disc.c:97-113` declares all four writes (`lidar-positions` F64, `lidar-classifications` U8, `lidar-intensities` U16, `lidar-num-returns` U8) as `kind = AK_OPAQUE_JSON`. Per §5 these are columnar numeric buffers. PLAN-VIEWER-PORT §9 reserves `POINT_COLUMN` for exactly this. The step even comments *"raw bytes; kind is informational here"* (line 99) — but the kind field is contractual per §4.1/§5 and used by downstream tooling. Should be a per-point-array / point-column kind, not JSON.

### B3 — Kind mismatch for canonical-location, scene-origin, fetch metadata
- `steps/coords/canonicalize-location.c:128` writes `AK_OPAQUE_JSON` for a small metadata object — **correct by §5** (JSON <64 KB fits kind 7).
- `steps/fetchers/fetch-ortho-rgb.c:102` and `fetch-elevation.c:96` use `AK_OPAQUE_JSON` but the write is a JSON *pointer metadata* (contains `blob_hash`). The real artifact that §4.9 promises (`ortho-rgb-jpeg`=raw JPEG, `elevation-f32`=Float32 grid) is not produced. This is a design drift: the step emits metadata pointing at the cached blob rather than the bytes themselves, so downstream must parse JSON + do a second store lookup to get the real content. PLAN-VIEWER-PORT §4.9 and §9 expect the artifact itself.

### B4 — Demo int32 writes use kind=1 (AK_PER_POINT_ARRAY)
`steps/demo/scale-by-factor.c:50`, `add-bias.c:50`, `multiply.c:28`, `reduce-sum.c:40` declare `.kind = 1` (AK_PER_POINT_ARRAY) for a 4-byte `int32` scalar. Per §5, `AK_PER_POINT_ARRAY` implies SoA per-point data; a lone scalar has no such shape. Closest fit is `AK_OPAQUE_JSON` or a new scalar kind. Same for `reduce-sum.c:40` (i64 scalar) and `gpu-smoke.c:55,92` (copy of arbitrary bytes). Probable bulk find/replace drift.

### B5 — gpu-smoke streaming/compute_location guard
`steps/gpu-smoke/gpu-smoke.c:66-70` conditionally flips `compute_location` between CPU and GPU at compile time. §4.4 says compute_location is fixed at build time — OK. However line 55,92 declare `smoke-output` and `smoke-input` as `kind = 1` (AK_PER_POINT_ARRAY) for an arbitrary opaque byte copy; see B4.

### B6 — Output-hash mismatch for `laz-extract-disc`
`steps/copc/laz-extract-disc.c:97-113` uses `STEP_OH_CONTENT` for all four writes. PLAN-VIEWER-PORT §10: *"laz-extract-disc | lidar-positions etc. | content"* — matches. **Not** a violation. Re-categorized as Green G1.

### B7 — Output-hash / output contract for `fetch-ortho-rgb` and `fetch-elevation`
§10 says *"`fetch-ortho-rgb` et al. bytes derivation"* — the write's hash should be derivation-hashed on the actual JPEG bytes. Current code stores a JSON metadata envelope with an embedded `blob_hash`, derivation-hashed. Two different `canonical-location` inputs can legitimately produce the same underlying blob (the fetcher dedupes by URL), but the step's metadata JSON still contains `bbox_l93` + `size_px` + `layer` → the metadata envelope differs whenever any of those differ, preventing downstream caches from converging even when the underlying image is identical. The PLAN's intent is for the step's artifact to *be* the bytes. See B3.

### B8 — Write name mismatch
`steps/fetchers/fetch-elevation.c:95` writes `elevation-bil`. PLAN-VIEWER-PORT §4.2/§4.9 names the artifact `elevation-f32`. Same pattern: fetch-ortho-rgb writes `ortho-rgb-jpeg` (matches) but as metadata (B3).

### B9 — Stale doc comments on fetchers
`steps/fetchers/fetch-ortho-rgb.c:2` and `fetch-elevation.c:2` comment blocks claim `// Reads: pipeline-input`. Actual `reads_arr[]` at line 99/93 reads `canonical-location` (correct, matches §4.2 post-canonicalize rule). Comments are misleading.

### B10 — `find-copc-tiles` uses `fetch_get`, not `fetch_and_cache`
`steps/copc/find-copc-tiles.c:73` calls `fetch_get(client, url, &resp)` directly. Per PLAN-FETCHERS §F4b: *"every fetcher step ... goes through `fetch_and_cache` — no step calls libcurl directly."* The URL is a deterministic function of bbox + output format; the WFS response for the same bbox is byte-stable enough to cache. (Even if not, the current code does not even pass through `fetch_and_cache`; it bypasses the URL-keyed cache entirely.)

### B11 — Pipeline reserved-name check (happy path)
No step writes to `PIPELINE_INPUT_ARTIFACT`. Clean. Green G2.

### B12 — fetch-copc-tiles `n == 0` returns error
`steps/copc/fetch-copc-tiles.c:25-26` returns `-1` when the input array is empty. PLAN-VIEWER-PORT §6.1/§6.2 implies writing an empty ARRAY_BY_HASH manifest is valid (see `demo/fan-process.c:22` which does exactly that). Returning -1 makes every empty-array path an error, which breaks the DAG walker's pruning expectation when `find-copc-tiles` legitimately returns 0 tiles at edge-of-coverage. Should write an empty manifest or rely on `find-copc-tiles` to already have returned -1 (which it does at line 91 — but that is itself a design debate).

---

## Missing

### M1 — No content-hash enforcement review for `compute-scene-origin`
PLAN-VIEWER-PORT §10 implicitly lists it under *"all downstream / internal"* → derivation. Current file uses derivation (line 127). Correct. No fix needed. (Noted for completeness.)

### M2 — `array: true` flag missing on some reads?
Spot-checked: `laz-extract-disc.c:94`, `fetch-copc-tiles.c:73`, `fan-process.c:57`, `reduce-sum.c:36` all set `.array = true` where reading arrays. `copc-tile-urls`/`copc-raw-tiles` writes all have `.array = true`. Clean.

### M3 — `kind = AK_PER_POINT_ARRAY` defaulting via `1` literal
All demo + gpu-smoke files use the magic integer `1` instead of the enum `AK_PER_POINT_ARRAY`. Compiles fine but hides B4; harder to grep. (Cosmetic but wanted for hygiene.)

---

## Robustness (non-blocking)

### R1 — `canonicalize-location.c:46`
`double lat = json_number_value(j_lat)` is called even if `j_lat` is missing (guard at line 41 only checks NULL before calling number_value when it's valid). If the field is non-numeric, `json_number_value` returns 0.0 silently — step produces a wrong canonical location instead of failing.

### R2 — `compute-scene-origin.c:69-70`
`cx = json_number_value(json_object_get(loc, "center_l93_x"))` without checking presence. Missing field → 0.0. Same for `radius` in `laz-extract-disc.c:37`.

### R3 — `find-copc-tiles.c:107-112`
If a feature has neither `url` nor `name`, silently skipped. Fine. But `snprintf` with raw JSON-unsafe characters in `url`/`name` can produce malformed JSON (no escape). Low risk against IGN-controlled strings but not robust against future endpoints.

### R4 — `fetch-copc-tiles.c:41`
If the array item is not a valid JSON object (e.g. find-copc-tiles' output got truncated), `json_loadb` returns NULL and returns -1. Fine but a partial fetch loop will cancel mid-way, leaving already-stored blobs orphaned (no cleanup; store will GC later). Acceptable given store LRU.

### R5 — `laz-extract-disc.c:67-70`
`laz_extract_disc_from_tile` return != 0 aborts the whole loop. Any single corrupt tile fails the whole run. Consider continuing on per-tile error with a log.

---

## Green (verified as correct, per doc)

- **§6.3 ordering contract**: `split-range` emits [0..N-1] by construction; `find-copc-tiles` `qsort`s by URL string (line 125); `fan-process` iterates in order; `fetch-copc-tiles` iterates `copc-tile-urls` in input order. All canonical.
- **§6.4 fan-out pattern**: `step_array_open` is paired with `store_release` in `laz-extract-disc.c:60,66`, `reduce-sum.c:21,24,29` (all error paths too). Clean.
- **§9.3 / §10 content-hash opt-ins**: `canonicalize-location` uses `STEP_OH_CONTENT`; `find-copc-tiles` content; `fetch-copc-tiles` content; `laz-extract-disc` content; everything else derivation. Matches §10 table exactly.
- **No libcurl in steps**: grep `curl_` and `<curl/` across `steps/` → zero hits. Goes through `fetch_and_cache` / `fetch_get`.
- **No endpoint strings in `core/src/fetch/fetch.c`**: grep for `geopf|ign|wfs|wms|https://` → zero hits. Fetcher is genuinely domain-agnostic (PLAN-FETCHERS §Scope).
- **Reserved name**: no step writes to `PIPELINE_INPUT_ARTIFACT`.
- **Canonicalize-as-root rule**: all real-data-fetching steps (`fetch-ortho-rgb`, `fetch-elevation`, `find-copc-tiles`, `compute-scene-origin`, `laz-extract-disc`) read `canonical-location`. Only `canonicalize-location` reads `pipeline-input`. Demo/gpu-smoke stubs read pipeline-input (allowed per audit scope).
- **Step version = 1** across all 16 registered steps; no stale "bump when…" comments.
- **No non-const mutable global state** outside `fetchers/fetchers.c` (`g_lock`, `g_client`), which is the documented exception.
- **step_def fields complete**: all 16 steps populate name/version/reads/writes/visibility/output_hash/compute_location/streaming/exec.
- **array flag consistency**: producer and consumer `.array` fields match across split-range↔fan-process, fan-process↔reduce-sum, find-copc-tiles↔fetch-copc-tiles, fetch-copc-tiles↔laz-extract-disc.
- **No step is streaming yet** — T14 deferred to B9 per PLAN-RUNNER. Clean.
