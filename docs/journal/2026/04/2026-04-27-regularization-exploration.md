---
id: "2026-04-27-regularization-exploration"
date: 2026-04-27
title: "Building Geometry Regularization: Attempts, Failures, and Conclusions"
tags: [boundary-3d, regularization, architecture, constraints, roof-graph]
category: finding
status: concluded
refs: ["2026-04-14-roof-graph-prepass-boundary-3d"]
pipeline: v7
---

## Context

After splitting the boundary-3d monolith into 5 steps (boundary-signals,
boundary-corners, boundary-polygons, boundary-merge, boundary-classify),
we scaffolded a `boundary-regularize` step to improve building geometry
by snapping angles to well-known values (90/45/135/180), making ridges
horizontal, and aligning downslopes perpendicular to ridges.

Test case: dahouet (real-22186-dahouet-502).

## Attempt 1: Per-plane corner nudge (Pass 2, D.7b)

**Approach**: For each corner in a plane's polygon, sample positions
along the bisector +/-15cm, score by wrongSideDist, accept if WSD
improves.

**Result**: Works for single-plane corners. Frozen shared corners
(ridge, coplanar, multi-plane) can't be touched.

**Wall hit**: The most interesting regularization targets (ridge
junctions where 3 planes meet, shared edges) are exactly the corners
that are frozen. Per-plane optimization can't reach them.

**Pivot**: Need a global, cross-plane optimizer.

## Attempt 2: Global corner optimizer (R.1+R.2)

**Approach**: Greedy coordinate descent on ALL corners simultaneously.
Cost = WSD + lambda_angle * angle_deviation^2 + lambda_disp *
displacement^2. Candidates: bisector, edge-aligned, angle-constrained
(target 90/45/135/180). Building-wide, not per-plane.

**Results**:
- 50 iterations, 50 moves, 134 corners, 215 angles
- First version: unbounded displacement, corners drifted 3.9m. Fixed
  with 0.15m cumulative cap.
- Plane constraint added: 1-plane corners projected onto plane,
  2-plane corners projected onto ridge line, 3+ plane corners frozen.

**Wall hit**: 3+ plane corners (where ridge lines meet) are fully
constrained by the intersection of their plane equations. They sit at a
unique point determined by the planes. The optimizer can't move them
without violating plane membership. These are the corners with the most
important angles to regularize (building corners, hip junctions).

**Key insight**: You can't fix the angles at a 3-plane corner by moving
the corner. The only way is to change the plane equations themselves,
which repositions the intersection point.

**Pivot**: Need to regularize plane equations first.

## Attempt 3: Plane equation regularization (R.0)

**Approach**: Parameterize each plane as (tilt, azimuth, offset). For
near-horizontal ridges, adjust the two planes' azimuths so their normal
cross product has zero Y component. Then recompute everything: ridge
lines, corner positions, polygon vertices.

### Sub-attempt 3a: Pairwise azimuth averaging

**Approach**: For each near-horizontal ridge, average the two planes'
azimuths (weighted by point count).

**Wall hit**: A plane with multiple ridge partners gets its azimuth
overwritten by each partner in sequence. The last write wins. Planes
like P5 (connected to P23, P32, P17, P21) oscillate chaotically.

### Sub-attempt 3b: Iterative weighted circular mean

**Approach**: Each ridge gives a "target azimuth" for each plane. A
plane with multiple targets resolves via weighted circular mean (weight
= partner surface area). Iterate with 0.5 damping until convergence.

**Result**: Converges in 15-18 passes to ~0.01 degree residual. Much
better.

**Wall hit**: Hip ridges (planes meeting at 45-90 degree angles, not
opposite-facing) can't be made horizontal by azimuth adjustment. The
constraint sin(azB - azA) = 0 requires azA = azB (mod pi), but hip
planes have azimuth differences of ~45-90 degrees. Filtering them out
reduced candidates from 13 to 7 ridges.

### Sub-attempt 3c: Ridge/corner recomputation cascade

**Wall hit**: After adjusting plane equations:
1. Ridge from/to positions weren't updated (only ridgeLine.line was)
   - Fix: project old from/to onto new line
2. Corners on adjusted planes need reprojection
   - Fix: project 1-plane corners onto plane, 2-plane corners onto
     ridge line
3. But two ridges meeting at a corner, both made horizontal, may no
   longer intersect at the same point. The corner can't lie on both
   horizontal lines if they diverged.
4. The cascade is fragile: each fix introduces new inconsistencies.

**Final wall hit**: Adjusting plane equations independently, then trying
to reconcile corners, is fundamentally wrong. The system is coupled:
planes, ridges, and corners form a mesh, and you can't move one type
of element without breaking the others.

## Attempt 4 (designed but not implemented): Corner-only optimizer

**Approach**: Only move corners. Derive everything else:
- Plane equations = best-fit through polygon's corner positions
- Ridge directions = from edge geometry
- Score moves by: angle quality + ridge horizontality + edge
  horizontality + WSD + displacement

This dissolves the 3-plane constraint: the corner moves freely, and
plane equations float to accommodate. No cascade.

**Not implemented** because of the realization below.

## Conclusion: Regularization is wrong for internal features

Buildings are not perfect geometry. Roofs sag, walls lean, ridges bow.
Especially old buildings (like dahouet, a Breton stone house). Forcing
mathematical regularity onto a structure that is physically irregular
is fighting the data.

The hip ridges that refused to become horizontal ARE NOT horizontal in
reality. The angles that don't match 90 degrees ARE NOT 90 degrees on
the actual building. The LiDAR is telling us the truth.

### What makes sense

Regularization should only apply to **free features** that don't
conflict with the measured geometry:

1. **Perimeter edges**: the outer boundary of each polygon, where no
   ridge constrains the position. These can be straightened, right-
   angled, and aligned without affecting internal topology.

2. **Free corners**: corners belonging to a single plane, not shared
   with any ridge partner. These can be nudged to improve polygon
   compactness and angle quality.

3. **Eave alignment**: horizontal eave edges from different planes at
   nearly the same height can be snapped to a common elevation. This
   is aesthetic regularization, not geometric — it doesn't change the
   roof planes.

### What does NOT make sense

1. **Forcing ridge horizontality** when the measured ridge slopes.
2. **Forcing 90-degree angles** at hip junctions where the actual
   angle is 88 or 92 degrees.
3. **Adjusting plane equations** to match idealized geometry when the
   point cloud says otherwise.

### What to keep

- The **visualization infrastructure** is valuable: angle arcs/labels,
  ridge slope labels, horizontal bars, downslope arrows, perpendicular
  squares. These SHOW the geometry without changing it. Keep them for
  inspection and quality assessment.

- The **per-plane corner nudge** (D.7b in boundary-polygons) is fine:
  it operates on free corners within a single plane, improving polygon
  quality without touching shared features.

- The **boundary-regularize step** should become a **free-feature
  refinement** step: nudge perimeter corners, align eave heights,
  straighten outer edges. Leave internal features alone.

## Technical artifacts

- `boundary-regularize.js`: contains R.0 (plane regularization, to be
  removed) and R.1+R.2 (corner optimizer, to be scoped to free corners).
- Visualizer additions in `app.js`: angle arcs, ridge slope labels,
  horizontal bars, downslope arrows. Keep all.
- The boundary pipeline split (5 steps) is solid and independent of
  the regularization question.

## Performance notes

- R.0 plane regularization: ~0ms (just math on normals)
- R.1+R.2 corner optimizer: ~2s on dahouet (134 corners, 50 iterations)
  - Main bottleneck: WSD recomputation (full PIP test per evaluation)
  - Could be optimized with incremental PIP or spatial hashing
