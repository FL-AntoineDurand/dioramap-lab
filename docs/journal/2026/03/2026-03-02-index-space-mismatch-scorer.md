---
id: "2026-03-02-index-space-mismatch-scorer"
date: 2026-03-02
title: "CRITICAL: Index-Space Mismatch in Segmentation Scorer"
tags: [metrics, segmentation, v2-rewrite, regression]
category: bugfix
status: concluded
refs: ["2026-03-02-v6x-hybrid-converges-to-v2", "2026-03-02-v6-kde-stage1-wins-stage2-loses", "2026-02-26-row-houses-v6-breakthrough", "2026-02-26-cross-plan-structural-limitation"]
---

# CRITICAL: Index-Space Mismatch in Segmentation Scorer

## Question

Why do overnight benchmark scores (avg 52.4) differ drastically from expected V2 scores (~92)?

## Setup

Overnight campaign — v2-champion, v6a-base, v6d-progressive × 463 test cases. Scores were far below historical expectations. Investigation started by comparing with V1 benchmark data (v2-champion scored 92.0 on 240 synth cases in V1).

## Discovery

The V2 composite on synth was 60.8 (should be ~95). Even V1 composite on V2 data showed 51.0. Individual metric inspection revealed:
- 103/232 synth GT cases had **0% detection rate** despite correct plane counts
- `gable-clean`: 2 detected planes, 2 GT planes, detection_rate = 0.00
- `flat-clean`: seg_f1 = 0.40 (should be ~1.0), completeness = 0.70 (should be ~1.0)

## Root Cause

Two-index-space mismatch in `lib/metrics/segmentation.js`.

The function builds two sets for overlap comparison:
1. **GT point sets** (lines 47-53): iterates `buildingPoints[bpi]`, looks up `pointPlaneIds[origIdx]`
2. **Detected point sets** (line 168): uses `inlierIndices` from detected planes

The bug: GT sets stored building-point array indices (`bpi` = 0, 1, 2, ...) while detected sets stored original cloud indices from `inlierIndices` (e.g., 6188, 6189, ...). These are different index spaces — `bpi` is a compact filtered array, `origIdx` is the position in the full point cloud including non-building points.

The overlap computation compared `{0, 1, 2, 3, ...}` against `{6188, 6189, 6190, ...}` — zero intersection on most cases.

**Why V1 was unaffected**: V1's algorithms populated `inlierIndices` in building-point space (indices into the RANSAC `remaining` pool, which starts as the full building-points array). So both GT and detected sets accidentally used the same wrong-but-consistent index space. V1's 98.9 score was valid.

**Why V2 broke**: V2's `hull-extraction.js` (line 61) stores `denoisedToOrig[i]` — original cloud indices — in `inlierIndices`. This was a correct architectural improvement (original indices are the canonical identity), but `segmentation.js` was copied from V1 without updating the GT side to match.

## Fix

2 lines:
```
segmentation.js line 51:  add(bpi) → add(origIdx)     // GT sets now use original cloud indices
segmentation.js line 212: add(bpi) → add(buildingPoints[bpi].idx)  // reconstruction path too
```

## Verification

After fix, quick test (10 cases) avg score jumped from 49.0 to 95.6. `gable-clean` detection_rate: 0.00 → 1.00. `flat-clean` seg_f1: 0.40 → 1.00.

## Impact on Existing Data

| What | Affected? | Details |
|------|-----------|---------|
| Database results | YES | All 6,049 GT-dependent results in bench.db (15 run_pipelines, 4 runs) have wrong values for: detection_rate, seg_f1, boundary_iou, plane_accuracy, composite, composite_v2 |
| Non-GT metrics | NO | fp_overshoot, plane_overlap, fp_coverage, residual_score, completeness are computed independently of segmentation matching |
| Plane counts | NO | The algorithm is correct; only the scorer was wrong |
| Relative comparisons (W/T/L) | PRESERVED | The bug affected all pipelines identically (same scorer). W/T/L direction between pipelines is still valid |
| Absolute scores | WRONG | All absolute scores in the DB and in lab notes entries from 2026-03-02 need recomputation |
| Prior lab notes (2026-02-25..27) | PARTIALLY | Plane count comparisons (row houses +62%, cross-plan -23%) are valid. Absolute scores are wrong. Relative conclusions survive. |

**Git history**: Bug introduced in commit `b6d8c65` ("raw savage commit") — the initial V2 rewrite. Only 2 commits ever touched `lib/metrics/`: `b6d8c65` (introduced) and `cacfcd2` (cleanup, didn't fix). The bug existed since V2 was created.

**Similar patterns**: Searched entire V2 codebase — no other index-space mismatches found. The `buildingPoints[i].idx` vs loop-variable-`i` pattern is used correctly in all other files (metrics/, pipeline/steps/).

## Lessons

1. When refactoring index spaces (V1→V2 changed inlierIndices from building-point to original-cloud indices), ALL consumers must be updated. The scorer was a consumer that got missed.
2. A "smoke test" comparing known-good scores would have caught this immediately: if `flat-clean` (1 plane, 1 detected) doesn't score ~100, something is fundamentally wrong.
3. Relative W/T/L comparisons survive scorer bugs (same bug affects all pipelines equally), but absolute scores do not. Always sanity-check absolute values against expectations.

## Version Strategy

Per-metric version tracking. Each `lib/metrics/*.js` exports a `VERSION` constant. These are collected by `getMetricVersions()` and stored as JSON in `runs.metric_versions`. No pipeline step version bump needed — metrics are not pipeline-cached.

Existing DB runs backfilled with `segmentation: 1` (all v1). New runs record `segmentation: 2`.

## Follow-ups

- [x] Fix segmentation.js (2 lines)
- [x] Add 3 unit tests: offset-index regression, multi-plane offset, reconstruction path (`test/metrics-detail.test.js`)
- [ ] Re-run overnight campaign with corrected scorer
- [ ] Update absolute scores in earlier entries after re-run
