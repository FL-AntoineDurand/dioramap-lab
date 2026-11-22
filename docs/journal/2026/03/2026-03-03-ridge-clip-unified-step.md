---
id: "2026-03-03-ridge-clip-unified-step"
date: 2026-03-03
title: "Ridge-Clip: Unified Snap/Clip/Edge Step Replaces Two-Step Sequence"
tags: [ridge, hull, pipeline, refactor]
category: development
status: concluded
refs: ["2026-03-03-immutable-artifact-refactor", "2026-03-03-multi-source-edge-detection"]
---

## Problem

The former `parallel-edge-clip` + `sharp-edges` two-step sequence had a
fundamental issue: `parallel-edge-clip` trimmed hull overshoots at ridges,
then `sharp-edges` recomputed edges from scratch. The recomputation ignored
the adjacency filtering logic and sometimes produced spurious edges between
non-adjacent planes.

## Solution

New unified `ridge-clip` step (v2) replaces both steps. For each adjacent
plane pair:

1. Compute plane-plane intersection line (ridge)
2. Project both hulls onto the ridge to find overlap
3. Snap nearby hull vertices to the ridge (fills gaps ≤ `ridgeSnapDist`)
4. Clip overshooting hull edges against the ridge
5. Emit the edge segment for the overlap region

### Adjacency Filtering

Only processes plane pairs where `hullMinDistXZ < ridgeAdjacencyDist`.
Skips obstacle planes (from `result-obstacle-indices`). This prevents
false ridges between non-adjacent planes that happen to be coplanar.

### Artifacts

| Artifact | Type | Description |
|----------|------|-------------|
| `planes` | planes | Hulls with vertices snapped/clipped to ridges |
| `edges` | edges | Ridge edge segments with `source: 'ridge-clip'` |

> **Note** (2026-03-04): Snapshot artifacts `planes-ridge-clipped` and `edges-computed` were removed in the qualified-names refactor. Intermediate states are now accessible via qualified names (e.g. `ridge-clip#0.planes`).

### Params

| Param | Default | Description |
|-------|---------|-------------|
| `ridgeAdjacencyDist` | 2.0 m | Max hull-to-hull distance for adjacency |
| `ridgeSnapDist` | 0.8 m | Max distance to snap vertices to ridge |

## Close-Corner-Gaps Update (v4)

Updated `close-corner-gaps` to reproject existing edges onto updated hulls
instead of recomputing from scratch via `computeSharpEdges`. This preserves
ridge-clip's adjacency decisions through the corner-closure step.

New `reprojectEdges()` function: for each edge's plane pair, recomputes the
intersection line and finds the hull-projection overlap on the updated
(corner-extended) hulls.

## Files

| File | Action |
|------|--------|
| `lib/pipeline/steps/ridge-clip.js` | **NEW** — unified step (v2) |
| `lib/pipeline/steps/close-corner-gaps.js` | **EDIT** — v2→v4, edge reprojection |
| `lib/pipeline/steps/parallel-edge-clip.js` | Still exists but replaced by ridge-clip in V6 pipelines |
| `lib/pipeline/steps/sharp-edges.js` | Still exists but replaced by ridge-clip in V6 pipelines |
