---
id: "2026-06-29-regularize-corner-insertion-edge-fit-metric"
date: 2026-06-29
title: "Idea: in-loop corner insertion in regularize, driven by a per-edge fit-misfit metric"
tags: [boundary-regularize, cost-function, wsd, corner-insertion, edge-misfit, compactness, repair, idea, v7]
category: idea
status: concluded
refs:
  - "2026-07-02-corner-insertion-lands"
  - "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
  - "2026-06-20-in-loop-make-shared-cascade-complete"
  - "2026-05-30-bp3-contour-arclength-ordering"
pipeline: v7-bp2
---

> **Realized 2026-07-02** — built and validated in
> [[2026-07-02-corner-insertion-lands]] (implementation + results). This entry is the
> original design/idea; see the companion for what actually shipped.

## Motivation

Boundary-regularize today can **move** and **merge/split** corners, but it cannot
**add** a corner mid-edge. So when an upstream polygon is under-built — e.g. plane 0 on
dahouet, whose polygon covers ~28 % of its 4807 points with 72 % sitting 5–20 m outside
(see [[2026-06-29-corner-slide-runaway-and-wsd-coverage-audit]]) — regularize has no good
move available. Its only lever to reduce the coverage cost (WSD `own_out`) is to drag an
existing vertex, which degenerates into a spike to infinity.

The idea: give regularize the ability to **insert a new corner on a polygon edge that is
locally fitting points badly, in the loop**, bowing the edge outward to actually engulf
the uncovered points. This recovers bad geometry instead of papering over it, and — key
insight — it **removes the incentive** for the spike: a proper bow reduces `own_out` far
more than any sliver, so a greedy descent prefers it. It is the in-loop dual of what
`boundary-polygons-2` does at construction time.

This entry is the design sketch; nothing is implemented yet.

## The core question: can we measure how well/badly an edge locally fits points?

Yes — and the metric falls straight out of the existing WSD machinery. `own_out` already
iterates every owner point outside the polygon and computes its **distance to the nearest
edge** (`pt_edge_dist_uv`). Today that returns only the distance. Extend it to also return
the **realizing edge index**, and bucket the misfit per edge:

```
edge_misfit(e) =  Σ  w·d   over OWNER points OUTSIDE whose nearest edge is e   (wants to bow OUT)
              −   Σ  w·d   over FOREIGN points INSIDE  whose nearest edge is e   (wants to bow IN)
```

This is literally a **per-edge decomposition of the own_out / foreign_in WSD we already
sum** — same loop, just histogram by edge instead of accumulating one scalar. So it costs
essentially nothing on top of the existing WSD pass. Interpretation:

- **Large positive `edge_misfit`** → the edge is slicing through its own point cloud,
  leaving a mass outside → candidate for **insert-and-bow-out** (plane 0's exact failure).
- **Large negative** → the edge over-reaches across foreign points → bow inward.

Report two flavours: the **total** per edge (which edge to fix) and the **per-length
density** (how concentrated the misfit is). A long edge that legitimately bounds a long
straight wall should *not* trigger just for being long.

## The insertion operation (sketch)

An in-loop sibling of the T7 `SPLIT_TO_SHARE_CORNER` op
([[2026-06-20-in-loop-make-shared-cascade-complete]]):

1. Pick the edge with the worst `edge_misfit` above a threshold.
2. Insert a vertex at the projection of the attributed outside-mass centroid onto the
   edge; push it toward that mass (e.g. to a far percentile of the attributed points).
3. Hand it to the normal coordinate-descent to refine.

Guards (reuse what T7 already established):

- **Self-intersection reject** (`v7_local_edges_cross_uv`, the M5 tier-1 test T7 uses).
- **Shape guard** — compactness/spike so the new vertex *bows* rather than *spikes*
  (this is precisely why a regularize compactness/anti-spike term is a prerequisite, see
  the parent entry's tracks 1–2).
- **Commit only if total cost strictly drops.**
- **Threshold-gated** so it does not over-fragment or fight bp2's job — a repair of last
  resort, not a routine pass.

## Why it is well-posed (and why it needs the other tracks)

The energy becomes a small active-contour problem:

- **coverage (`own_out`)** supplies the outward pull,
- **compactness / anti-spike** supplies the "stay smooth, don't spike" counter-force,
- **corner insertion** supplies the *topological freedom* to actually achieve coverage
  without a degenerate vertex.

Without insertion, the shape term alone can't grow a polygon that is genuinely too small.
Without the shape term, insertion just makes fancier spikes. They are complementary, not
alternatives — which is why this is filed as a track *alongside* normalization and
anti-spike in the parent entry, not as a standalone fix.

## Open design questions

- **Attribution for deep concave pockets.** Nearest-edge is a fine v1, but for a point
  deep in a concavity the nearest edge may not be the one that "should" contain it. A v2
  could attribute by "which edge's supporting line the point lies beyond."
- **Where/how far to push** the new vertex (centroid vs far-percentile vs convex-hull of
  the attributed mass), and whether to insert one vertex or several for a large misfit.
- **Interaction with make-shared** — an inserted corner on a shared edge must respect the
  cross-plane sharing invariants (it may itself become a shared corner).
- **Scope** — repair-of-last-resort vs a routine refinement; and whether it should be
  gated behind the same density-scaling as the rest of regularize.
- **Does it subsume the ridge clamp?** If insertion reliably offers a better-than-spike
  move, the clamp may become redundant — but only once normalization makes the descent
  fair enough that the better move actually wins.
