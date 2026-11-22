---
id: "2026-03-03-ridge-edge-fit-segment-pairs"
date: 2026-03-03
title: "Ridge Edge Fitting: Segment-Pair Clustering Splits Multi-Ridge Junctions"
tags: [edges, ridge, clustering, finding]
category: finding
status: concluded
refs: ["2026-03-03-multi-source-edge-detection", "2026-03-02-cascade-step-split"]
---

## Problem

`ridge-edge-fit` v1–v2 used pure spatial clustering on multi-source ridge
candidate points. On the church test case (proc-church-large-standard), the
main roof ridge (777 points, elongation 2058) was detected perfectly, but the
tower's 4 ridges (300 points total) merged into a single round blob
(elongation 1.0) and were rejected by the elongation filter.

**Root cause**: The 4 tower ridges meet at the apex. Their candidate points
are spatially adjacent in 3D (within `connectDist`), so BFS-based connected
components merges them into one cluster. A round cluster has elongation ~1
and fails the minimum elongation check.

## Diagnostic

Per-segment-pair analysis revealed 4 clean ridges hidden inside the blob:

| Pair | Points | Elongation | Length |
|------|--------|------------|--------|
| 2–4 | 78 | 32.7 | 4.4 m |
| 2–3 | 76 | 34.5 | 4.4 m |
| 4–5 | 71 | 28.1 | 4.4 m |
| 3–5 | 75 | 27.4 | 4.4 m |

Each ridge borders exactly two plane segments. The segment ownership
information (from `owner`) provides the discriminating signal
that spatial distance alone cannot.

## Solution (v3)

**Segment-pair-aware clustering**: Before spatial clustering, group each
candidate point by its (ownSegment, dominantNeighborSegment) pair.

For each candidate:
1. Look up its segment from `owner`
2. Query k-NN in the full point cloud
3. Find the most common *different* segment among neighbors
4. Group by pair key: `min(own, neighbor) + '-' + max(own, neighbor)`
5. Cluster + fit edges within each pair group independently

**Result**: 1 → 5 edges (main roof 35.25m + 4 tower ridges ~4.4m each).

**Fallback**: If `owner` or `_kdTree` not available (non-V6
pipelines without cascade), falls back to pure spatial clustering.

## Key Insight

Ridge points carry implicit segment-boundary information. Using segment
ownership to partition candidates before clustering avoids the fundamental
problem of spatial proximity at multi-ridge junctions (tower apexes,
hip-ridge intersections, etc.).
