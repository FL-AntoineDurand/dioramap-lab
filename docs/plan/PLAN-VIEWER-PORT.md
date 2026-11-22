# Viewer Backend Port Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md) and the subsystem plans. Scope: port every backend algorithm in `lidar-3d-environment-viewer/backend/` to a set of v3 steps that can run under the `dioramap-processor` serve/batch runner. Frontend SDK is out of scope — we ship identical outputs on the wire, the SDK adapts later.*

---

## 1. Why this doc exists

A7 is done; the runtime can execute arbitrary step DAGs over WS. Before writing fetchers (A9) or porting any real algo (Phase B), we need to answer four concrete questions:

1. **What fetchers are actually needed** — endpoints, payload types, caching behaviour.
2. **What steps exist** — break the monolithic `runPipeline` into atomic, content-addressed, cacheable steps with clean reads / writes.
3. **What's the DAG** — which step feeds which, where can parallelism happen, where are the cache funnel points.
4. **What new runtime primitives are required** — so we can scope the ones that need to land alongside or before the ports.

Answering these here scopes A9 (fetchers) and the first batch of Phase B algorithm ports with concrete requirements instead of generic infrastructure.

## 2. What the JS pipeline does (one paragraph)

Given `{ lat, lon, radius }`, fetch four external data sources in parallel (COPC lidar tiles via IGN, RGB + IRC ortho photos via IGN WMS, elevation grid via IGN WMS, cadastre parcels + buildings via IGN WFS + BD-Ortho photo metadata via IGN WFS). Compute per-point normals and ceiling flags. Co-register ortho to lidar (multi-pair edge correlation). Classify each point as canopy / grass / neither (NDVI + normal scatter + building mask). Raster a non-canopy ground min-Z grid. Auto-fit cadastre to the LiDAR building mask. Run deshadow: detect photo shadows, coarse-to-fine sun-time search, synthetic shadow cast, category-aware darkness correction. Emit all the above as WS messages.

## 3. External data sources (A9 scope)

| Source | Protocol | URL pattern | Payload | Notes |
|---|---|---|---|---|
| IGN WMS — ortho RGB | HTTPS GET + query params | `https://data.geopf.fr/wms-r/wms?SERVICE=WMS...LAYERS=ORTHOIMAGERY.ORTHOPHOTOS...FORMAT=image/jpeg` | JPEG bytes | 2048×2048 at `radius=75m` → ~500 KB |
| IGN WMS — ortho IRC | same endpoint, `LAYERS=ORTHOIMAGERY.ORTHOPHOTOS.IRC` | JPEG bytes | same | Optional: layer may be absent in some regions |
| IGN WMS — elevation | same endpoint, `FORMAT=image/x-bil;bits=32`, `LAYERS=ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES` + fallback `ELEVATION.ELEVATIONGRIDCOVERAGE` | raw Float32 grid | 512×512 grid, 1 MB. Fallback path needs 3-pass box smoothing. |
| IGN WFS — COPC tile discovery | `https://data.geopf.fr/wfs/ows?SERVICE=WFS...TYPENAMES=IGNF_NUAGES-DE-POINTS-LIDAR-HD:dalle` | JSON GetFeature | Returns a list of `{name, url}` for tiles overlapping the bbox — typically 1–4 tiles |
| COPC LAZ tiles | HTTPS GET | Whatever URL the WFS returned (typically `https://...lidar-hd/...laz`) | LAZ-compressed octree | **Whole-tile fetch** (not per-node range like the JS code). This is a step-level decision made here in PLAN-VIEWER-PORT — the fetcher library is domain-agnostic and just fetches a URL. Rationale: a tile is 100–500 MB; one store entry per tile; prod store budget is TB-scale so caching whole tiles is cheap. On a second request, the local file is `mmap`'d and the COPC parser reads nodes out of RAM — zero network. Simpler cache, fewer HTTP requests, fewer failure modes. |
| IGN WFS — cadastre parcels | `...TYPENAMES=CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle` | JSON GetFeature | Up to 2000 polygons |
| IGN WFS — cadastre buildings | same, `:batiment` | JSON GetFeature | Up to 5000 polygons |
| IGN WFS — BD-Ortho photo metadata | same, `TYPENAMES=ORTHOIMAGERY.ORTHOPHOTOS.GRAPHE-MOSAIQUAGE:graphe_bdortho` | JSON GetFeature | One feature with `date_vol`, dept, resolution, campaign |

**A9 implications:**

- HTTPS client → **libcurl + mbedTLS**, per §D3 / PLAN-FETCHERS decision 2. (Inbound TLS terminated by nginx per §D14; outbound still needs its own TLS stack.)
- Response types: raw bytes (image / BIL / LAZ) and UTF-8 JSON. No SOAP, no XML.
- **Caching**: fetchers cache-by-URL. Fetcher lib exposes a `fetch_and_cache(store, url) -> hash` helper — a thin wrapper over the runtime's `cached_compute` primitive (see HASH-MODEL.md §3) with a URL-derived step identity. On intent-map hit, returns the stored content hash with zero HTTP; on miss, HTTP-fetches and `store_put`s the bytes, records the identity→content mapping, returns the content hash. Every fetcher step uses this or the plain `cached_compute` — never calls libcurl directly.
- **Auth**: IGN geopf.fr endpoints are anonymous. (Changes when we hit paid endpoints later.)
- **Rate limits**: observed 429 responses under concurrent bursts. Retry / backoff lives in the fetcher lib, with a per-host token bucket shared across concurrent steps.

## 4. Step inventory

Grouped by role. Each row is one `step_def_t` in v3 terms.

### 4.1 Coordinate canonicalization

| Step | Reads | Writes | Params |
|---|---|---|---|
| `canonicalize-location` | `pipeline-input` | `canonical-location` (JSON: quantized lat, lon, radius) | `cell_size_m` (default 20) |

The first step after `pipeline-input`. Quantizes the client's raw `{lat, lon, radius}` to a 20m grid so that small user movements hit the cache instead of re-running the pipeline. Every downstream step — fetchers included — reads `canonical-location` instead of `pipeline-input`. The raw input is kept as the DAG root for traceability but no step downstream depends on it.

Radius quantization bumps up to the next canonical bucket (e.g. 50/75/100/125m) so a user asking for 76m and 74m both produce the same canonical-location.

### 4.2 Fetchers (A9 deliverables)

| Step | Reads | Writes | Params |
|---|---|---|---|
| `find-copc-tiles` | `canonical-location` | `copc-tile-urls` (**array artifact**, inline JSON: `[{url, name}, ...]`) | — |
| `fetch-copc-tiles` | `copc-tile-urls` | `copc-raw-tiles` (**array artifact**, by-hash: N LAZ blob hashes) | — |
| `fetch-ortho-rgb` | `canonical-location` | `ortho-rgb-jpeg` | `size_px`, `layer` |
| `fetch-ortho-irc` | `canonical-location` | `ortho-irc-jpeg` (may be empty on failure) | `size_px` |
| `fetch-elevation` | `canonical-location` | `elevation-f32` (Float32 grid in §5 layout) | `grid_size`, `layer_preference` |
| `fetch-ortho-surround` | `canonical-location` | `ortho-surround-jpeg` | `size_px` (default 1024), `radius_m` (default 600) |
| `fetch-elevation-surround` | `canonical-location` | `elevation-surround-f32` | `grid_size` (default 256), `radius_m` (default 600) |
| `fetch-cadastre-parcels` | `canonical-location` | `cadastre-parcels-geojson` | `max_count` |
| `fetch-cadastre-buildings` | `canonical-location` | `cadastre-buildings-geojson` | `max_count` |
| `fetch-photo-meta` | `canonical-location` | `photo-meta-json` | — |

Notes:

- `find-copc-tiles` and `fetch-copc-tiles` are split because COPC is the one case where multiple tile files cover the requested area. `find-copc-tiles` produces an **inline array artifact** (bytes = the JSON list). `fetch-copc-tiles` reads that array, loops internally (using libcurl multi-handle for concurrent downloads), calls the fetcher lib's `fetch_and_cache(url)` for each entry, and emits a **by-hash array artifact** pointing at the N stored LAZ blobs. Per-URL caching is automatic because `fetch_and_cache` hashes the URL. See §6 for the array-artifact semantics.
- All other fetchers are single-in / single-out. Their step identity = `hash(step_name, version, params, canonical-location-hash)` — same canonical-location → intent-map hit → cache hit (skip execution, reuse stored content hash).
- Surround is not a separate pipeline. It lives as two additional fetcher steps in the primary DAG; clients subscribe to `ortho-surround-jpeg` / `elevation-surround-f32` when they want it. If they don't subscribe, nothing downstream depends on surround artifacts — the DAG walker prunes those branches.

### 4.3 Geometry

| Step | Reads | Writes | Params |
|---|---|---|---|
| `laz-extract-disc` | `copc-raw-tiles` (array), `canonical-location` | `lidar-positions` (F64), `lidar-classifications` (U8), `lidar-intensities` (U16), `lidar-num-returns` (U8) | `point_budget`, `max_depth` |
| `compute-normals` | `lidar-positions` | `lidar-normals` (F32), `lidar-spacing` (F32) | `k` (default 24) |
| `compute-ceiling-flags` | `lidar-positions` | `lidar-ceiling-flags` (U8) | — |

`laz-extract-disc` is the step where per-tile data collapses to one disc-filtered point cloud. Its internal loop (see §6) opens each LAZ blob from `copc-raw-tiles`, runs laz-perf decompression, filters to the canonical-location's disc (`dx² + dy² ≤ radius²`), and unions into single output arrays. Downstream sees one combined artifact per column — no tile seams, no fan-out to reason about.

### 4.4 Compute scene origin

| Step | Reads | Writes | Params |
|---|---|---|---|
| `compute-scene-origin` | `canonical-location`, `elevation-f32` | `scene-origin` (JSON: x, y, z in L93) | — |

Pure math step. PROJ handles WGS84↔Lambert-93; midZ is the midpoint of elevation's min/max. Small JSON output.

### 4.5 Registration

| Step | Reads | Writes | Params |
|---|---|---|---|
| `decode-ortho-coreg` | `ortho-rgb-jpeg`, `ortho-irc-jpeg` | `ortho-coreg-rgb-rgba`, `ortho-coreg-irc-rgba` | `coreg_crop_frac`, `coreg_cols`, `coreg_rows` |
| `build-lidar-rasters` | `lidar-positions`, `lidar-intensities`, `lidar-num-returns`, `canonical-location` | `lidar-rasters` (maxZ, minZ, meanZ, density, intensity, roughness, multi-return; all F32) | `cell_size_m` |
| `build-ortho-rgb-rasters` | `ortho-coreg-rgb-rgba` | `ortho-rgb-rasters` (r, g, b, luminance, saturation, greenness, lumStddev) | — |
| `build-ortho-irc-rasters` | `ortho-coreg-irc-rgba`, `ortho-rgb-rasters` | `ortho-irc-rasters` (nir, ndvi) | — |
| `compute-ndsm` | `lidar-rasters` | `lidar-ndsm` (F32) | `local_min_window` (default 3) |
| `compute-building-mask` | `lidar-ndsm`, `lidar-rasters` | `lidar-building-mask` (U8) | (fixed thresholds) |
| `coregister-ortho-lidar` | `lidar-rasters`, `lidar-ndsm`, `lidar-building-mask`, `ortho-rgb-rasters`, `ortho-irc-rasters` | `coreg-offset` (JSON: dx, dy, per-pair scores) + `coreg-score-maps` (**debug**) | `cell_size`, `search_range_m`, `weights{A,AStar,B,C,D}` |
| `rasterize-cadastre-edges` | `cadastre-buildings-geojson`, `coreg-offset` | `cadastre-edges-raster` (U8, **debug**) | `thickness` |
| `fit-cadastre-to-lidar` | `cadastre-edges-raster`, `lidar-building-mask` | `cadastre-fit` (JSON: offsetX, offsetY, score) | `search_range_m` |

All raster-valued outputs (`*-rasters`, `*-ndsm`, `*-building-mask`, `*-score-maps`, `cadastre-edges-raster`) are `visibility: debug`. Only `coreg-offset` and `cadastre-fit` are `visibility: public`.

### 4.6 Classification

| Step | Reads | Writes | Params |
|---|---|---|---|
| `decode-ortho-tile` | `ortho-rgb-jpeg`, `ortho-irc-jpeg` | `ortho-tile-rgb-rgba`, `ortho-tile-irc-rgba` | `tile_cols`, `tile_rows` |
| `build-tile-ndvi` | `ortho-tile-rgb-rgba`, `ortho-tile-irc-rgba` | `tile-ndvi` (F32, **debug**) | (constants inlined) |
| `compute-normal-scatter` | `lidar-positions`, `lidar-normals`, `canonical-location` | `normal-scatter-raster` (F32, **debug**) + `cell-min-z` (F32, **debug**) | `canopy_cell_m` |
| `compute-near-building-mask` | `lidar-building-mask`, `cadastre-buildings-geojson`, `cadastre-fit`, `coreg-offset` | `near-building-raster` (U8, **debug**) | `dilate_r`, `canopy_cell_m` |
| `classify-vegetation` | `lidar-positions`, `lidar-normals`, `tile-ndvi`, `normal-scatter-raster`, `cell-min-z`, `near-building-raster` | `canopy-flags` (U8), `grass-flags` (U8) | thresholds |

### 4.7 Ground + parcels

| Step | Reads | Writes | Params |
|---|---|---|---|
| `build-lidar-ground` | `lidar-positions`, `lidar-classifications`, `canonical-location` | `lidar-ground-grid` (F32 min-Z, BFS-filled, no NaN) | `ground_cell_m` |
| `build-parcel-lines` | `cadastre-parcels-geojson`, `elevation-f32`, `lidar-ground-grid`, `cadastre-fit`, `scene-origin`, `canonical-location` | `parcel-lines` (F32 line segments, 6 floats each) | `height_offset_m` |

### 4.8 Shadow + deshadow

| Step | Reads | Writes | Params |
|---|---|---|---|
| `detect-photo-shadows` | `ortho-rgb-rasters` (luminance) | `photo-shadow-mask` (U8, **debug**) | thresholds |
| `find-best-sun-time` | `photo-shadow-mask`, `lidar-rasters` (maxZ), `photo-meta-json`, `canonical-location` | `sun-time` (JSON) + `sun-search-scores` (F32, **debug**) | search policy |
| `cast-shadows-from-dsm` | `lidar-rasters` (maxZ), `sun-time` | `synthetic-shadow-mask` (U8, **debug**) | `cell_size_m` |
| `deshadow-ortho` | `ortho-rgb-jpeg`, `synthetic-shadow-mask`, `ortho-irc-rasters` (ndvi), `sun-time` | `ortho-rgb-corrected-jpeg` | `crop_frac` |

`find-best-sun-time` has real per-candidate iteration (hundreds of sun positions × hundreds of ray casts each). First candidate for a real GPU port (per §7.6 canary pattern).

### 4.9 Output shape (no assembly step)

The JS pipeline packs seven columns into one `POINTS_FINAL` message (positions + normals + spacing + ceiling + classifications + canopy + grass). In v3 each column is its own public artifact. The client subscribes to whichever it needs and renders progressively as each arrives. No `assemble-points-final` step exists.

| Public artifact | Source step |
|---|---|
| `lidar-positions` | `laz-extract-disc` |
| `lidar-classifications` | `laz-extract-disc` |
| `lidar-intensities` | `laz-extract-disc` |
| `lidar-num-returns` | `laz-extract-disc` |
| `lidar-normals` | `compute-normals` |
| `lidar-spacing` | `compute-normals` |
| `lidar-ceiling-flags` | `compute-ceiling-flags` |
| `canopy-flags` | `classify-vegetation` |
| `grass-flags` | `classify-vegetation` |
| `lidar-ground-grid` | `build-lidar-ground` |
| `parcel-lines` | `build-parcel-lines` |
| `elevation-f32` | `fetch-elevation` |
| `elevation-surround-f32` | `fetch-elevation-surround` |
| `ortho-rgb-jpeg` or `ortho-rgb-corrected-jpeg` | `fetch-ortho-rgb` / `deshadow-ortho` |
| `ortho-surround-jpeg` | `fetch-ortho-surround` |
| `scene-origin` | `compute-scene-origin` |
| `coreg-offset` | `coregister-ortho-lidar` |
| `cadastre-fit` | `fit-cadastre-to-lidar` |
| `sun-time` | `find-best-sun-time` |
| `photo-meta-json` | `fetch-photo-meta` |

Per-column caching is a nice side effect: changing the `k` param for `compute-normals` invalidates `lidar-normals` + `lidar-spacing` but positions and classifications stay cached.

### 4.10 Visibility rule

`visibility: debug` means **lab UI only**. The viewer SDK is hard-coded to `tier: public` and never subscribes to debug artifacts. (Tier enforcement is deferred to A10 but the classification on each write is final.) Per §4.5 of the architecture doc.

### 4.11 Summary counts

| Category | Step count |
|---|---|
| Canonicalization | 1 |
| Fetchers (A9) | 10 (incl. find-copc-tiles, fetch-copc-tiles, surround pair) |
| Geometry | 3 |
| Scene origin | 1 |
| Registration | 9 |
| Classification | 5 |
| Ground + parcels | 2 |
| Shadow + deshadow | 4 |
| **Total** | **35 steps** |

## 5. DAG sketch (pipeline `viewer/full`)

```
pipeline-input → canonicalize-location → canonical-location
                                             │
    ┌───── fetch-ortho-rgb ───────────────────┤
    │      fetch-ortho-irc                    │
    │      fetch-elevation ──► compute-scene-origin
    │      fetch-ortho-surround               │
    │      fetch-elevation-surround           │
    │      fetch-photo-meta                   │
    │      fetch-cadastre-parcels             │
    │      fetch-cadastre-buildings           │
    │      find-copc-tiles ─► fetch-copc-tiles │
    │                              │           │
    │                              └─► laz-extract-disc ─► lidar-positions (+ cls/int/nr)
    │                                                          │
    │                                                          ├─► compute-normals ─► lidar-normals, lidar-spacing
    │                                                          └─► compute-ceiling-flags

    decode-ortho-coreg ── build-ortho-rgb-rasters ─┐
                                                    │
    lidar-positions + cls/int/nr ─► build-lidar-rasters ─► compute-ndsm ─► compute-building-mask
                                                    │                        │
    decode-ortho-coreg + ortho-rgb-rasters ─► build-ortho-irc-rasters        │
                                                    │                        │
                                                    └──► coregister-ortho-lidar ─► coreg-offset
                                                                                     │
    cadastre-buildings ─► rasterize-cadastre-edges ─► fit-cadastre-to-lidar ─► cadastre-fit

    decode-ortho-tile ─► build-tile-ndvi
                              │
    lidar-positions + lidar-normals ─► compute-normal-scatter + cell-min-z
                              │
    building-mask + cadastre-fit + cadastre-bldgs ─► compute-near-building-mask
                              │
                              └──► classify-vegetation ─► canopy-flags, grass-flags
                                         │
                                         └──► build-lidar-ground ─► lidar-ground-grid

    cadastre-parcels + elevation-f32 + lidar-ground-grid + cadastre-fit + scene-origin
                              └──► build-parcel-lines ─► parcel-lines

    ortho-rgb-rasters ─► detect-photo-shadows ─► find-best-sun-time ─► sun-time
                                                       │
                                                       └──► cast-shadows-from-dsm
                                                                   │
                                                                   └──► deshadow-ortho ─► ortho-rgb-corrected-jpeg
```

Every arrow above carries a content-addressed artifact. Anything not reachable from the client's subscription set is not executed (DAG walk is lazy).

## 6. Array artifacts + internal-loop fan-out

One new concept. Motivated by COPC (1–4 tiles per query); generalizes to any step whose input/output is semantically a set of items.

### 6.1 Concept

An **array artifact** is a v3 artifact whose content is an ordered, indexed collection of elements. Two subtypes:

- **Inline array** (`ARRAY_INLINE`): elements live inside the artifact's bytes. Typical payload: small JSON objects. Example: `copc-tile-urls` = `[{url, name}, {url, name}]` serialized as JSON (~200B for two tiles).
- **By-hash array** (`ARRAY_BY_HASH`): artifact's bytes are a 32-byte-per-element hash manifest `[N | hash_0 | hash_1 | ... ]`. Each element's bytes live elsewhere in the store under its own hash. Example: `copc-raw-tiles` = a 2-entry manifest pointing at two stored LAZ blobs.

### 6.2 Step surface

`step_read_t` and `step_write_t` gain an `array: bool` flag — declarative only, used for validation of the step's actual read / write calls.

Step context helpers for arrays:

```c
size_t step_array_len(step_context_t*, const char* name);

// Inline arrays: element bytes are inline in the artifact. Return a handle
// that exposes length + raw pointer to the element's bytes (or a typed
// helper for JSON object paths: step_array_read_json(ctx, name, i, "$.url")).
int step_array_read_bytes(step_context_t*, const char* name, size_t i,
                          const uint8_t** out_bytes, size_t* out_len);

// By-hash arrays: open the i-th element via store_lookup. Zero-copy mmap.
int step_array_open(step_context_t*, const char* name, size_t i,
                    store_handle_t* out);

// Write an inline array. `item_bytes` is a concatenation of N items; `stride`
// is 0 for variable-length items preceded by a length prefix per item.
int step_write_array_inline(step_context_t*, const char* name,
                            const uint8_t* item_bytes, size_t total_len,
                            size_t n, size_t stride);

// Write a by-hash array. Caller has already stored N blobs; just pass the
// hashes in canonical order. Runner writes the manifest.
int step_write_array_by_hash(step_context_t*, const char* name,
                             const hash_t* hashes, size_t n);
```

### 6.3 Cache semantics

- An array artifact's content hash is the hash of its own bytes. For `ARRAY_INLINE` that's the hash of the serialized items. For `ARRAY_BY_HASH` that's the hash of the manifest (which contains the element hashes); so two runs producing the same N elements in the same order produce the same manifest hash.
- A step reading an array artifact is identity-hashed with that array's manifest hash as one of its read hashes. Same manifest → same identity → cache hit.
- Elements themselves are independently content-addressed (by their own hash). Two different arrays that happen to share an element share the underlying store entry; the element is never duplicated.
- Ordering contract: the step that writes an array must emit elements in a deterministic canonical order (e.g. sort by URL string). Documented per-step in `meta.json`. Reorder-invariance is not a store-level property — it's a producer-contract property.

### 6.4 Inside-the-step loop pattern

Canonical shape of a fan-out step that loops internally:

```c
static int fetch_copc_tiles_exec(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "copc-tile-urls");
    hash_t hashes[MAX_TILES];
    for (size_t i = 0; i < n; i++) {
        const uint8_t* url_bytes; size_t url_len;
        step_array_read_bytes(ctx, "copc-tile-urls", i, &url_bytes, &url_len);
        const char* url = extract_url_field(url_bytes, url_len);

        // Self-cache by URL. fetch_and_cache() lives in the fetcher lib and
        // is the ONLY path used for HTTPS fetches.
        hashes[i] = fetch_and_cache(ctx->runner->store, url);
    }
    return step_write_array_by_hash(ctx, "copc-raw-tiles", hashes, n);
}
```

A reducer step (combining array elements into one output) looks like:

```c
static int laz_extract_disc_exec(step_context_t* ctx) {
    const char* loc_json;
    size_t loc_len;
    loc_json = step_read(ctx, "canonical-location", &loc_len);
    double cx, cy, radius; parse_location(loc_json, loc_len, &cx, &cy, &radius);

    size_t n = step_array_len(ctx, "copc-raw-tiles");
    point_buffer_t pts = {0};
    for (size_t i = 0; i < n; i++) {
        store_handle_t h;
        step_array_open(ctx, "copc-raw-tiles", i, &h);
        laz_decompress_into_disc(h.bytes, h.len, cx, cy, radius, &pts);
        store_release(ctx->runner->store, &h);
    }
    step_write_output(ctx, "lidar-positions", pts.xyz, pts.n * 3 * sizeof(double));
    step_write_output(ctx, "lidar-classifications", pts.cls, pts.n);
    step_write_output(ctx, "lidar-intensities", pts.intensity, pts.n * 2);
    step_write_output(ctx, "lidar-num-returns", pts.nr, pts.n);
    point_buffer_free(&pts);
    return 0;
}
```

### 6.5 What this costs the runner

Minimal. One task per step, always. Scheduling is unchanged. Changes:

1. Two new `artifact_kind_t` enum entries (`ARRAY_INLINE`, `ARRAY_BY_HASH`).
2. Four new `step_context_t` helpers (§6.2).
3. Declaration-flag validation in the registry (warn if a step's `array: true` read is satisfied by a non-array artifact, etc.).
4. No changes to the DAG walker, scheduling, identity hashing, cancel / reconnect flow, or WS protocol.

Logged in PLAN-RUNNER as task T13.

### 6.6 What this does NOT add

- No runner-level fan-out: a step is never instantiated N times by the runner. If a step needs internal parallelism, it uses libcurl multi-handle (I/O-bound) or borrows a thread pool from runtime (CPU-bound, future).
- No cross-product arrays, no array-of-arrays. If they're ever needed, add them strictly on top of this design.
- No progress per element — the step emits one `STEP_COMPLETE` at the end. Progress telemetry (if wanted for long fan-outs) lives in a future `PROGRESS` WS message.

## 7. Streaming delivery

The JS pipeline streams `POINTS_PREVIEW` chunks during COPC load for fast first pixel. v3 supports this as a proper framework primitive, not a retrofit. Landed in B9.

### 7.1 Semantics

A write declared `streaming: true` in its step def may be produced via `step_write_output_stream(ctx, name, bytes, len, final)`. The step may call this helper multiple times before returning; exactly one call must have `final=true`. Each call emits a `STREAM_CHUNK` WS message:

```jsonc
{ "t": "STREAM_CHUNK", "run_id": "...", "artifact": "lidar-positions",
  "seq": 3, "final": false, "size": 65536, "hash": null }
// followed by one binary WS frame of `size` bytes
```

The final chunk has `final: true` and carries the **finalized artifact hash** the client can use for subsequent subscriptions. The runner writes the complete artifact to the store atomically on `final: true`.

Non-streaming writes (`step_write_output` / `_owned` / `_array_*`) are unaffected. Mixing streaming and non-streaming helpers for the same artifact in one exec is rejected with an error.

### 7.2 Semantics inside the runner

- **Accumulator location**: in-process, inside `step_context`. One growing heap buffer per streaming write (64 KiB initial capacity, geometric realloc). No temp file, no store-side append helper — the runner calls the existing `store_put` once, at finalisation, passing the concatenated buffer via ownership-transfer (same path as `step_write_output_owned`). Rationale: the streams we currently produce (≤100k points × 24 bytes = ≤2.4 MB) fit trivially in RAM, and an in-process buffer avoids introducing fsync-per-chunk syscall churn into the store. Future very-large streams (hundreds of MB+) would need the temp-file variant — tracked in PLAN-STORE S11.
- **Atomicity**: because `store_put` runs exactly once at `final=true`, there is no partial entry visible to any reader at any time. Mid-stream cancel or step error drops the accumulator in the body-trampoline cleanup loop; no `store_put` call, no intent-map entry. Matches the §9 atomicity model by construction.
- **Cache integration**: the artifact's content hash is `hash_bytes(concatenated-buffer)` — identical to the hash a non-streaming write of the same bytes would produce. Under the unified hash model, the streaming sequence is one `cached_compute` invocation whose body-fn accumulates bytes and produces a single result on `final=true`. There is no "skip while streaming" — the intent-map entry lands only on full-stream success. A second identical run is a full cache hit: the step's `exec` is not called, no `on_stream_chunk` fires, the stored artifact is re-delivered via the regular `ARTIFACT_HEADER` path (§7.3).
- **Downstream visibility**: downstream `step_read` blocks until `final=true`; partial streams are invisible to the DAG. Streaming is a transport concern, not a DAG concern.

### 7.3 Dual WS delivery

When a streaming artifact finalises, the server emits **both** the final `STREAM_CHUNK` (with hash) *and* a regular `ARTIFACT_HEADER` + binary frame carrying the full bytes. The sequence is:

```
STREAM_CHUNK final:false ×N  →  STREAM_CHUNK final:true  →  ARTIFACT_HEADER + binary  →  STEP_COMPLETE
```

This is intentional. It means:

- Clients that understand streaming consume the `STREAM_CHUNK` pairs for low-latency rendering and ignore the trailing `ARTIFACT_HEADER` (they already have the bytes).
- Clients that don't understand streaming (older builds, simple subscribers, the batch-mode callback) receive the artifact through the standard `ARTIFACT_HEADER` path with no special-casing.
- Late-subscribers (reconnect or a post-hoc SUBSCRIBE) always retrieve the artifact via `ARTIFACT_HEADER` from the store — they never see in-flight chunks.

The duplication costs one extra copy of the finalised bytes on the wire. For our LiDAR-positions stream that's ~2 MB; acceptable for the delivery-simplicity win. If it ever matters, a client could opt out by passing a session flag (not currently implemented).

### 7.4 Batch mode behaviour

In batch mode the `on_stream_chunk` callback pointer is `NULL`, so the runner's chunk-emission code path is skipped entirely. The step still streams internally (the accumulator still grows chunk-by-chunk); no per-chunk side effects fire. On `final=true` the artifact lands in the store atomically and `batch_on_artifact_ready` dumps it to the output directory exactly as it would for `step_write_output_owned`. Batch mode pays zero protocol overhead for streaming and gets a bit-identical final artifact.

### 7.5 Implementation scope (landed in B9)

- Runner: `step_write_output_stream()` + in-process accumulator (PLAN-RUNNER T14 ✅).
- Store: **no change** — the existing `store_put` is called once at finalisation. PLAN-STORE S11 (temp-file append helper) is deferred until a stream outgrows RAM.
- Protocol: `STREAM_CHUNK` added to the WS text-message list (PLAN-PROTOCOL P10 ✅). Schema finalised in UNIFIED-ARCHITECTURE.md §8.1.
- First consumer: `laz-extract-disc` streams `lidar-positions` in CHUNK_POINTS=20000-point (480 KB) chunks; the three ancillary columns (classifications, intensities, num-returns) stay non-streaming.
- Test: `serve_stream.py` drives a 5-chunk dummy (`stream-demo` step), asserts chunk-by-chunk delivery, deterministic byte pattern, `STREAM_CHUNK final.hash == ARTIFACT_HEADER.hash` (the runner hashes with xxh3-128 zero-padded — same algorithm as every other `hash_t` on the wire — so the protocol-level equality check gives the same guarantee as recomputing client-side), and first-chunk-within-500ms. Measured ~0.2 ms RUN_STARTED → first chunk on the bench.

## 8. External C library dependencies

| Need | Lib | Notes |
|---|---|---|
| HTTPS fetches | **libcurl** | A9. PLAN-FETCHERS decision 1. |
| TLS (outbound) | **mbedTLS** | §D3 / PLAN-FETCHERS decision 2. |
| JSON | **jansson** | Already linked. |
| JPEG encode/decode | **libjpeg-turbo** | Vendored. ~500 KB static. |
| BIL / raw Float32 | — | No lib; `memcpy` on returned bytes. |
| LAZ decompression | **laz-perf** ([hobu/laz-perf](https://github.com/hobu/laz-perf)) | Ready C++11 library, CMake, widely used (entwine, untwine, PDAL). Vendor as submodule; link as C++ object, expose through a thin `.h` wrapper. Integration, not porting. |
| COPC hierarchy parsing | **copc-lib** ([rockrobotic/copc-lib](https://github.com/rockrobotic/copc-lib)) | Ready C++17 library. Same vendor pattern as laz-perf. If it turns out to miss something, the COPC binary format is documented — a from-scratch parser is ~300 LOC. |
| WGS84 ↔ Lambert 93 | **PROJ 9.x** | MAIN-IMPL-PLAN decision. |
| Sun position | ~50 LOC port of SunCalc | No library. |
| Sobel / morphology / NDVI / PCA / kd-tree | Pure C | Inline. |

### Risk items

- **Integration of laz-perf + copc-lib into the CMake build.** Both are C++ libs; we link them via C-wrapper translation units. Common pattern, day of work, not a blocker.
- **libjpeg-turbo** vendoring: trivial.
- **PROJ** adds a heavier dependency (~10 MB static). Alternative: inline closed-form Lambert-93 (~50 LOC). If static binary size becomes a concern we can swap; for now PROJ gives us the full family of projections we'll eventually need.

## 9. Artifact kinds needed

Extend the `artifact_kind_t` enum (§5 of the architecture doc):

| Kind | Bytes | Notes |
|---|---|---|
| `ARRAY_INLINE` | header + concatenated items | Items may be JSON objects (variable-length with per-item length prefix) or fixed-stride records. Used for `copc-tile-urls` and any "list of small values" output. |
| `ARRAY_BY_HASH` | header + N + N × 32-byte hashes | Manifest. Used for `copc-raw-tiles` and any "list of large blobs" output. |
| `POINT_COLUMN` | one column per artifact (F32 / F64 / U8 / U16) + metadata (count, bbox) | Replaces the JS `POINTS_FINAL` packed struct. §4.9. |
| `RASTER_F32` | Float32 grid | metadata: cols, rows, bbox, cellSize. Serves elevation, NDSM, scatter, NDVI. |
| `RASTER_U8` | Uint8 grid | metadata: cols, rows, bbox, cellSize. Serves masks, shadow masks. |
| `IMAGE_JPEG` | compressed bytes | metadata: width, height, bboxL93. Pass-through payload. |
| `IMAGE_RGBA` | uncompressed pixels | metadata: width, height, flipped flag. |
| `GEOJSON` | UTF-8 JSON | metadata: feature count. Pass-through from WFS. |
| `LINE_SEGMENTS_F32` | F32, 6 values per segment | parcels. |
| `METADATA_JSON` | small UTF-8 JSON | scene-origin, coreg-offset, sun-time, photo-meta, canonical-location, etc. |
| `COPC_LAZ_RAW` | raw LAZ bytes | fetcher boundary. |

## 10. Caching model — content-hash everywhere

*(Previous revisions of this section contained a per-step output-hash decision table choosing between `derivation` and `content`. That dichotomy was retired on 2026-04-21 (see UNIFIED §15 N9) in favour of a unified model where **all step outputs are content-hashed** and cache-skip is handled by the intent map. See [`HASH-MODEL.md`](../HASH-MODEL.md) for the full model.)*

Under the unified model:

- Every step write is content-hashed. No `output_hash` field in `step_def_t`.
- Cache-skipping on exact-identity reruns happens via the intent map (`step_identity → content_hash`). Funnel convergence (different inputs → same output bytes) is automatic because content-hashing collapses identical bytes onto one store key.
- The per-step choice has collapsed. No table. The step author just writes the exec function.
- **Escape hatch**: a write with `skip_on_identity: false` forces re-execution on every invocation. Reserved for non-deterministic steps (wall-clock or RNG dependency). None of the viewer-port steps need this.

### Funnel steps in this pipeline

Not for the step author to choose, but useful to keep in mind as the points where upstream chains converge:

- `canonicalize-location` — different raw coords that quantize to the same 20m cell.
- `find-copc-tiles` — different canonical-locations that pick the same IGN tile set.
- `fetch-copc-tiles` — different upstream manifest representations encoding the same URL set.
- `laz-extract-disc` — different tile sets that happen to contain the same points in the disc.

All four produce byte-identical outputs from different upstream identities — the content-hash model shares them automatically.

### Param stability

Changing a step's params produces a different step identity hash → different intent-map entry → re-execution. That's the intended invalidation: tuning `NDVI_THRESHOLD` in one pipeline invalidates only that pipeline's downstream cache, not other pipelines using `classify-vegetation` with different params. **Step `version` bumps only on algorithmic change.** Param tuning is not a version bump.

## 11. Port order (Phase B milestones)

**B1 — Fetcher foundations (A9 deliverables part 1).** ✅ **Landed** (commit `dff4fef`). libcurl + mbedTLS wrapper; `fetch_and_cache()` helper; retry/backoff; per-host concurrency limiter (token-bucket deferred per PLAN-FETCHERS F9). Shipped `fetch-ortho-rgb`, `fetch-ortho-irc`, `fetch-elevation`, `fetch-photo-meta`, `fetch-cadastre-parcels`, `fetch-cadastre-buildings`. Validation: smoke pipeline downloads ortho for a known lat/lon; content-hash matches a committed golden.

**B2 — Canonicalization + coord step.** ✅ **Landed** (commit `feb553e`). Ship `canonicalize-location` and `compute-scene-origin`. PROJ vendored. Validation: given two nearby coords, both produce the same canonical-location; scene-origin roundtrips through PROJ correctly.

**B3 — Array artifacts (T13) + COPC end-to-end.** ✅ **Landed** (commits `f7afe5d`, `cc19d2a`). Runner: `ARRAY_INLINE`, `ARRAY_BY_HASH` kinds + four context helpers. Library: vendor laz-perf + copc-lib. Steps: `find-copc-tiles`, `fetch-copc-tiles`, `laz-extract-disc`. Validation: same lat/lon → disc point count within JS reference; byte-identical where LAZ decodes deterministically.

**B3.5 — Hash-model refactor (HASH-MODEL + PLAN-RUNNER T15/T16 + PLAN-STORE S12–S15).** ✅ **Landed** (commit `f359b50`). Architectural prerequisite for B4+. Unifies the cache model: all outputs content-hashed, intent map for cache-skip, `cached_compute` primitive, hot-tier zero-copy store. One-time coordinated refactor across runner + store + step API. No new user-facing behaviour on day one, but subsequent milestones benefit automatically. Validation: existing `batch_array_demo` + `batch_viewer_pointcloud` ctests remain green; new integration test verifies second-run of `batch_array_demo` produces zero `body_fn` executions (full cache hit via intent map).

**B4 — First real algo: `compute-normals`.** ✅ **Landed** (commit `3df9524`). Kd-tree + PCA. Validation: ≥99.99% of points' normals within 1e-3 of JS reference. **Checkpoint**: does the step-authoring flow work? Does `meta.json` capture what we need? Does caching hold for F32 outputs produced from real point data?

**B5 — Registration chain.** ✅ **Landed** (commits `6154927`, `0f56d33`, `d5602d7`, `e285929`). `build-lidar-rasters` → `compute-ndsm` → `compute-building-mask` → `build-ortho-{rgb,irc}-rasters` → `coregister-ortho-lidar` → cadastre fit. Validation: coreg-offset within 5 cm of JS reference on a test grid of 20 coords.

**B6 — Classification.** ✅ **Landed** (commit `5e20794`). `build-tile-ndvi` → `compute-normal-scatter` → `compute-near-building-mask` → `classify-vegetation`. Validation: canopy/grass flags ≥99.9% match vs. JS reference.

**B7 — Ground + parcels.** ✅ **Landed** (commit `4efbd45`). `build-lidar-ground`, `build-parcel-lines`. Light ports. **Post-ship (commit `72abaf4`)**: `build-lidar-ground` v1→v2 — switched the per-cell min-Z filter from `canopy-flags` (tall-canopy detector only) to ASPRS class-2 (rejects low/medium veg, buildings, winter trees and noise that the canopy classifier missed). Eliminates BFS-fill dent under dense forest. `build-parcel-lines` default `height_offset_m` 0.3 → 1.5 m (fence-height read instead of ground-flush ribbon).

**B8 — Shadow + deshadow.** ✅ **Landed** (commits `e5697d2`, `b59f829`). `detect-photo-shadows` → `find-best-sun-time` → `cast-shadows-from-dsm` → `deshadow-ortho`. Candidate for first real GPU port (§7.6 canary pattern applied to `find-best-sun-time`) — GPU port deferred (no GPU runner). Probably the longest single milestone.

**B9 — Streaming retrofit (T14).** ✅ **Landed** (commit `bd10425`). Runner `step_write_output_stream` + in-process accumulator, WS `STREAM_CHUNK`. `laz-extract-disc` streams positions in 20k-point chunks. Validation: first chunk arrives ~0.2 ms after RUN_STARTED (far inside the 500 ms budget). No store-side append helper was needed; the in-process accumulator covers all current streams (see §7.2). PLAN-STORE S11 deferred until a future stream outgrows RAM.

**B10 — Surround + remaining fetchers.** ✅ **Landed** (commit `762ece2`). `fetch-ortho-surround`, `fetch-elevation-surround`. Integration + test that subscription-gating correctly prunes surround from runs that don't subscribe to it.

Each milestone is a shippable increment: after B4 the binary can compute normals for a real lat/lon on demand; after B5 it can coregister; etc. The JS viewer keeps running alongside until B8 ships a functional equivalent.

## 12. Open questions

1. **Radius canonicalization buckets.** `radius` is continuous in the client's request but we need discrete values (e.g. 50 / 75 / 100 / 125 m) for cache sharing. Pick a small fixed set at `canonicalize-location` time; document in `meta.json`. Decision deferred to B2 but the design doesn't depend on it.
2. **IGN region coverage.** COPC tiles only exist for France. The fetcher errors out clearly on `tiles.length == 0`; no fallback in Phase B (same as the JS code).
3. **JPEG vs PNG for deshadowed ortho.** The JS code re-encodes to JPEG (~lossy). v3 could emit PNG for perfect roundtrips at the cost of 3-5× bytes. Lean: stay with JPEG; optimise the quality setting.
4. **How large is the `find-best-sun-time` search cost under real inputs on CPU?** If > a few seconds we might want GPU earlier (B7 instead of B10 equivalent). Measure in B5 once the lidar rasters exist.

## 13. What this plan does NOT commit to

- A runner-level fan-out primitive (N-instance scheduling). The internal-loop pattern in §6 covers every concrete case in this pipeline.
- Cross-product array inputs (step with 2 array reads → N×M loop). If ever needed, added on top.
- The frontend SDK port. SDK stays on the existing JS path + new WS protocol; it cuts over to v3 outputs once message shapes match.
- The control-plane + usage billing (A10). Out of scope for Phase B.
- A GPU port for any step in B1–B7. `gpu-smoke` keeps the wiring alive; first real GPU port in B8.
- Backwards-compatibility with the legacy JS message formats on the wire. v3 uses §8.1's message schema; the SDK adapts.
