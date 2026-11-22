---
id: "2026-05-24-bp3-implementation"
date: 2026-05-24
title: "bp3 implementation — v3 v7-boundary-polygons-2 rebuilt around grid-fill contour topology"
tags: [boundary-polygons, contour, topology, v7, v7-boundary-polygons-2]
category: development
status: concluded
refs:
  - "2026-05-22-bp3-grid-fill-topology"
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-05-21-bp2-autonomous-run-report"
pipeline: v7
---

## What landed

bp3 is now the v3 implementation of `v7-boundary-polygons-2`. The bp2
top-level (components → subset enum → final cycle) is gone; the lower
layers (cost function, WSD-multi, spike penalty, `best_cycle` TSP solver,
all 12 sub-1a unit fixtures) carry over unchanged. The step name is
unchanged — `v7-boundary-polygons-2` — because the architecture promised
by the May 21 rethink (TSP, not Dijkstra; multi-start, not adjacent-swap)
is what bp3 actually delivers; bp2's classification/subset-enum was the
detour, not the destination. Per the conversation that produced this:
*"reuse bp2, which is mostly unusable anyway"*. One commit, no
A/B-comparable bp3 step proliferation.

## Architecture

```
v7_bp2_compute(inputs):
    project each anchor to plane UV (once)
    for each contour in inputs.contours:
        subset = { anchor : pt_edge_dist(anchorUV, contour) ≤ 0.75m }
        if |subset| < 3: skip
        cycle = best_cycle(subset, FUNCTIONAL mode)   // multi-start TSP
        if cycle valid:
            emit polygon(kind = OUTER if !contour.is_hole else INNER,
                         corner_ids = cycle in walk order,
                         wsd / compactness / spike_penalty diagnostics)
```

No component merging, no subset enum, no orientation classification.
Topology is a *property* of the upstream `RG_SECT_PLANE_CONTOURS`
section emitted by `v7-boundary-signals` — each closed loop becomes
exactly one polygon. Holes are emitted as separate polygons with
`kind = INNER`, not buried in an outer's `holes[]` field (the wire
format already supports independent polygons per contour; the
"polygon-with-holes" model was bp2's). That sidesteps the K>1 wire-up
question deferred in the rethink.

### What disappeared from bp2

| Removed | Lines | Why |
|---------|-------|-----|
| `walk_balance_field` | 130 | topology now comes from upstream contour |
| `collect_components` + `free_components` | 70 | no per-fragment component sets |
| `subset_anchors` | 20 | no 2^K enum |
| `v7_bp2_compute` subset-enum body | 125 | replaced by per-contour TSP loop |
| `v7_bp2_balance_field_t` struct | 6 | input gone |
| `v7_bp2_component_override_t` struct + test hook | 11 | input gone |
| `V7_BP2_BALANCE_THRESHOLD` global + param | 4 | walk gone |

Net deletion: ~370 lines in `v7-boundary-polygons-2-impl.{c,h}`. Net
addition: ~120 lines for the new contour-driven `v7_bp2_compute`. The
new code is dominated by glue (project anchors, look up by distance,
emit per-contour); the algorithmic heavy-lifting is in the
*unchanged* lower layers.

## Step wrapper + bench + test

- **Step wrapper** (`v7-boundary-polygons-2.c`): added `parse_plane_contours`
  reading `RG_SECT_PLANE_CONTOURS` records (header 40 bytes + outer F32
  UVs + per-hole F32 UV blocks, all converted to F64 for the impl).
  `process_plane` now looks up the plane's contour record by `plane_id`
  and builds a flat `v7_bp2_contour_t[]` = `[outer] + holes`. The
  `balanceThreshold` JSON param is dropped from `parse_params`. Step
  version bumped 1→2.
- **Iso bench** (`v7-boundary-polygons-2-only-bench.c`): stubbed to pass
  `n_contours = 0` (returns +1 no polygon). The bench was a v2-parity
  comparison tool; v2-parity is no longer the success criterion (see
  May 21 rethink), so the bench is non-functional until bench-v2 starts
  dumping contour data. TODO comment in place.
- **Test harness** (`v7-bp2-test.c`): replaced `balance_field` +
  `components_override` JSON loaders with a unified `load_contours`
  helper that reads `inputs.contours[].polyU/polyV/is_hole`. Stability
  path (σ=1mm perturbation, ≥99/100 match) gets the same contours
  across all runs — only anchors perturb.

## Fixtures

**Deleted (tested the bp2 dead path):**
- `t5-signal-fragmentation` — multi-component merge is a bp2 concept
- `t7-spurious-noise-component` — subset enum was the noise filter
- `t9-walk-one-ring` — walk-specific
- `t10-walk-real-signal-hole` — walk-specific
- `t11-walk-interior-hole` — walk-specific

**Rewritten with `contours` input:**
- `t1-clean-square` — outer = unit square; 4 anchors at corners
- `t2-clean-square-noise` — stability, same outer contour
- `t3-extra-collinear-corner` — outer = 5-vertex (4 corners + 1 mid-edge);
  expect 5-vertex polygon (no deletion — that's regularize's job)
- `t4-interior-hole` — outer + 1 hole; under bp3 → 2 polygons (1 OUTER, 1 INNER)
- `t8-outer-with-two-holes` — outer + 2 holes; under bp3 → 3 polygons

**Added:**
- `t12-no-anchors-in-band` — contour exists but anchors all >0.75m away;
  expect empty output (no fabricated polygons)
- `t13-stability-with-hole` — K=2 stability oracle (outer + hole, σ=1mm)

**Kept unchanged:** all 12 sub-1a unit fixtures (wsd / cost / spike /
best-cycle). They test bp3's lower layers, which bp3 reuses verbatim.

## Validation

`ctest -R v7_bp2`:

```
Summary: 20 passed, 0 failed, 0 skipped
```

The two stability fixtures (`t2-clean-square-noise` K=1 and
`t13-stability-with-hole` K=2) both hit 100/100 runs match at σ=1mm —
well above the ≥99% gate. The new success criterion is met.

Open: end-to-end dahouet validation through the step wrapper (planes
43/47/48 — the ghost-mostly ones the n_pts gate relaxation just admitted
— should now produce contour-driven polygons; previously they bailed
out). Tracked separately as task #308.

## What this changes downstream

The polygon-emit wire format (POLYGON_PATHS section) is unchanged —
bp3 produces the same `corner_ids[]` cycle records bp1/bp2 did. So
`boundary-merge`, `boundary-classify`, and `boundary-regularize`
consume bp3 output without modification. The behaviour change is
purely in WHICH cycle gets emitted, governed now by:

- Which closed contour the corner pool projects onto (deterministic —
  contour positions are cells-apart, not LSBs-apart, so corner-to-contour
  assignment is stable under sub-mm noise)
- best_cycle's multi-start TSP search (bp2's solver, unchanged)

Both are dramatically more stable than bp1's `floor()`-cell-index walk +
arc-length tie-break. The May 21 finding that bp1 was "a coin flip on
FP rounding" no longer applies.

## Refs

- v3 commits this session: (this work — pending commit at journal-write time)
- bp3 design: [`2026-05-22-bp3-grid-fill-topology`](./2026-05-22-bp3-grid-fill-topology.md)
- TSP rethink: [`2026-05-21-boundary-polygons-tsp-rethink`](./2026-05-21-boundary-polygons-tsp-rethink.md)
- bp2 autonomous run report: [`2026-05-21-bp2-autonomous-run-report`](./2026-05-21-bp2-autonomous-run-report.md)
