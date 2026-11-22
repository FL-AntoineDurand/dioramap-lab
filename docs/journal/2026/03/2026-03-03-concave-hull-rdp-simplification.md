---
id: "2026-03-03-concave-hull-rdp-simplification"
date: 2026-03-03
title: "Concave Hulls with RDP Simplification Replace Convex Hulls"
tags: [hull, alpha-shape, geometry, architecture]
category: development
status: concluded
refs: ["2026-03-03-immutable-artifact-refactor"]
---

## Problem

All plane hulls were computed as convex hulls (`convexHull3DOnPlane`). This
over-approximates L-shaped, U-shaped, and other concave roof sections,
causing false overlaps and incorrect area estimates.

## Solution

Switched `createPlaneResult()` in `_common.js` from `convexHull3DOnPlane` to
`concaveHull3DOnPlane` (alpha-shape). The alpha-shape produces concave
boundaries that respect the actual point distribution.

### RDP Simplification

Raw alpha-shape boundaries have noise-level wiggles. Added Ramer-Douglas-Peucker
(RDP) polygon simplification to `alpha-shape.js`:

1. Find the two farthest-apart vertices as anchor points
2. Split ring into two arcs at anchors
3. Recursively simplify each arc with epsilon = mean edge length
4. Preserve real concavities while removing noise wiggles

**Key functions added to `lib/geometry/alpha-shape.js`:**
- `rdpSimplifyRing(pts, indices, epsilon)` — closed-ring RDP with dual-anchor split
- `rdpArc(pts, indices, arc, epsilon, keep)` — recursive arc simplification

### Integration

In `concaveHull3DOnPlane()`, after computing the alpha-shape boundary polygon,
RDP runs with `epsilon = meanEdgeLength` of the boundary. If simplification
reduces the polygon below 3 vertices, the original polygon is kept.

## Files Changed

- `lib/geometry/alpha-shape.js` — added RDP functions, integrated into `concaveHull3DOnPlane`
- `lib/algorithms/_common.js` — `createPlaneResult` now imports and uses `concaveHull3DOnPlane`
