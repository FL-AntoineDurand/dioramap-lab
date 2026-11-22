---
id: "2026-05-21-bp2-autonomous-run-report"
date: 2026-05-21
title: "v7-boundary-polygons-2 — Autonomous Run Report (Tasks #295/#296/#297-partial)"
tags: [v7-boundary-polygons-2, tsp, autonomous, status-report]
category: review
status: concluded
refs: ["2026-05-21-boundary-polygons-tsp-rethink"]
---

## Summary

Three autonomous tasks executed against the scope contract in
`v3/docs/plan/BOUNDARY-POLYGONS-2-TEST-SCOPE.md`:

| Task | Gate | Status |
|---|---|---|
| #295 — cost fn + WSD-multi + cost/wsd/spike unit tests | sub-1a green | **PASS** |
| #296 — best_cycle TSP solver | sub-1a best_cycle green | **PASS** |
| #297 — components/classify/merge/e2e | all 6 sub-1b green | **PARTIAL** (4/6) |

20 fixtures green; v3/build-noasan ctest for `v7_bp2_test` green. Total
4 commits this run: `ad4aceb`, `7c1b672`, `f4ce10e`, `95dcef8`.

## What's running

**Unit fixtures (15 sub-1a):**
- WSD: zero-perfect-enclose, known-outside-distance, two-sided-nearby-inside, with-hole-excludes-correctly
- Spike: acute-penalised, 90deg-zero, flat-not-penalised
- Cost order: square-vs-bowtie (self-intersect = +∞), thin-rect-vs-square (compactness), square-vs-hexagon (compactness)
- Classify: outer-known, inner-known, ambiguous-equal
- Best-cycle: three-corners-trivial (n=3), four-square (n=4 full enum), self-cross-avoided (n=12 multi-start)

**End-to-end (4/6 sub-1b):**
- T1 clean square — 4 anchors → CCW outer.
- T2 stability — 100 runs, σ=1mm corner noise → 100% match rate (well above ≥99% gate).
- T3 collinear corner — 5 anchors with extra vertex on top edge → 5-vertex polygon (no deletion).
- T4 interior hole — 8 anchors (outer + hole), classify INNER on hole → emit 4-vertex outer.

## Deferred (gate items not yet green)

**T5 signal-gap, T6 rotated-noisy:** require either:
- a balance-field walk that produces multiple components which must
  be **merged by proximity** when together they form a closed cycle, or
- a rotation/translation harness for the noise stress test.

Today's component-union logic skips any component with n<3 anchors,
so two 2-anchor fragments of a broken ring get dropped instead of
merged. Fixing this needs either:
1. A pre-classification component-union pass keyed on
   "endpoint anchors are within `V7_BP2_CORNER_TO_FRAGMENT_RANGE_M`",
   which is a heuristic, not the spatial-contour-adjacency the user
   sketched in the journal. Or:
2. Punt to the real balance-field-walking pass (task #297b) where
   the connected-component machinery does the job naturally.

I picked #2 — defer to #297b — since it's the load-bearing path
in production anyway.

## Public API additions to v7-boundary-polygons-2-impl.h

```c
typedef struct v7_bp2_component_override {
    const uint32_t* anchor_ids;
    size_t          n_anchor_ids;
} v7_bp2_component_override_t;

// In v7_bp2_inputs_t:
const v7_bp2_component_override_t* components_override;
size_t                             n_components_override;
```

`components_override` is the Tier-1 testing hook that bypasses
balance-field walking. Real-world callers (the step wrapper, bench,
pipeline) will leave it NULL and supply `balance_field` instead.

## Decisions taken under "agent authority"

Per the scope doc, the agent has authority on these; the journal
captures them so the user can review:

1. **TSP solver is full permutation enumeration (Heap's algorithm) for
   small n, multi-start local search above.** Classical edge-cost DP
   wasn't applicable — the cost function isn't decomposable per edge
   (WSD depends on the whole polygon, not on consecutive-pair distances).
2. **V7_BP2_FULL_ENUM_MAX = 8.** 8!/2 = 20160 cycles per component,
   still fast enough to run per-plane on dahouet-scale inputs.
3. **Multi-start seeds = 8** (NN×2 + centroid-angle + 5 random).
4. **Cost weights kept at 0.8/0.1/0.1.** All cost-order fixtures
   pass; no tuning needed.
5. **Classify of n<3 components: skipped** (not merged into outer).
   This is what blocks T5. Documented as TODO above.

## Files touched

```
v3/steps/v7/v7-boundary-polygons-2-impl.h    (+15 lines)
v3/steps/v7/v7-boundary-polygons-2-impl.c    (+125 lines)
v3/steps/v7/v7-bp2-test.c                    (+200 lines)
tests/fixtures/v7-boundary-polygons-2/*.json   (+13 new fixtures)
```

## Pointer to next work

In task-list order:
- **#297 (continue):** decide T5/T6 strategy — proximity-union now,
  or wait for the balance-field walk in #297b.
- **#297b:** step wrapper + bench harness; replaces the K=1 fallback
  with the real balance-field walk.
- **#297c:** v2 JS port. Mirror the algorithm in
  `lidar-algos-bench-v2/lib/pipeline/steps/boundary-polygons-2.js`.
- **#297d, e:** Tier-2 integration tests + pipeline switch.
- **#294:** "design doc + dual-impl plan" — still pending; arguably
  superseded by the scope doc + this journal entry. Consider closing.
