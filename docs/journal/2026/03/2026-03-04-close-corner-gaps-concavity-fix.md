---
id: "2026-03-04-close-corner-gaps-concavity-fix"
date: 2026-03-04
title: "close-corner-gaps v5: Preserve Concave Hulls via Polygon Union"
tags: [pipeline, hull, bugfix]
category: bugfix
status: concluded
refs: ["2026-03-03-wall-roof-detect-v10-two-phase"]
pipeline: v6g-kde-full
---

## Problem

`closeCornerGaps` computes new corner vertices (ridge-ridge, ridge-eave, and
eave-eave intersections), then merges them with existing hull vertices and runs
`convexHull3DOnPlane`. This destroys any concavity in the original hull.

On the church test case, the main roof planes have concave (L-shaped) hulls from
alpha-shape extraction. After close-corner-gaps, they became rectangular convex
hulls — visually filling areas where no roof exists.

## Fix

Replace `convexHull3DOnPlane` with `polygon-clipping.union` (Martinez-Rueda-Feito
boolean ops, already imported in the file).

For each new vertex outside the hull:
1. Find the 2 nearest hull vertices
2. Form a triangle (new vertex + 2 nearest hull vertices)
3. Skip degenerate triangles (area < 1e-8)

Then union all triangle patches with the original hull polygon in one call.
This extends the hull locally at each corner while preserving the concave
boundary elsewhere.

## Verification

Church plane 0 (main roof, L-shaped):
- Before: 10 vertices, area = 166.22 m²
- After v5 (polygon union): 12 vertices, area = 166.56 m² (+0.2%)
- After v4 (convex hull): would inflate area significantly by filling the notch

Gable benchmark: 24/24 pass, avg 92.3 (+0.2 vs v4).
- `gable_with_parapet-small`: 82.7 → 90.3 (+7.6, 2p → 4p)
- `gable_with_parapet-large`: 81.7 → 79.1 (-2.6)

## Files Changed

- `lib/postprocess/plane-intersection.js` — `closeCornerGaps()` reconvex replaced
  with polygon union
- `lib/pipeline/steps/close-corner-gaps.js` — version 4 → 5
