# Scoring design — v2 analysis + v3 plan

Analysis of how `lidar-algos-bench-v2` scores algorithm output, what is
worth carrying into v3, what should be dropped, and how the v2 design
maps onto v3's very different architecture.

This is the deeper "port v2 scoring" study behind ROADMAP item #73 and
the Stream B design in `V7-TEST-STRATEGY.md §4`. That doc already drafts
the v3 metric table and harness layout; this doc explains the **source**
system it's drawn from, and justifies the keep/drop calls.

Scope: **analysis only.** No code is changed by this document.

---

## 1. The four capabilities you remembered, located in code

All four exist. Here is each, with the actual implementation.

### 1.1 Pipeline-wide scoring

A pipeline run produces one final 3D model; `computeAllMetrics()`
scores that whole model against ground truth and rolls everything into a
single composite (0–100).

- Entry point: `lib/metrics/index.js:67` `computeAllMetrics(result, groundTruth, footprint, positions, classifications, pointPlaneIds, footprintOffset)`.
- The benchmark driver calls it once per (pipeline × test case):
  `scripts/benchmark.js:414`, and stores `compositeV3 ?? compositeV2 ?? composite` as the case score (`benchmark.js:431`).
- Three composites coexist — `compositeScore` (V1), `compositeScoreV2`,
  `compositeScoreV3` — in `lib/metrics/composite.js`. V3 is primary; V1/V2
  are kept only for historical bench.db comparison.

### 1.2 Scoring multiple outputs within one pipeline

A single pipeline emits several **named** output artifacts, and
`computeAllMetrics` scores each output *family* with its own metric
group in the same pass:

| Output artifact | Metric family | Module |
|---|---|---|
| `planes` | detection rate, segmentation F1, boundary IoU/Hausdorff, plane normal/tilt/azimuth accuracy, completeness, coverage/overshoot/overlap | `segmentation.js`, `boundary.js`, `coverage.js`, `geometry-quality.js`, `residual.js` |
| `surfaces` (curved) | surface detection rate, parametric accuracy, type accuracy | `surface-accuracy.js` |
| `edges` | edge completeness/precision/orientation, edge-type accuracy | `edges.js`, `edge-type.js` |
| `alignment` | co-registration residual, alignment score, direction accuracy | `alignment.js` |

So "score multiple outputs in one pipeline" = one model, several output
kinds, each scored independently then folded into the composite
(`index.js:274-305`). GT-dependent families return `null` when their GT
or the producing step is absent.

There is **also** a second, more granular form of this: the standalone
`boundary-bench/dim-bench/` harness (`bench.js`) scores *intermediate*
signals — D1 position, D2 direction, D3 topology labels, D4 global
topology — across multiple competing **providers** per dimension. That
is a research scaffold for developing the boundary algorithm, not
end-to-end model validation (see §2.6 / drop list).

### 1.3 Flagging a step as a scorer input

Pipelines declare an `outputs` map that names which step (by label)
produces each scorable artifact:

```js
// lib/pipeline/pipelines.js:292
outputs: {
  planes: 'shape-fitting#0', edges: 'resolve-overlaps#0',
  obstacles: 'build-obstacle-records#0', 'planes-lod2': 'lod2-simplify#0',
  alignment: 'co-register#0',
  'solar-panels': 'fit-solar-panels#0', 'solar-summary': 'fit-solar-panels#0',
}
```

`runner.js:166` (`getOutput`) extracts those qualified artifacts after
the run. A **subset** of the output names — `SCORER_INPUTS = {planes,
edges, surfaces, alignment}` — are the ones the scorer consumes; the UI
graph view styles exactly those output ports gold with a gauge icon
("Scorer input"), per the `2026-03-07-scorer-v3-23-metrics` journal
entry. That's the "flag a step's output to be used for scoring"
mechanism: the step is named in `outputs`, and if its name is in
`SCORER_INPUTS` it feeds a metric family.

### 1.4 Metrics for cases with ground truth AND without

The scorer is dual-mode (`index.js:77`):

- **With GT** (synthetic + annotated real): full 23-metric path, GT-
  dependent families active, composite from all axes.
- **Without GT** (real buildings, e.g. `real-*`):
  `computeNoGTMetrics()` (`index.js:375`) returns **geometry-only**
  intrinsic metrics — completeness, footprint coverage, residual RMS,
  building RMS, footprint overshoot, plane overlap, alignment — and a
  no-GT composite. GT-dependent fields are `null`, never 0.

The two composites encode this directly:
`compositeScoreV2` switches on `metrics.detectionRate != null`
(`composite.js:69`): 7 GT-metrics vs a 5-metric geometry-only blend.
`compositeScoreV3` (`composite.js:115`) drops null components and
**redistributes their weight** proportionally across whatever remains —
so the same function scores GT and no-GT cases on a comparable 0–100
scale.

The motivation is recorded in `2026-02-26-idea-real-building-ground-truth`:
most real buildings have no verified GT, so they can only be judged by
GT-free metrics (or pseudo-GT by algorithm consensus).

---

## 2. The rest of the system (supporting machinery)

### 2.1 Metric catalog + versioning discipline

- 23 numeric metrics across ~8 axes (`2026-03-07-scorer-v3-23-metrics`).
- Every metric module exports a `VERSION`; `getMetricVersions()`
  (`index.js:39`) collects them and they're stored per run in
  `runs.metric_versions`. **CLAUDE.md mandates bumping VERSION on any
  computation change.** This lets you compare results across scorer
  revisions regardless of git state.
- Matching is **one-to-one** between detected and GT items before
  scoring (greedy in segmentation; the v3 plan upgrades to Hungarian).
- Detection rate is **area-weighted** (`index.js:128-145`).
- "N/A vs 0": uncomputable metrics are `null` with a UI reason, never 0.

### 2.2 Storage + UI

- Results land in SQLite `results/bench.db`: 36-column `results` rows +
  `runs.step_versions` + `runs.metric_versions` + git state
  (`lib/benchmark/db.js`). Metrics are **not** cached by the pipeline
  step cache — scorer changes don't invalidate pipeline caches
  (`2026-03-07` design decision #1).
- UI renders all 23 metrics grouped by axis with N/A tooltips.

### 2.3 Focused polygon scorer

`boundary-bench/score.js` is a self-contained boundary-polygon scorer
(IoU via Sutherland–Hodgman + ear-clipping, Hausdorff, edge-count ratio,
vertex accuracy, closed bonus → 0–100). Useful as a **reference
implementation** of polygon IoU if v3 ever scores in C instead of
Python.

---

## 3. What to KEEP (port the concept, not the JS)

1. **Dual-mode GT / no-GT scoring.** Non-negotiable: real buildings
   (Dahouet etc.) have no GT and can only be judged by geometry-only
   metrics. Port `computeNoGTMetrics`' intrinsic set (completeness,
   footprint coverage, residual RMS, overshoot, overlap, alignment).
2. **The metric families themselves** (detection, segmentation F1,
   boundary IoU/Hausdorff, residual RMS, plane normal/tilt/azimuth
   accuracy, completeness, coverage/overshoot/overlap, edge
   completeness/precision/orientation/type, surface param accuracy,
   alignment). These already map 1:1 onto the v3 metric table in
   `V7-TEST-STRATEGY.md §4.3`.
3. **One-to-one matching before scoring** (upgrade greedy → Hungarian on
   plane-normal-angle / polygon-centroid / corner-distance, per §4.3).
4. **N/A-vs-0 discipline** — keep null semantics; a missing output must
   not read as a zero score.
5. **Metric versioning** — replicate the per-metric VERSION + a
   `metric_versions` block in `scorecard.json`. Cheap, and it's the only
   way to compare scorer revisions honestly.
6. **Area-weighted detection rate** — avoids tiny-plane gaming.
7. **Scoring stays decoupled from the pipeline cache** — already the v3
   posture (scoring is an external eval layer).
8. **Per-tier curation + baseline-diff** (easy/standard/hard) — already
   adopted in §4.2/§4.5.
9. **`boundary-bench/score.js` polygon-IoU math** — keep as a reference
   if a C scorer is ever needed.

## 4. What to DROP / not port

1. **Three coexisting composites (V1, V2, V3).** Keep one adaptive
   composite (V3-style redistribution). V1/V2 exist only for historical
   bench.db comparison, which retires with v2.
2. **The in-process JS scorer architecture.** v2 imports metric modules
   into the Node benchmark runner and scores live JS objects. v3's
   pipeline is a C binary emitting a serialized model — scoring must be
   an external comparator (already chosen: Python in
   `tests/v7-model-validity/`). Do not try to embed a JS scorer.
3. **SQLite `bench.db` (36-col results + step/metric_versions tables).**
   v3's lighter `scorecard.json` + `baselines/` + `history/` replaces
   it. Don't port the DB or its migrations.
4. **The `dim-bench` 4D provider harness.** Research scaffolding for
   *building* the boundary algorithm (comparing D1–D4 signal providers),
   not for validating a finished model. It retires with the research
   phase. Re-create later only if v3 wants a provider bake-off.
5. **UI graph "scorer-input gold port" styling.** v2-UI concern; revisit
   only if v3's lab-ui grows an equivalent graph view. Low priority.
6. **Solar / obstacle outputs in the scorer** — out of scope for v7
   model validity; score only planes/polygons/edges/corners.

## 5. Why v3 is a different shape (the integration deltas)

| Axis | v2 (bench) | v3 (dioramap-processor) |
|---|---|---|
| Language | Node/JS | C (processor) + Python/Node (eval harness) |
| Pipeline output | live JS objects (`planes`, `edges`, …) on a context | serialized model: `ARRAY_INLINE` planes + `RGRAPH` TLV polygons/edges/corners |
| Where scoring runs | in-process, same Node run as the pipeline | **out of process** — a comparator reads serialized output + GT |
| Oracle | GT bundled in the test-case JSON | bench-v2 generator's **analytic** GT (synthetic); none for real cases |
| Result store | SQLite bench.db | `scorecard.json` + committed `baselines/` |
| Gating | none (research scoreboard) | impl-tests gate PRs; model-validity gates releases (§4.7) |

Consequence: v3 cannot "import the scorer." The natural v3 form is a
**comparator that parses the serialized model**, which is exactly what
`tests/v7-model-validity/metrics/compare.py` already started (it parses
`ARRAY_INLINE` + `RGRAPH` TLV and implements `plane_count_match`).

## 6. Decision (2026-05-27): hybrid scoring

Premise that drives the design: in v3 **the entire scorable model is in
the serialized output** (`ARRAY_INLINE` planes + `RGRAPH` TLV polygons /
edges / corners). An external reader therefore sees *everything* an
in-pipeline step would — so the choice is about cost and separation of
concerns, not data access.

Resolution: **two scoring surfaces with a clean split.**

### 6.1 External GT comparator — the regression / quality net

The GT-based scoring (model validity, baselines, dashboard, algorithm
bake-offs) stays **external**, in Python, extending the B.1 skeleton in
`tests/v7-model-validity/`. Rationale: the GT geometry (concave polygon
IoU, Hungarian matching, Hausdorff) is far cheaper and safer in
`shapely`/`scipy` than in audited C, and it keeps the test oracle +
baseline-diff + gating out of the production binary.

- Reads the serialized roof-graph + planes + analytic GT.
- Full metric set from `V7-TEST-STRATEGY.md §4.3`, one-to-one Hungarian
  matching, per-output families (planes / polygons / edges / corners),
  per-metric tolerances → per-case pass/fail (gating) + metric means.
- Baselines + diff + dashboard.
- **Multi-variant comparison without an in-graph step**: dump N
  roof-graphs (different params / stages, via the existing
  dump-step / `stopAfter` machinery) and point the comparator at all of
  them. This is how "compare as we wish" is served on the GT side.
- Polygon IoU via `shapely` (robust concave clipping) — not a re-impl of
  Martinez–Rueda; v2 needed the `polygon-clipping` npm pkg for exactly
  the concave case (`2026-02-24-v2-composite-metrics`).
  `boundary-bench/score.js` remains the reference math.

### 6.2 In-pipeline intrinsic step — production confidence + in-graph use

A **small C step** (e.g. `v7-score-intrinsic`) in `steps/v7/`, computing
**only the no-GT intrinsic metrics** — completeness, footprint coverage,
residual RMS, footprint overshoot, plane overlap (the
`computeNoGTMetrics` set, `index.js:375`). Rationale: these need no GT,
no oracle, and are **rasterization + distance math only** — no concave
clipping, no Hungarian — so they are bounded and audit-friendly under
the standard step bar (review → safe_alloc → ACSL → Frama-C → -Werror).

- Reads the roof-graph + planes artifacts already on the context; writes
  a `model-score` artifact that serializes into the output, so a
  **served model carries a self-consistency confidence score** with no
  Python/GT on the box.
- Wireable into production pipelines (confidence) and repeatable in a
  test graph (score after merge vs after regularize in one run) — this
  is the "wired as we wish, multiple times" capability, for the metrics
  that are cheap to compute in C.
- Compute overshoot / overlap / coverage by **XZ rasterization** (as v2
  `geometry-quality.js` / `coverage.js` did), avoiding exact polygon
  clipping in C.
- Cache: it's a **leaf** (nothing reads `model-score` downstream), so a
  version bump invalidates only itself — it does not poison the rest of
  the pipeline cache, preserving v2's "scorer changes don't invalidate
  pipeline caches" property.

### 6.3 The division line (one implementation per metric)

To avoid two drifting implementations of the intrinsic metrics:

- The **C step is the single source of truth** for the intrinsic no-GT
  set. For real (no-GT) cases, the external comparator **reads the
  emitted `model-score`** rather than recomputing it, and adds nothing
  GT-dependent.
- The external comparator **only computes the GT-dependent metrics**
  (detection, segmentation F1, boundary IoU, plane/surface accuracy,
  edge metrics, alignment) — the part that has no business in C.
- Net: every metric is implemented exactly once, in the language that
  fits it.

### 6.4 Carried over to both surfaces

- **Versioning**: `metric_versions` in `scorecard.json`; the C step
  carries `meta.version`. Bump on any formula change (v2's CLAUDE.md
  rule).
- **N/A vs 0**: null when uncomputable, never 0.
- **One adaptive composite for ranking only** (port `compositeScoreV3`'s
  null-drop + weight-redistribution) for the dashboard — **never a
  gate**. Gating is per-metric pass/fail per §4.3.

## 7. Open decisions (need a human call)

- **Does the intrinsic step ship in production pipelines, or test-only?**
  The C step is cheap enough to always run, but whether the *served*
  model should carry a confidence score is a product call. Default
  assumption: wireable, off unless a pipeline opts in.
- **Composite formula**: confirm the single ranking composite is wanted
  at all (it is non-gating either way).
- **Pseudo-GT for real buildings?** v2 floated algorithm-consensus
  pseudo-GT (`2026-02-26-idea-real-building-ground-truth`). For v3 the
  analytic-synthetic oracle covers the gated path; real-building scoring
  stays the intrinsic `model-score` only. Defer pseudo-GT.
- **Resume gate.** Per `V7-TEST-STRATEGY.md §6.2`, the GT comparator
  build-out (B.2+: full metric set, baselines, tiers) stays paused until
  the v7 port matches bench-v2 on Dahouet AND v2 retirement is in sight.
  The **intrinsic C step is not gated by that** — it has no bench-v2
  dependency and can be built whenever. This analysis prepares the
  design so each side is a build-out, not a redesign.

## 8. Cross-links

- `V7-TEST-STRATEGY.md §4` — the v3 metric table, scorecard format,
  tiers, resume conditions (§6.2).
- `tests/v7-model-validity/` — the B.1 skeleton this extends.
- bench-v2: `lib/metrics/index.js` (orchestrator), `lib/metrics/composite.js`
  (composites), `lib/pipeline/runner.js` (outputs map),
  `scripts/benchmark.js` (driver), `boundary-bench/score.js` (polygon IoU),
  `boundary-bench/dim-bench/bench.js` (provider research harness),
  `CLAUDE.md` (versioning rule).
- bench-v2 journals: `2026-02-24-v2-composite-metrics`,
  `2026-03-07-scorer-v3-23-metrics`, `2026-02-26-idea-real-building-ground-truth`.
