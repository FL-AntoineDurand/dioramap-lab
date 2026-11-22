---
id: "2026-03-10-bridge-vertex-concave-hull-fix"
date: 2026-03-10
title: "Bridge vertex fix in alpha-shape concave hulls"
tags: [alpha-shape, concave-hull, 3d-rendering, bugfix]
category: bugfix
status: concluded
refs: ["2026-03-03-concave-hull-rdp-simplification"]
---

## Bug

Alpha-shape concave hulls produced self-touching boundaries at dormer concavities — two vertices at the same 3D position ("bridge vertices"). This caused hull-extraction plane 0 on `proc-dormer_house-large` to render as a tiny ~1 m² patch instead of its actual 345 m².

## Fix

### Source fix (lib/geometry/alpha-shape.js)

- `stripBridgeVertices()`: recursively removes concavity loops
- Area recomputed from cleaned hull using tangent-basis projection
- All downstream consumers (seed, hull-extraction) get clean polygons

### UI defense-in-depth (ui/public/app.js)

- Removed broken frame polygon detection (false-triggered on bridges)
- `splitAtBridges()` for recursive bridge splitting
- `projectToPlane2D()` using proper tangent-basis projection (replaces axis-dropping which collapsed tilted roof geometry)
- Rewrote `earClipTriangulate3D()` with force-ear fallback

### Version bumps

`hull-extraction` v6→v7, `normal-cascade-seed` v6→v7 (cache invalidation).

Files: `alpha-shape.js`, `hull-extraction.js`, `normal-cascade-seed.js`, `app.js`. 4 files, +197 −116 lines.
