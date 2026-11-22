# PHASE-5-V7-PORT.md

**Status**: boundary chain ported through `v7-wall-detect`; `5.7g` (boundary-regularize) remains as the terminal port. Last updated 2026-05-26.

**Scope**: port the **useful path** of bench-v2's V7 pipeline to v3, step
by step, with output validated against v2 references at every stage.
This is Phase 5 of the umbrella roof-recon evaluation tracked in
[`PLAN-THIRD-PARTY-RESEARCH.md`](PLAN-THIRD-PARTY-RESEARCH.md), and
unlike the third-party tracks (PolyFit, KSR, Roofer) it ports
*our own* algorithms — so the goal is faithful re-implementation,
not just wrapping a vendored library.

## V2 V7 pipeline, at a glance

V7 is bench-v2's most-evolved roof-reconstruction pipeline. Definition
in `lidar-algos-bench-v2/lib/pipeline/pipelines.js` lines 1079–1141.
33 steps total, of which **7 are diagnostic dead-ends** (not consumed
downstream) and **26 are on the data path**.

### Main path (data-bearing — port targets)

Topological order, with v2 source paths and a one-line role:

| # | v2 step | source | role |
|---|---|---|---|
| 1  | `tile-signals` | `lib/pipeline/steps/tile-signals.js` | unified KD-tree / PCA / normals / curvatures / density / ts-weights |
| 2  | `hemisphere-normals-density-aware-segment` | `lib/pipeline/steps/hemisphere-normals-density-aware-segment.js` | Lambert equal-area normal histogram → cascade segments + ridge candidates |
| 3  | `normal-cascade-seed` | `lib/pipeline/steps/normal-cascade-seed.js` | seed planes from cascade segments |
| 4  | `density-voids-2` | `lib/pipeline/steps/density-voids-2.js` | XZ-density-void ghost-plane detector (experimental, dahouet slate-dropout) |
| 5  | `grow-refit-merge` | `lib/pipeline/steps/grow-refit-merge.js` | iterative grow / refit / merge — primary segmentation engine |
| 6  | `planes-from-owner` | `lib/pipeline/steps/planes-from-owner.js` | extract per-plane hulls + plane equations from per-point owner |
| 7  | `roof-perimeter` | `lib/pipeline/steps/roof-perimeter.js` | eave / dormer perimeter edge list |
| 8  | `adjacency-edges` | `lib/pipeline/steps/adjacency-edges.js` | plane-adjacency graph + ridge edges + ridge corners |
| 9  | `detect-boundary` | `lib/pipeline/steps/detect-boundary.js` | per-point boundary candidate bits (gap / multi-return / height / eave / grow) |
| 10 | `boundary-signals` | `lib/pipeline/steps/boundary-signals.js` | roof-graph build, boundary points, balance fields |
| 11 | `boundary-corners` | `lib/pipeline/steps/boundary-corners.js` | corner pool from roof-graph |
| 12 | `boundary-polygons` | `lib/pipeline/steps/boundary-polygons.js` | per-plane polygon cycles |
| 13 | `boundary-merge` | `lib/pipeline/steps/boundary-merge.js` | cross-plane edge merge |
| 14 | `boundary-classify` | `lib/pipeline/steps/boundary-classify.js` | edge classification (shared / overhang / perimeter / wall) |
| 15 | `boundary-regularize` | `lib/pipeline/steps/boundary-regularize.js` | greedy coordinate-descent corner nudging |
| 16 | `wall-roof-detect` | `lib/pipeline/steps/wall-roof-detect.js` | vertical wall + footprint XZ edges from height-gap / multi-return |
| 17 | `ridge-clip` | `lib/pipeline/steps/ridge-clip.js` | clip planes to ridges, emit ridge edges |
| 18 | `combine-edges` | `lib/pipeline/steps/combine-edges.js` | unify ridge / wall / boundary edges with source tags |
| 19 | `snap-edges` | `lib/pipeline/steps/snap-edges.js` | dedupe / snap / close gaps |
| 20 | `compute-wall-edges` | `lib/pipeline/steps/compute-wall-edges.js` | vertical edge segments at corners |
| 21 | `close-corner-gaps` | `lib/pipeline/steps/close-corner-gaps.js` | reproject planes / edges to close corner gaps |
| 22 | `clip-to-footprint` | `lib/pipeline/steps/clip-to-footprint.js` | clip planes to the cadastre polygon |
| 23 | `obstacle-filter` | `lib/pipeline/steps/obstacle-filter.js` | dissolve micro-planes / obstacles |
| 24 | `resolve-overlaps` | `lib/pipeline/steps/resolve-overlaps.js` | height-aware plane overlap resolution |
| 25 | `shape-fitting` | `lib/pipeline/steps/shape-fitting.js` | ridge-constrained plane regularization (final) |
| 26 | `lod2-simplify` | `lib/pipeline/steps/lod2-simplify.js` | LoD2 output (roof-only, no obstacles) |

### Diagnostic-only (skip on first port; revisit if useful)

`beam-incidence`, `y-histogram`, `ransac-full` (disabled), `planarity-watershed` (disabled), `rasterize-footprint`, `competitive-grow`, `point-stealing`. All produce viz only — no downstream consumer.

## Strategy

### How "identical results" is interpreted here

V2 is JavaScript; v3 is C/C++. Bit-identical output is realistically
impossible (JS `Math.*` vs `libm`, KD-tree node-allocation order, etc.)
but **numerically equivalent within tolerance** is the bar — same
algorithmic outcome on the same input, with float diffs at the
expected-rounding-error scale.

Per-step validation strategy:

1. Set up a small **reference-dump CLI** in bench-v2 that runs V7 on a
   chosen test case and writes each step's outputs to a stable JSON+
   binary artifact format under `bench-v2/.refs/<step>/<case>/`.
2. The v3 step's smoke test loads the reference, runs v3, compares
   per-array float arrays with `||a-b||∞ < eps_step` (per-step
   tolerance — KD-tree-derived signals get a higher eps).
3. Failures are diff'd field by field; the v3 step iterates until each
   array is within tolerance.

### Reference dump — which test case (bldgonly is the default)

**As of 2026-05-18**, the canonical bench-v2 dump for `dahouet` is the
**bldgonly** fixture, not the full tile:

| Dump | Input | Why |
|---|---|---|
| **`/tmp/v7-refs-bldgonly-fresh/`** | `real-22186-dahouet-502-bldgonly.json` (16092 pts) | **Primary v2 reference for the v3 audit.** Same physical points as v3 prod's per-bldg `extract-buildings` output → any v3-vs-v2 divergence isolates to algo / FP / frame, not "v2 saw different points". |
| `/tmp/v7-refs-fresh/` (legacy) | `real-22186-dahouet-502.json` (full 45696-pt tile) | Used for the original "v3 algo tile-wide ports faithfully" gate (commit `0f9ddf9`). Still valid as a regression net; the v3 baselines at `v3/tests/baselines/v7-*/dahouet/` were generated against this dump. |

**v3 baselines now exist in two flavours** (additive — keep both):
- `v3/tests/baselines/v7-*/dahouet/` — v3 bench against full-tile refs
- `v3/tests/baselines/v7-*/dahouet-bldgonly/` — v3 bench against bldgonly refs (NEW 2026-05-18)

When a future port lands or a bench comparison is needed, **default to
bldgonly** unless the work specifically targets tile-wide behavior. The
bldgonly fixture is what v3 prod actually operates on; matching v2
there isolates the smallest possible v3-vs-v2 surface area.

The full-tile baseline remains useful for:
- Catching regressions in v3's tile-wide bench tool itself (algo
  correctness gate)
- Validating that v2 still byte-matches its own historical full-tile
  output (catches accidental v2 changes)

See `THRESHOLD-INVESTIGATION-DAHOUET.md` for the full reasoning behind
the bldgonly fixture construction, and the `dahouet-bldgonly-fixture`
project memory for the gotchas.

### Last-known v3-vs-v2 divergence on bldgonly (2026-05-18 audit)

Captured here so the next session has the baseline:

| Step | Status | Notes |
|---|---|---|
| tile-signals | ✅ at expected noise | `point-weights.bin` byte-equal; ~0.09 % of normals/curvatures differ at libm/V8-cos noise (documented §5.1) |
| hemisphere-segment | ✅ EXACT | `cascade-segments.bin` byte-equal |
| normal-cascade-seed | ✅ EXACT | `owner.bin` byte-equal; 43=43 planes |
| density-voids-2 | ✅ EXACT count | 6=6 ghost-planes |
| grow-refit-merge | ✅ EXACT | `owner.bin` byte-equal; 49=49 plane equations |
| beam-incidence | ✅ at expected noise | 62 % of floats differ, max 1.4e-6 (~2 ULPs on unit vectors). Three independent F32-precision sources: libm-vs-V8 `sin`/`cos`, `sqrt(sum²)` vs `Math.hypot`, and `bh0*inv_len` vs `bx/len`. Per-axis asymmetry (x:99%, y:61%, z:27%) tracks op-chain length: y has the shortest path (just `-cos_a/len`), z's multiply-add collapses to a single multiply when `corr_h1=0`. Geometric impact ≈ 7 μm at 5 m projection distance — eight orders of magnitude below downstream thresholds. No cascade to boundary-merge metrics (still 11=11 shared edges). |
| boundary-signals | ✅ ridge/RI exact; cand counts close | 49 vs 40 planes (+9 walls); 34=34 ridges; 11=11 RI |
| boundary-corners | ✅ ridge-end/RI exact; pool close | pool 299 vs 316 (close ignoring walls/coplanar); 11=11 RI; 38=38 ridge-end |
| boundary-polygons | ✅ EXACT on metrics | 27=27 polygons; 3=3 shared edges |
| boundary-merge | ✅ EXACT on metrics | 27=27 polygons; 11=11 shared edges |

Reproduce: run the 10 bench tools with `<refs-dir> = /tmp/v7-refs-bldgonly-fresh`,
compare to v2 dump artifacts at the same path.

### Test cases

Use bench-v2's `test-cases-v2/` fixtures (synth + proc + real). Three
canonical fixtures to lock for the v3 port:

- `synth-flat-roof-noise` — simplest happy path
- `proc-hip-roof` — non-trivial multi-plane geometry
- **`real-22186-dahouet-502`** — the canonical real-LiDAR fixture
  the v3 port has been validated against. Three coexisting variants that
  are easy to confuse:

  | Fixture | Points | Frame | Origin | Use |
  |---|---|---|---|---|
  | `real-22186-dahouet-502.{json,bin}` | 45696 | v2 y-up local (centred on `metadata.l93_center`) | Full IGN LiDAR-HD tile, all classes | v2 tile-wide v7 run; v3 prod loader input |
  | `real-22186-dahouet-502-bldgonly.{json,bin}` | **16092** | same v2 y-up local frame | **Subset of the full TCBD to the same point indices that v3 prod's `extract-buildings` keeps (building footprint + 5 m buffer)** | v2 tile-wide v7 run on a working set identical to v3 prod's per-bldg subset — designed for byte-level v2/v3 comparison |
  | v3 prod input (same `.bin` as full) | 45696 in → 16092 per-bldg out of `extract-buildings` | z-up Lambert93 world | Same physical points as bldgonly after rotation | per-bldg production pipeline (`research-v7-tile-signals-real.json`) |

  **Important**: `bldgonly` is NOT a class-filter. It is the building +
  5 m buffer SUBSET, byte-matched to what v3 prod's
  `extract-buildings` selects on the same input. Same 16092 physical
  points; the only difference between v2 bldgonly and v3 prod's
  per-bldg working set is the coordinate frame (rotationally equivalent).

  Shared-edge benchmark on dahouet:
  - v2 full tile → **14** shared edges at `v7-boundary-merge`
  - v2 bldgonly → **11** shared edges
  - v3 prod (per-bldg) → 7 (pre-fix) → 14 expected after fixes (verify under task #253)
  - v3 bench tool on the matching ref dump → 14 (full) / 11 (bldgonly) — both byte-match v2 since commit `0f9ddf9` (plane-equations override of stale seed normals)

  Full investigation: `v3/docs/plan/THRESHOLD-INVESTIGATION-DAHOUET.md`.

### Directory & build layout

- `v3/steps/v7/` — one .cpp per ported step, plus `v7-canary.cpp` and
  the usual `v7-steps.{c,h}`.
- `dioramap_steps_v7` static lib, gated on `DIORAMAP_RESEARCH=ON`
  (same firewall as polyfit / ksr / roofer).
- `v3/pipelines/smoke/research-v7.json` — full V7 chain pipeline.
- Validation harness: `v3/scripts/bench-v7-validate.sh` (loads
  bench-v2 refs, runs v3 step, prints per-array diff). Set up once;
  reused for every step.

## Phase order

| Phase | Step | Status |
|---|---|---|
| 5.0 | Plan + skeleton (this doc + v3/steps/v7/ + canary + CMake gate) | done |
| 5.0b | bench-v2 reference-dump CLI (`scripts/dump-v7-refs.js`) | done |
| 5.1 | `v7-tile-signals` (impl + step + bench harness) | done |
| 5.1b | `v7-load-real-test-case` + lab-ui viz helpers + 13 viz wrappers + dahouet pipeline + v2-frame ↔ LAZ-frame loader | done |
| 5.1c | Factor kd-tree + centroid + bbox: `build-building-kd-trees` step + `building-local-frames` artifact (extract-buildings); kd_tree.h gets `nodes_owned` + `kd_serialize` / `kd_deserialize`; v7-tile-signals consumes both — mirrors v2's transient `'kd-tree'` artifact pattern | done |
| 5.1d | ARRAY_INLINE record bytes 8-byte aligned (4-byte zero pad in record header + trailing pad to next 8-boundary). Bumped step versions on every ARRAY_INLINE writer (12 steps) so existing intent-map entries invalidate. `kd_deserialize` returns to zero-copy. New `array_inline_test` round-trip would have caught the original misalignment. | done |
| 5.2 | `v7-hemisphere-segment` (impl + step + viz; bench harness deferred) | done (algo) |
| 5.3 | `v7-normal-cascade-seed` | done |
| 5.4 | `v7-density-voids-2` | done |
| 5.5 | `v7-grow-refit-merge` | done |
| 5.6 | `v7-beam-incidence` (replaces planned `v7-detect-boundary` — audit found detect-boundary's only V7 consumer was bit 4, now inlined in `v7-boundary-corners`) | done |
| 5.7a | `v7-boundary-topology` | done |
| 5.7b | `v7-boundary-signals` (Phase B + Phase C) | done |
| 5.7c | `v7-boundary-corners` (D.0 → D.2b + pool merge + super-merge + coplanar + bench harness) | done |
| 5.7-pre | `v7-boundary-signals` v2: cache per-anchor CoM-direction bisector under new CORNERS flag bit 0x20 (`has_com_bisector_by_plane`) — lets `v7-boundary-polygons` skip its v2 D.4 phase entirely. See ROOF-GRAPH-FORMAT.md §4.6. | done |
| 5.7d | `v7-boundary-polygons` (D.3→D.8, no Dijkstra, MR drop, per-bldg layout) | done (commit f111696) |
| 5.7e | `v7-boundary-merge` (Stage 2.6 ridge + 2.7 eave, shared v7-corner-pool) | done (commit 1dd0ec0) |
| 5.7-fix | Dahouet shared-edge recovery: root-cause `has_side` flag bit silently zero in v7-boundary-topology (commit d0a634a) + D.2b FP-sign-of-zero snap-target guard (commit 1ea4570) + flag-condition asserts at all RIDGE_LINES consumer sites (commit e692c24). Bench-baseline shared-edge count went 7 → 14. Production-lab-UI verification pending under task #253. | done |
| 5.7-audit | FP-sign-of-mathematically-zero audit across v3 + v2 (#274/#277), flag-conditional silent-fallback audit (#275/#276), GRM strip-align FP-coin-flip guard (audit #274, commits 512c399 + 97dbb54). Comment-only annotations on safe sites; code fix only where genuine FP traps existed. | done |
| 5.7-doc | Per-step `.doc.json` sidecars for all 11 v7 steps (#278) — lab UI step-doc panel input. Schema: phases/parameters/reads/writes/v3_divergences/related[]/open_questions. Two passes: paleontology + verification-against-v3. See `v3/steps/v7/v7-boundary-corners.doc.json` as canonical sample. | done |
| 5.7-rg-cleanup | Roof-graph CORNERS unification — fold the standalone CORNER_MEMBERS section (0x07) into CORNERS as an inline `members[]` tail block (flag 0x40), and populate each member's `plane_id` field (was reserved zeros). Drives v2 `planeAnchors[g]` semantics in v7-boundary-polygons D.3 NMS. Bumps boundary-corners v5→v6, polygons v2→v3, merge v2→v3. Behavior-preserving on the bldgonly bench: 54/55 outputs byte-identical pre↔post; the only diff is the additive `planeId` field in corner-members.json dumps. v2↔v3 polygon-shape divergence (26 of 27 still differ) is NOT explained by per-plane positions on this dataset — deltas (3–13 cm) are below D.3's 40 cm dedup radius, so NMS picks the same anchors. Other root causes (cycle building, edge scoring) carry the divergence. (commits 201540e + ae673bb, 2026-05-19) | done |
| 5.7f | `v7-boundary-classify` — Phase F perimeter edge classification + outward XZ persistence + level-transition detection + PIP overhang fallback. Iso-equivalence on dahouet-bldgonly: 5/5 dimensions match v2 byte-by-byte (tag counts identical, per-edge tags 0/150 mismatch, outwardXZ presence 100% match, outwardXZ values max-\|Δ\|=2.93e-8 / F32-cap, new-adjacency counts identical). Spec extension: PERIMETER_TAGS flag bit 0x02 `has_outward_xz` (forward-compatible). 10 Stream-A tests (45 checks) + iso recipe in `tests/iso-equivalence.py`. (commits TBD, 2026-05-26) | done |
| 5.7g | `v7-boundary-regularize` (terminal artifact) | pending |
| 5.7h | `v7-wall-detect` — beam-geometry overhang lower bound + existing-wall merge + new vertical-plane emission + unassigned-point grab. First v3 step to MUTATE PLANES (append new walls) + CORNERS (4 per wall) + POLYGON_PATHS (replace merged + append new) in the roof-graph TLV, and the first downstream consumer of the 5.7f outward_xz spec extension. Iso-equivalence on dahouet-bldgonly: **8/8 dimensions match v2** — owner.bin BYTE-IDENTICAL (0/16092 mismatches), walls created 14, walls merged 5, points grabbed 30, per-edge deltaSource / mergedWith / pointsGrabbed 0 mismatches, deltaMin max-\|Δ\|=4.7e-4 m (within v2's `toFixed(3)` 1-mm quantum). Algorithm-level audit findings (8 trap classes; plane.source confirmed dead in v2 main path and dropped from v3 — saves a spec change). 8 Stream-A tests (39 checks). End-to-end smoke through `dioramap-processor batch`: 17 steps, classify 210 ms + wall-detect 222 ms. (commits TBD, 2026-05-26) | done |

### DAG scope — what we port and what we don't

The v7 pipeline in bench-v2 has 33 step entries; many are **dead branches**
(visualization-only ports, disabled experiments, deprecated growers, post-
boundary edge-fitting paths). For Phase 5 we only port steps **on the DAG
path leading to `boundary-regularize`'s output** (the terminal artifact).

Tracing backwards through bench-v2's `pipelines.js` v7 wiring overrides:

```
                       boundary-regularize  ← terminal
                              ↑
                       boundary-classify
                              ↑
                       boundary-merge
                              ↑
                       boundary-polygons
                              ↑
                       boundary-corners ─────────── detect-boundary
                              ↑                            ↑
                       boundary-signals ───────────────────┤
                              ↑                            │
                       grow-refit-merge ───────────────────┤
                              ↑                ↑           │
                normal-cascade-seed   density-voids-2      │
                              ↑                ↑           │
                       hemisphere-segment ─────┴───────────┤
                              ↑                            │
                       tile-signals (done) ────────────────┘
```

Steps **explicitly out of scope** (do not port):
`beam-incidence`, `y-histogram`, `ransac-full`, `ransac-poly`,
`planarity-watershed`, `grow-steal-resolve`, `density-voids` (v1 only —
v2 is in scope), `rasterize-footprint`, `competitive-grow`,
`point-stealing`, `planes-from-owner`, `roof-perimeter`,
`adjacency-edges`, `boundary-global`, `boundary-edges`, `boundary-3d`,
`wall-roof-detect`, `ridge-clip`, `combine-edges`, `snap-edges`,
`compute-wall-edges`, `close-corner-gaps`, `clip-to-footprint`,
`obstacle-filter`, `resolve-overlaps`, `shape-fitting`, `lod2-simplify`,
`cascade-ridge-detect`.

The v7 phase order above is now 11 algorithmic steps (1 done +
10 remaining), not 26 as initially scoped.

### Policy: porting v7 steps

Apply this lens to **every** new port (Phase 5.2 onwards):

1. **Trace consumers in the v7 main path before deciding what to emit.**
   Run `grep -nE "getArtifact\\([\"']<artifact>" lib/pipeline/steps/*.js`
   and cross-reference against the in-scope step list above. v2 emits
   many artifacts that only feed dead branches we're not porting; those
   become viz-only or droppable in v3.

2. **Three-bucket each output:**
   - **Load-bearing** — read by an in-scope downstream step. Always emit.
   - **Viz-only** — useful in lab UI but no algorithmic consumer in scope.
     Emit gated on a step parameter `emitVizArtifacts: bool` (default
     `true`). Pipelines that target prod (`research-roof-recon-compare`)
     set it `false`.
   - **Derivable** — computable from other emitted outputs (e.g. v2's
     `cascade-segment-stats` is derivable from `hs-segment-list` +
     `ts-weights`). Drop. Lab UI computes on the fly when needed.

3. **For viz-only outputs, emit RAW data, not v2's metadata-wrapper format.**
   No `{kind: 'lambert-2d', label: 'Lambert Hemisphere Normal Density', …}`
   wrappers — emit `hs-lambert-grid` (Float32 NxN density surface) +
   `hs-lambert-peaks` (fixed-stride records with tilt/azimuth/etc.).
   The viz layer wears the labels and rendering choices, not the artifact.

4. **Skip pass-through outputs** that an earlier in-scope step already
   emitted. v2 sometimes re-emitted artifacts as a defensive shim when
   producers were newer than consumers; in v3 the in-scope DAG already
   ensures upstream availability.

5. **One step file = one `step_def_t`.** The `*-impl.{h,c}` companion
   pattern (pure compute) only when the algorithm is meaty enough to
   deserve a bench-harness CLI. Trivial steps stay single-file.

6. **Bench harness validates only load-bearing outputs** byte-against-v2.
   Viz-only outputs get loose tolerance checks (RMS error, peak counts,
   etc.). The numerical-correctness gate is the production-path artifacts.

7. **Reusable infrastructure check** — before writing new code, audit
   what existing v3 steps + 5.1c/5.1d infrastructure already provide
   (kd-tree, PCA, building-local-frames, building-kd-trees, padded
   ARRAY_INLINE). The 5.1c factoring should keep paying dividends.

## Post-port reassessments

Surfaced during the port — items where v3 currently mirrors a v2 quirk
to keep byte-validation tractable, but where the v2 quirk itself looks
like a transparent bug or hand-tuned-without-coordination value. Revisit
*after* the full V7 chain is ported and end-to-end byte-validated, when
we can change one thing at a time and measure the effect on roof-recon
metrics on the 219-case bench.

- ~~**ts-weights wiring to `normal-cascade-seed`**~~ — **RESOLVED 2026-05-27**.
  Closed during the 5.7g.5 iso-debug detour. Root cause turned out to
  be broader than just NCS: v2's GRM also used unweighted PCA, and
  boundary-signals/boundary-edges did a redundant area-weighted refit
  to compensate. Fixed three layers:
  1. v2 GRM v10 + boundary-signals v7 + boundary-edges v11 → all
     plane-fits area-weighted; redundant refits deleted.
  2. v2 NCS, DV2, planes-from-owner, adjacency-edges, hemisphere-
     normals-density-aware-segment, and 9 disabled-in-v7 steps → all
     wire `pointWeights` to their plane fits.
  3. v3 `v7_plane_fit_lsq` requires `point_weights` (NULL → -1). v3
     GRM, DV2, NCS, HS, BS all enforce non-NULL at entry and strip
     silent `? : 1.0` fallbacks from cluster/group/comp-area accumulators.
  Validated by iso boundary-regularize: dof.axisA/B alignment now at
  machine epsilon (4.4e-16). See journal entry
  `2026-05-27-boundary-regularize-iso-baseline.md` Pass 3.
- **distThreshold mismatch — seed (0.09) vs GRM (0.12)**. Seed derives
  from `σ_z=0.02 → max(0.05, 4.5σ) = 0.09`. GRM has a literal hardcoded
  `0.12` with TODO comment "was deriveDistThreshold(zNoise)=0.09;
  hardcoded 12cm to test strip-offset tolerance". So GRM was hand-tuned
  to absorb IGN-LiDAR-HD strip-offset noise (≈5 cm parallel-plane drift
  per scan strip) at the cost of a looser inlier gate. Seed wasn't
  retuned in the same pass. After full port: review both as a coupled
  pair — seeds want a tight gate to start clean; GRM wants a looser
  gate to grow across strip boundaries. Likely candidates: keep them
  different but justify, or unify with a per-stage scale factor.
- **`boundary-signals` vertical-beam fallback must be removed**
  (surfaced 2026-05-10 during 5.6). v2's boundary-signals.js:638 falls
  back to `(0, -1, 0)` (straight down) when `bi-directions` is
  absent. The user (after porting beam-incidence) confirmed that
  fallback is **wrong and unusable** — nearby-projection along a
  vertical ray crosses the wrong nearby planes (any non-vertical roof
  surface gets projected onto by every overhead point), producing
  garbage `nearbyItems[]` lists that downstream boundary-corners /
  -polygons / -regularize then build off. v3 should make
  `bi-directions` a strict required input — fail loudly when
  missing rather than silently degrading. v2 keeps the fallback for
  legacy compatibility (older test cases didn't have scan-angles in
  their TCBD sidecars). v3's loader now always emits scan-angles when
  the sidecar carries them, so the fallback becomes a foot-gun
  rather than a safety net. **Action**: in v7-boundary-signals,
  refuse to run when `bi-directions` is missing.
- **`detect-boundary` step skipped — bit 4 inlined in `boundary-corners`**
  (surfaced 2026-05-10 during 5.7 audit). v2's `detect-boundary` computes
  a 7-bit mask (height-gap k-NN, ground-prox eave, multi-return,
  wall-return, sub-classification) tile-wide, ~400 LOC. The full V7
  boundary chain (`boundary-signals` through `boundary-regularize`) reads
  the mask at exactly **one** site — `boundary-corners.js:703`,
  `& 16` (bit 4 = not-last-return). Every other bit exists for off-V7
  legacy consumers (`boundary-3d`, `wall-roof-detect`, `co-register*`,
  `snap-footprint`, `boundary-edge-detect*`). v3 skipped the standalone
  step entirely; the bit-4 predicate is inlined inside
  `v7-boundary-corners`'s per-building plane loop, reading the
  `return-numbers` + `number-of-returns` POINT_COLUMNs the loader
  already emits. `boundary-candidate-infos` (object-array side output
  with PCA-derived `dirBin` per candidate) has no V7 consumer either,
  so its 80 LOC of connected-components + PCA in detect-boundary's
  Phase 0b drops too. **Reactivation cost** if v7 ever needs other
  bits: full ~400 LOC port back as a separate step (same as today's
  audit's "original" path) — it's not on a critical path, so deferral
  is cheap.

- **Anchor `dirs` could feed `boundary-polygons` cycle extraction**
  (surfaced 2026-05-11 during boundary-signals Phase C audit). The
  per-anchor outgoing edge directions (`dirA`, `dirB` — two unit
  vectors per concave/convex anchor derived from the OA-gradient
  local-maximum's best-left / best-right neighbour) are currently
  **viz-only metadata** in the v2 chain: downstream
  `boundary-corners` / `boundary-polygons` / `boundary-merge` /
  `boundary-classify` / `boundary-regularize` propagate them as
  opaque payload, never read for math; the only actual reads are in
  the LEGACY monolithic `boundary-3d.js` step (red OA-whiskers / blue
  CoM-whiskers debug overlay).

  As of the 2026-05-11 cleanup commit, v3 re-enables `dirsByPlane` on
  CORNERS (flag bit 0x04) so the values land in artifacts and can be
  read by downstream steps. The PLANE_ANCHORS section was DROPPED
  (redundant with CORNERS+planeIds+dirsByPlane); a per-plane anchor
  iteration is a one-line pass over CORNERS where `source ∈ {4,5}`
  AND `planeId ∈ planeIds`.

  **Update 2026-05-16** — the "per-plane anchor = filter by source AND
  plane_id" reasoning held while only signal corners existed. Coplanar
  import (Task #243, same day as the 0x08 drop) absorbs a foreign
  pool entry into a new plane with `SRC_COPLANAR`, but the absorbed
  entry retains its origin-plane source. That broke the assumption.
  Fixed by adding `source_per_plane[]` parallel to `plane_ids[]` inside
  the CORNERS record (no new section — see ROOF-GRAPH-FORMAT.md §4.6).
  Section 0x08 stays reserved.

  **Opportunity**: when `boundary-polygons` ports, audit the cycle-
  extraction algorithm for places where these pre-computed local edge
  tangents could substitute for ad-hoc neighbour searches:
  - Polygon-graph cycle picking (each anchor knows two outgoing
    directions → graph edges should preferentially follow them)
  - 35° / 72° direction-cone gates on candidate edges (anchor's
    dirA/B is the natural cone centre)
  - Score-based corner→corner walk (boundary-3d.js#L2333+) uses
    direction agreement; `dirs` is exactly the signal it
    reconstructs from scratch

  Action: during the `v7-boundary-polygons` port, re-read app.js
  L4286+ and boundary-3d.js L2280–2333 to surface where v2 recomputes
  edge tangents that we already have.

- **OADirs (`dirA` / `dirB`) for D.5 edge scoring + D.7 polygon
  scoring** (surfaced 2026-05-13 during boundary-polygons audit). v2
  `boundary-polygons.js` builds its candidate edge graph (D.5) on
  three signals — balance-grid path average, proximity, ridge bonus
  — and scores final cycles (D.7) with WSD + edge-avg + bisector
  coherence + shoelace + compactness. **None of the per-anchor
  `dirA`/`dirB` OA-tangents are consumed.** That's a missed signal:
  the two outgoing edge tangents at each anchor are precisely the
  ground-truth direction along which legitimate polygon edges should
  leave the anchor. Two concrete experiments to run **after** the
  baseline byte-port lands:

  - D.5 candidate filter: for each candidate edge (A, B), require the
    edge direction to align with at least one of A's `dirA`/`dirB`
    AND one of B's `dirA`/`dirB` within a cone (e.g. ±25°). Drop
    edges that fail both endpoints — eliminates implausible cross-
    cuts before D.6b's contour-walk anchor sort.
  - D.7 score bonus: at each cycle corner, reward the cycle when the
    in-edge and out-edge directions match the anchor's `dirA`/`dirB`
    pair (one direction in, the other out). Penalise when both
    cycle edges land on the same OA tangent (a degenerate fold).

  Likely improves polygon quality on buildings with crisp 90° / 60°
  corners where v2 currently picks suboptimal cycles when balance-
  field path-average is weak. Cost: cheap — `dirsByPlane` is
  already cached. Risk: byte-divergence from v2 (acceptable once the
  baseline port is byte-valid and we move into algorithm-tuning).

- **CoM-direction bisector — diverges from v2 D.4 under pool merge**
  (surfaced 2026-05-13 during 5.7-pre design). v3 caches the bisector
  at signal-emission time using the original candidate position. v2
  computes it fresh inside `boundary-polygons.js` D.4 using the
  POST-pool-merge anchor position. The two values can differ when
  `v7-boundary-corners` super-merge shifts an anchor by up to
  pool_merge_dist (≤ 0.25 m → up to 2.5 grid cells), since the OA-grid
  cell looked up changes. Impact expected to be small in practice
  (concave/convex anchors rarely co-locate with RI corners and so
  rarely move during merge) but non-zero. **Action**: if bench
  validation flags meaningful polygonPath / edge divergence on real
  cases, fall back to recomputing the bisector in `v7-boundary-
  polygons` (port v2 D.4 logic). Until then, the cache wins.

- **D.2b ridge-side filter — design space + meta-critique**
  (surfaced 2026-05-17 during the dahouet shared-edge investigation).
  After the topology `has_side` flag bug was fixed, the underlying
  D.2b filter became visible as a hand-tuned 1.5 m radius + perpendicular-
  to-infinite-line distance test that has two known failure modes:
  (1) ridge segment doesn't extend to the anchor's locality (long-axis
  case), and (2) courtyard-scale parallel ridges of the SAME plane
  (U-shape or frame-shape boundary). Candidate gates considered: max-
  distance cap (current), segment-distance (clamped t), outward-direction
  via `com_bisector_by_plane` (has a known U-shape interior-corner
  failure mode — see journal). Decision: **no change yet** — v3 stays
  aligned with v2 until byte-parity is reached. Open questions and
  meta-critique (D.2b as an upstream-symptom treatment) captured in
  full at:
  `docs/journal/2026/05/2026-05-17-wrong-side-corner-killing-review.md`.

- **Per-step `.doc.json` sidecar schema is provisional**
  (surfaced 2026-05-17, work landed under #278). The cross-reference
  `related[]` shape (`{type: "journal"|"commit"|"doc", ...}`) was a
  first cut, not a designed schema. **Action**: when the lab UI step-
  doc panel is built and starts consuming these files, validate one
  sidecar's rendering end-to-end and revise the schema based on what
  the UI actually needs. Coverage gap: only v7 steps have sidecars; if
  the lab UI shows non-v7 v3 steps (extract-buildings, KSR, Roofer,
  etc.) those need the same treatment.

## Follow-ups to revisit after v3 reaches byte-parity with v2

Carry-over from the 2026-05-17 corner-killing review and the .doc.json
work — items where the right answer is "wait for byte-parity, then
measure":

1. **D.2b: segment-distance instead of perp-to-infinite-line.** Catches
   failure mode 1 from the journal entry. Cheap to apply; needs to land
   on both v3 and v2 in lock-step.
2. **D.2b: outward-direction (`com_bisector_by_plane`) as a third
   stacked gate?** Pros/cons enumerated in the journal — U-shape
   interior-corner failure mode is the main risk. Decide after #1 ships.
3. **D.2b: should it exist at all?** Meta-critique — could upstream
   changes (boundary-signals weighting by plane-interior consistency,
   topology priority of RI corners over signal anchors, more aggressive
   D.2 snap) reduce wrong-side anchors enough that D.2b becomes
   unnecessary?
4. **Quantify D.2b false-positive rate across the 219-case bench.**
   Currently unmeasured — we're optimizing blind. Add a counter to the
   bench harness.
5. **Lab UI step-doc panel.** Wire the 11 `.doc.json` files to a
   render surface; the schema may need adjustment based on what HTML
   the UI ends up needing.
6. **`related[]` schema refinement** (see post-port reassessment).
7. **`.doc.json` coverage for non-v7 v3 steps** if the lab UI shows
   them (extract-buildings, KSR, Roofer, etc.).

## Open questions / known concerns

- **Validation harness**: bench-v2 doesn't currently have a CLI that
  dumps per-step refs. Need to add one (small node script). Each ref
  is a JSON manifest + binary blobs of the typed arrays each step
  produces.
- **Floating-point tolerance**: each step needs an empirically-tuned
  eps. Earlier signals (PCA eigenvalues, normals) have tighter
  tolerance (~1e-5); later derived ones (boundary signals, roof
  graph) have looser (~1e-3 on positions, exact on integer indices).
- **KD-tree determinism**: v2 uses a custom KD-tree with insertion
  order = input order. v3 should match. CGAL's `Spatial_searching`
  doesn't guarantee identical neighbor ordering for ties; we may need
  a custom KD-tree for the v7 path or a stable ordering on top of
  CGAL's results.
- **Density-voids-2** is in active iteration in v2 (per the user's
  memory). The port should target the version pinned at the v2 commit
  current at port time; subsequent v2 changes are a manual sync.

## Decisions / gates

- **2026-05-07** — Phase 5 ordering: foundation first (`tile-signals`,
  `hemisphere-segment`, `normal-cascade-seed`), then the experimental
  bit (`density-voids-2`), then the main engine (`grow-refit-merge`),
  then the boundary chain. This is the data-flow order — each step's
  input is the previous step's output, so testing is incremental.
- **2026-05-07** — Phase 5.1 implementation pattern. Pure-compute lives
  in `v7-tile-signals-impl.{h,c}` (no v3-runtime deps); step wrapper
  `v7-tile-signals.c` parses `per-building-records`, gathers per-building
  positions (centroid-shifted then cast to F32 to match v2's
  `Float32Array` storage), runs the impl, packs each output into a
  per-building `AK_ARRAY_INLINE` artifact. Standalone CLI
  `v7-tile-signals-bench` validates against the bench-v2 dump
  byte-by-byte. Subsequent Phase 5.x steps follow the same pattern:
  pure compute + step wrapper + bench harness.
- **2026-05-07** — Phase 5.1 numerical match. 9 of 13 outputs are
  byte-identical to bench-v2 on `gable-default` (21544 pts):
  ts-eigen-l1, ts-eigen-l2, ts-planarity, ts-linearity, ts-eigen-rel,
  ts-weights, ts-eigen-ternary; plus the bench harness's positions
  buffer cmp == 0. The remaining 4 outputs differ on 0.02–0.14 % of
  values at fp-noise level (~1e-16 abs): ts-eigen-l3, ts-scattering,
  ts-curv-k8/k16/k32 — these are downstream of `cos(phi + 2π/3)`
  in the Cardano eigenvalue solver, where glibc's `cos` and V8's
  `Math.cos` diverge in the last bit. point-normals shows ~5 sign
  flips (<0.1 %) for points where `ny ≈ 0` (orientation
  genuinely ambiguous). Considered acceptable; bit-identical match
  with V8 transcendentals is not achievable in C.
- **2026-05-07** — Phase 5.1 K-expansion semantics. v2's `expanded`
  flag is set the instant `lin0` (at `K=eigen_k`) trips the trigger,
  even if `lin` (at `K_STAGES[0]`) is already below threshold and the
  expansion loop body never runs. The downstream K-loop reads
  `expanded` to decide whether to use `n_total` or the per-slice cap;
  conflating "actually expanded beyond K_STAGES[0]" with "trigger
  fired" gives wrong K=16 PCA results for ~3 % of points.
- **2026-05-07** — Phase 5.1b coordinate-frame contract.
  bench-v2's `.bin` sidecar stores positions in a local three.js frame
  centred on `metadata.l93_center` with one axis flipped:
  `bin = (LAZ.x - cx,  LAZ.z,  -(LAZ.y - cy))`.
  `v7-load-real-test-case` inverts the transform on read so v3 sees
  standard LAZ-frame `lidar-positions` (east, north, alt). The pure
  compute `v7_ts_compute` exposes an `up_axis` param so the same
  binary serves both purposes:
  - Bench harness defaults to `up_axis = 1` (y-up) → byte-identical
    against bench-v2 dumps.
  - Pipeline step (`v7-tile-signals.c`) sets `up_axis = 2` (z-up) →
    correct in v3's L93 + altitude frame.
  Affects only the upward-flip + degenerate-default normal — every
  scalar / eigenvalue is rotation-invariant.
- **2026-05-07** — Phase 5.1b validation tiers.
  Two independent gates, both honoured:
  1. **Algorithmic** — `v7-tile-signals-bench` consumes the v2 dump's
     `input-lidar.positions.bin` directly. On `gable-default` 9/13
     outputs are byte-identical; the residual 4 differ on
     0.02 – 0.14 % of values at libm/V8-cos noise (~1e-16). On
     `dahouet` ~0.07 % of values diverge with median ~1e-3 abs error
     — same root cause amplified by real-data borderline cases on
     the K-expansion threshold. `ts-weights` is byte-identical
     in both cases.
  2. **Pipeline** — `research-v7-tile-signals-real.json` runs the
     loader + extract-buildings + v7-tile-signals chain on dahouet
     (789 ms; 45696 pts → 16092 building inliers). Pipeline output
     cannot byte-match v2 dump by design: (a) v2 ran on the whole
     tile, v3 runs per-building → kNN neighbourhoods differ at the
     footprint boundary; (b) outputs are in v3's LAZ frame, v2 in
     three.js frame. Lab UI is the visual gate for this tier.
