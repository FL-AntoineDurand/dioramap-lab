# PLAN — `lidar-positions` wire format (F64 absolute → F32 scene-local)

Status: **draft, not committed to a path 2026-05-07**. Captures three options with their tradeoffs so we can choose later when we have measurements (or when bandwidth bites). Companion to `PLAN-VIEWER-PORT.md` §7.3 (streaming) and `PLAN-VIEWER-SDK-PORT.md` §5.4 (subscribe list).

Scope: the wire format of `lidar-positions` only. The artifact is the dominant payload to the SDK on cold runs (~24 MB per disc at 1 M points × 3 × F64); halving it moves real wall-clock time on customer connections. Out of scope: other artifacts (rasters, point columns < 4 MB), F32 quantization on raster artifacts, and the GPU upload path (already F32 today, won't change).

---

## 1. Today's contract

`steps/copc/laz-extract-disc.c:4`:

```
// Writes: lidar-positions — F64, interleaved (x,y,z) in L93 metres
```

Streamed in 20 k-point chunks (`step_write_output_stream`) plus a trailing full `ARTIFACT_HEADER` (dual-delivery, `PLAN-VIEWER-PORT §7.3`). Decoded on the SDK as `Float64Array`. The viz then narrows to `Float32` at GPU upload time, after subtracting `scene-origin` to bring values into the [-100m, +100m] range where Float32 has sub-mm precision.

Why F64-absolute on the wire today:
- Lambert-93 at France magnitudes (~7 M north) needs F64 to keep sub-cm precision at any coordinate.
- Server-side consumers (`compute-normals`, `build-lidar-rasters`, `compute-ndsm`, `build-lidar-ground`, `classify-vegetation`, `build-parcel-lines`, `coregister-ortho-lidar`) all do geographic math directly on the L93 numbers — they need a stable absolute frame.
- The L93 representation is stable across users / sessions / pipeline reconfigurations: one cache key per `(location, radius, step version)`, hits across embedders.

Cost: 24 MB per disc on the wire, decoded twice on the client (once into F64 array, once narrowed to F32 GPU buffer).

---

## 2. Three options

### Option A — Additive new artifact `lidar-positions-wire`

A tiny new server step `compute-positions-wire` reads `lidar-positions` (F64 absolute) + `scene-origin` and emits `lidar-positions-wire` (F32, scene-local). The SDK subscribes to `lidar-positions-wire` instead of `lidar-positions`. Server-side consumers are unchanged — they keep reading the F64 absolute artifact.

**Wire**: 24 MB → 12 MB (-50% on the dominant payload).

**Cache**: +12 MB per disc (both formats coexist; F64 is source-of-truth for downstream, F32 is wire-only). Cache footprint per disc 24 MB → 36 MB (+50%).

**Server work**: one new step (~80 LOC). Reads F64 absolute, subtracts scene-origin, writes F32. Streaming-capable (consumes F64 stream as it arrives, emits F32 stream).

**Streaming first-chunk timing**: new step depends on `scene-origin`. Scene-origin requires `canonical-location` (instant) + `elevation-f32` (small HTTP fetch from IGN BIL, fast). In practice scene-origin lands well before COPC streaming begins; first F32 chunk emit lags by 0 ms most of the time. Worst-case race-lost: a few hundred ms of buffered F64 chunks waiting for scene-origin.

**Client change**: one line in `viewer-sdk/src/viewer.js`'s `SUBSCRIBE_LIST`. Decoder kind switches from `AK_POINT_COLUMN` (F64×3) to a new `AK_POINT_COLUMN_LOCAL_F32` (or reuse with a flag). Viz drops the F64→F32 narrowing pass; uploads directly.

**Risk**: low. Additive, fully backward-compatible. Old-format consumers unaffected. Decision can be reverted by changing the SDK subscribe back.

**Effort**: ~1-2 days (step + decode + SDK + tests).

### Option B — Replace the wire format

`laz-extract-disc` itself emits F32 + baseline. The artifact contract changes from F64 absolute to F32 scene-local with a per-stream baseline carried in the artifact header. Every server-side consumer that reads positions is updated to do `(x + baseline.x)` etc. when it needs absolute coords.

**Wire**: 24 MB → 12 MB (-50%).

**Cache**: 24 MB → 12 MB (-50%) — single artifact, no parallel F64 copy.

**Baseline**: use `canonical-location.center_l93_{x,y}` (already a dep of `laz-extract-disc`, no new dep, no serialisation hit). Z baseline = 0 or per-stream median; either way F32 within ±200 m of baseline retains ~µm precision. **Key**: do NOT use `scene-origin` (which requires the elevation fetch); use `canonical-location.center` for streaming-time independence.

**Server consumer changes**: ~6-7 steps need a small adapter to add baseline back when they touch absolute L93. List:
- `compute-normals` — PCA in local neighborhoods, no absolute coords needed at all (no change).
- `build-lidar-rasters`, `build-lidar-ground` — bin into raster cells using `(point + baseline − bbox.min) / cell_size`. ~5 LOC each.
- `compute-normal-scatter` — same (cell binning).
- `classify-vegetation` — same.
- `build-parcel-lines` — touches cadastre polygons in absolute L93. Adds baseline back at the comparison boundary. ~10 LOC.
- `coregister-ortho-lidar` — works on already-built rasters, doesn't see raw positions (no change).

Total ~30-50 LOC across server-side steps, all mechanical.

**Streaming first-chunk timing**: identical to today. `laz-extract-disc` doesn't gain a dep.

**Risk**: medium. Artifact contract change. Cached `lidar-positions` artifacts from before the change become unreachable (one-time cache invalidation; small store today, ~110 MB). Step versions cascade-bump for every consumer of `lidar-positions`. Tests need an epsilon retune (F32 vs F64 differs by µm; most tolerances don't notice but `compute-normals` precision tests may need a tweak).

**Effort**: ~3-5 days (~6 step adapters + decoder + SDK + test retune + cache flush in dev/prod).

### Option C — Quantize further (int16 cm-deltas)

Wire format becomes baseline (F32×3) + int16×3 per point, where each int16 represents `(coord − baseline) × 100` in cm. ±32 768 cm = ±327 m, fine for a 75-m disc.

**Wire**: 24 MB → 6 MB per disc (-75%).

**Cache**: similar saving if applied as Option A's variant or replacement.

**Precision**: 1 cm. Below human-perceivable rendering accuracy. Below typical lidar measurement noise (~5 cm). Server-side analyses run on F64 (cached upstream of any int16 lossy stage), so consumers untouched.

**Risk**: medium-high. Lossy quantisation is a one-way trip; if any future consumer ever needs <1 cm precision over the wire, this format can't deliver it. Architectural lock-in concern.

**Effort**: similar to A or B + the decode/encode pair on either side (~50 LOC).

---

## 3. Tradeoff comparison

| | Today | A (additive) | B (replace) | C (int16) |
|---|---|---|---|---|
| Wire bytes / disc | 24 MB | 12 MB | 12 MB | 6 MB |
| Cache bytes / disc | 24 MB | 36 MB (+50%) | 12 MB (-50%) | 6 MB (-75%) |
| Server consumer changes | — | none | ~6 steps × ~5 LOC | none (if A-shape) |
| New server step | — | yes (~80 LOC) | no | yes-ish |
| Artifact contract change | — | additive | breaking (cache invalidate) | additive |
| Step version cascade | — | one new step | every position-consumer | one new step |
| Streaming first-chunk delay | — | 0 ms typical | 0 ms always | 0 ms typical |
| Precision floor | mm-class | µm | µm | 1 cm |
| Reversibility | — | revert SDK subscribe | requires reverse migration | requires reverse migration |
| Engineering effort | — | 1-2 days | 3-5 days | 2-3 days |

---

## 4. When does this matter

The win is only meaningful if:

1. **Cold-cache fresh runs are the dominant flow.** If most users hit cached scenes, the bandwidth savings only land on the first visit per location-and-radius bucket. Worth knowing how cold/warm the customer-side hit ratio is — measure on agri-map prod traffic before committing.
2. **Customer connections are constrained.** Mobile users on 4G, rural ADSL, etc. — they pay 24 MB per cold load today. A 12 MB save shaves a meaningful slice of cold-load wall time. On a 1 Mbps mobile, that's ~96 s → ~48 s; on a 10 Mbps ADSL, 19 s → 9.5 s. On office fibre (50+ Mbps) the save is in the noise.
3. **Cache pressure isn't currently fine.** Today's dev cache: ~110 MB across 76 entries. A 50 % cache cost increase from Option A would push it to ~160 MB. Operationally invisible. If we ever hit a cache-budget ceiling on a production processor (per `PLAN-STORE.md`), Option B's smaller footprint becomes a real input.

Without these signals, this is a "nice optimisation we can do whenever" rather than a "now or never." Commit to a measurement step before commit to an implementation step.

---

## 5. Cross-cutting concerns (apply to whichever option)

### Artifact decoder

Today's `apps/protocol/src/artifact-decoder.js:203` maps `f64` to `Float64Array`. Adding a new local-F32+baseline kind needs:
- A new decoder branch reading the `{baseline_x, baseline_y, baseline_z}` from the artifact header (or from an inline 12-byte prefix on the bytes).
- The decoded payload exposes `data: Float32Array` plus `baseline: {x, y, z}` for consumers that need to recover absolute coords.

For Option B, the existing `lidar-positions` decoder branch is updated in place; no new kind is added. For Option A, a new artifact name + kind keeps both shapes alive simultaneously.

### Cache invalidation

- Option A: zero. New artifact name → fresh cache namespace.
- Option B: every `lidar-positions` cache entry is unreachable after the step version bump. Dev cache flush on deploy. In production this is a one-time cost — the next request rebuilds.
- Option C: same as A or B depending on which shape it follows.

### Streaming precision concern

When chunks arrive, the SDK builds a Float32 buffer for the GPU. Today's flow: F64 chunks → F64-arithmetic subtract `scene-origin` → narrow to F32. New flow: F32 chunks (already centered) → upload. The intermediate F64-arithmetic subtract is what preserves precision today; baking F32 already-centered means the precision was preserved at emit time on the server in F64-arithmetic before the F32 narrowing. Same precision contract, just one step earlier in the pipeline. Verified math: F32 ε at magnitude 75 ≈ 4 × 10⁻⁶ → 4 µm, well below relevant tolerances.

### Precision audit before committing to B

Before adopting Option B, run an empirical check: load a test disc, recenter+narrow to F32, compute normals, compare to F64 reference within `compute-normals`'s existing tolerance (currently 1e-3 in the validation harness, per `PLAN-VIEWER-PORT.md` §11 B4). Expectation: the F64 → F32 quantisation introduces error well below 1e-3. If it doesn't, the test tolerance was too tight or the F32 path needs more care (e.g. compute normals in F64 against F32+baseline points before narrowing).

---

## 6. Recommendation matrix

| If the problem is | Pick |
|---|---|
| "We want this clean architecturally and have a multi-day window" | B |
| "We just want to ship the wire saving without touching server consumers" | A |
| "Cold-cache wall time on mobile is the user complaint, every byte counts" | C |
| "Bandwidth is fine for now, but we should plan for the future" | this doc, no commit |

The default is **no commit yet**. Measure cold/warm hit ratio in prod and cold-load wall-clock on a typical mobile + a typical fibre customer. If those numbers point at "bandwidth matters," return to this doc and pick A or B based on appetite for the consumer-side refactor.

---

## 7. Open questions

1. **Streaming + baseline emission ordering.** If `laz-extract-disc` emits F32+baseline (Option B), does the baseline ride in every chunk header, or in a one-shot `STREAM_START` frame before the first chunk? `STREAM_START` is cleaner but requires a protocol addition (today's protocol has only `STREAM_CHUNK`). Inline-in-every-chunk wastes 24 bytes × N-chunks (negligible, ~50 chunks × 24 = 1.2 KB).
2. **Z baseline policy.** Three candidates: (a) use 0 (simplest, F32 still has plenty of precision over France's altitude range); (b) use `canonical-location` z if it has one (it doesn't today — only x, y, radius); (c) use the per-stream median Z (compute one-pass during emit). Recommend (a) for simplicity; (c) only if the typical altitude is far from 0 and we measure that F32 precision is starting to bite (it isn't).
3. **What does Option A's new step output as `kind`?** Reuse `AK_POINT_COLUMN` with a precision flag, or introduce `AK_POINT_COLUMN_LOCAL_F32`? Slight preference for the latter — keeps existing F64 column decoders strict, makes the `kind` self-documenting.
4. **Lab-ui's composite dispatcher.** Same change-set as the SDK orchestration if Option A is taken (new artifact name, new decoder branch). Should land in the same PR as the SDK change.
5. **Tests.** Today's `core/tests/` mostly exercise step output structure, not numerical comparison against reference. Need to add a `lidar-positions-wire` golden test for whichever option ships, comparing local-F32+baseline reconstruction against the absolute F64 reference within ε.

---

## 8. What this plan does NOT commit to

- **Any of the three options.** This doc captures the design space; the decision is deferred until there's traffic data.
- **A measurement methodology beyond "look at customer connection traces."** Specific metrics + thresholds get scoped when we decide to pull the trigger.
- **Changes to other artifacts.** `lidar-normals` (~12 MB, F32), `lidar-classifications` (~1 MB, U8), rasters (each < 4 MB) are out of scope for this plan. If they ever become the bandwidth tail, separate analysis.
- **Quantising raster artifacts.** Different problem (precision-vs-bandwidth on regular grids); deferred entirely.
- **A protocol-level streaming change** (e.g. adding `STREAM_START` for shipping baseline once per stream). If we take Option B and decide the per-chunk baseline overhead matters, that's a follow-on plan.

---

## 9. References

- `steps/copc/laz-extract-disc.c` — current emitter, F64 absolute.
- `apps/scene-base/src/viz/lidar-positions.viz.js` — recenter+narrow at GPU upload.
- `apps/protocol/src/artifact-decoder.js:203` — F64 decoder branch.
- `apps/scene-base/src/viz/point-cloud-discs.viz.js`, `canopy.viz.js` — consumers that read `decoded[scene-origin]` and recenter independently of the streaming preview.
- `PLAN-VIEWER-PORT.md` §7.3 — dual-delivery streaming protocol.
- `apps/viewer-sdk/CHANGELOG.md` 0.8.1 — fresh-run streaming bug fix that revealed how brittle the current preview-recentering path is.
