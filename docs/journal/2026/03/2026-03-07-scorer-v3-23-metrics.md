---
id: "2026-03-07-scorer-v3-23-metrics"
date: 2026-03-07
title: "Scorer V3 — 23-metric system with full DB and UI wiring"
tags: [scorer, metrics, database, ui, alignment, edges, surfaces]
category: development
status: concluded
refs: ["2026-03-04-edge-accuracy-metric"]
---

## Context

The scorer evolved from V1 (7 metrics, composite weighted average) through V2 (added coverage/overlap) to V3. Each iteration added more axes of evaluation but the DB schema and UI only stored/displayed a subset. This session completes the full wiring.

## What changed

### Scorer V3 metric system (lib/metrics/)

23 numeric metrics organized in 6 axes, each with sub-components and raw values:

| Axis | Metrics | Source |
|------|---------|--------|
| **Detection** | `detectionRate` | planes vs GT planes |
| **Segmentation** | `segF1` | point-level precision/recall |
| **Boundary** | `boundaryIoU`, `hausdorffDist` | rasterized boundary overlap |
| **Residual** | `residualScore`, `residualRms` | point-to-plane distances |
| **Coverage** | `completeness`, `fpOvershoot`, `planeOverlap`, `fpCoverage` | rasterized area coverage |
| **Geometry** | `planeAccuracy`, `geometricAccuracy` | normal/offset agreement |
| **Surface** | `surfaceDetectionRate`, `surfaceParamAccuracy`, `surfaceTypeAcc` | curved surface matching |
| **Edge** | `edgeQuality`, `edgeCompleteness`, `edgePrecision`, `edgeOrientation` | edge segment matching |
| **Alignment** | `alignmentScore`, `alignmentResidual`, `alignmentDirAcc` | co-registration quality |

New modules: `alignment.js` (V1), `surface-accuracy.js` (V1).

Three composite scores coexist: `composite` (V1), `compositeV2`, `compositeV3`. V3 is the primary; V2/V1 kept for historical comparison.

### Database (lib/benchmark/db.js)

- 14 new columns added to `results` CREATE TABLE
- Migration 2: `PRAGMA table_info` detection + `ALTER TABLE ADD COLUMN` for each missing column
- `insertResult()` expanded to 36 columns (verified 36/36/36 alignment)
- Index changed from `composite_v2` to `composite_v3`
- Validated on existing 7,996-row bench.db — migration runs cleanly, old data gets null for new columns

### Pipeline runner (lib/pipeline/runner.js)

Output extraction expanded: `planes`, `edges`, `surfaces`, `alignment` are extracted from pipeline context and passed to `computeAllMetrics()`. Only `planes` and `edges` are wired in most pipelines today; `surfaces` and `alignment` require dedicated steps.

### UI (ui/public/app.js, style.css)

- All 23 metrics displayed with colored bars, grouped by axis
- N/A indicators with explanatory tooltips when pipeline doesn't output required artifacts (e.g., "No surfaces step in pipeline" for surface metrics, "No ground truth available" for GT-dependent metrics)
- Each metric name has a plain-English tooltip explaining computation
- `compositeV3` is the primary score display with V2/V1 fallback

### Graph view (ui/public/artifact-graph-view.js)

- Scorer-fed artifact ports (`planes`, `edges`, `surfaces`, `alignment`) styled with gold background, gold left border, and gauge icon
- Built from `pipelineOutputs` map filtered to `SCORER_INPUTS` set
- Tooltip: "Scorer input -- {name}"

## Key design decisions

1. **Metrics are NOT cached** by the pipeline step cache. `computeAllMetrics()` runs fresh every benchmark invocation. This means scorer changes don't invalidate pipeline caches.

2. **Metric versions** tracked per-module in `runs.metric_versions` JSON column. Enables comparing results across scorer versions regardless of git state.

3. **N/A vs 0**: Metrics that can't be computed (missing GT, missing pipeline output) return `null`, not 0. The UI shows these as hatched N/A bars with reason text, not as zero scores.

4. **No-GT composite**: When ground truth is unavailable (real buildings), the scorer switches to a geometry-only composite using residual + alignment + edge metrics.

## Test coverage

- `test/scorer-v3.test.js` — V3 scorer integration tests
- `test/edges.test.js` — edge metric unit tests
- `test/metrics.test.js` — updated for V3 versions
- All 1749 tests passing, 0 failures
