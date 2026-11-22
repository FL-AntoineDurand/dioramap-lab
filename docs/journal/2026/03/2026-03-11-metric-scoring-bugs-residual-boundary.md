---
id: "2026-03-11-metric-scoring-bugs-residual-boundary"
date: 2026-03-11
title: "Two metric scoring bugs — residualScore and boundaryIoU"
tags: [metrics, bugfix, residual, boundary]
category: bugfix
status: concluded
refs: ["2026-03-07-scorer-v3-23-metrics", "2026-03-10-building-rms-edge-type-metrics"]
---

## Bug 1: residualScore always 0

`inlierIndices` (in positions-space, i.e. indices into the full point cloud) were used as direct indices into the filtered `buildingPoints` array, looking up wrong points. Every lookup hit near-zero residuals by coincidence.

Fix: build a `posIdx→bpIdx` reverse map via the `.idx` field that `extractBuildingPoints` stores on each point.

File: `lib/metrics/residual.js`, +26 −2 lines.

## Bug 2: boundaryIoU near-zero

`getXZBoundary()` preferred `boundary_2d_xz`, which is in plane-local tangent space (not world XZ despite the field name). The detected hull is in world XZ, so the comparison was cross-coordinate-space.

Fix: prefer `vertices_3d` projected to world XZ for flat planes; fall back to `boundary_2d_xz` only for curved surfaces where it genuinely is in world XZ.

File: `lib/metrics/boundary.js`, +27 −9 lines.

## Impact

`proc-dormer_house-large`: 71.6 → 91.0 (+19.4 pts). Both bugs affected all test cases with ground truth.
