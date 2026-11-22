---
id: "2026-03-31-boundary-next-gen-plan"
date: 2026-03-31
title: "Boundary Next-Gen: Beam Reconstruction, Nearby Points, Void Classification"
tags: [boundary, geometry, lidar-physics, beam-angle, nearby-points, plan]
category: design
status: proposed
refs: ["2026-03-29-plane-geometry-approaches", "2026-03-19-boundary-edges-development", "2026-03-05-psid-metadata-deep-analysis", "2026-03-11-detect-boundary-bits-5-6-7", "2026-03-03-wall-roof-detect-evolution", "2026-03-11-footprint-snap-strategy-redesign"]
---

# Boundary Next-Gen: Beam Reconstruction, Nearby Points, Void Classification

## Motivation

Phase 4 (polygon construction from edges) remains unsolved after 11 approaches
(see `2026-03-29-plane-geometry-approaches`). The fundamental problem:

- **Edge-first** produces 5-7 vertex polygons when edges close, but fails on sparse planes
- **Grid-first** always produces something, but 30-100+ vertices instead of 4-8

Two key insights emerged that open new attack vectors:

1. **Real concavities vs sparse voids are distinguishable.** A concavity with points
   above/below (another plane, ground, wall) is real geometry. A void with nothing is
   just LiDAR sparsity — and can be filled.

2. **We only use half the available data.** Boundary detection runs per-plane on group
   points only. But adjacent planes, ground, and unclassified points define the boundary
   too — often better, because the adjacent plane has different tilt/azimuth and doesn't
   suffer the same sparsity from beam angle.

## Beam Direction Reconstruction — CONFIRMED FEASIBLE

### The problem
Nearby points projected vertically onto a plane's 2D coordinate system get displaced
inward from the true boundary by `overhang × tan(scan_angle)`. At 15° and 0.5m overhang
→ 0.13m. At 20° and 1m → 0.36m. This is 1-3× point spacing, creating a "mixing zone"
where nearby points overlap with group points.

### What we proved (2026-03-31)

**Scan angle gradient method**: fit `scan_angle = g_x·x + g_z·z + c` per PSID strip.
Gradient direction = cross-track; perpendicular = flight direction. One linear regression
per strip, negligible cost.

| Metric | Dahouet test case |
|--------|------------------|
| Strips | 2 (PSID 15, 16) |
| Scan angle range | 7.0° to 9.4° |
| Gradient magnitude | 0.02°/m |
| R² | 0.24–0.28 (sufficient for direction) |
| Flight direction | Both strips: north-south (0°/180°) |

**Displacement measurement** (from agent analysis on nearby points):

| Statistic | Value |
|-----------|-------|
| Median | 0.27m |
| P95 | 0.73m |
| Max | 1.61m |

**Visual validation**: `beam-incidence` step emits 1m edges at each point along the
reconstructed beam direction. Per-PSID coloring confirms beam directions are consistent
with observed illumination zones and shadow/obstruction patterns.

### Implementation
- Step: `lib/pipeline/steps/beam-incidence.js` (v2)
- Artifacts: `beam-incidence` (edges), `beam-psid-colors` (per-point RGB by strip)
- Data available: `scan_angles` + `point_source_ids` in `.bin` sidecars for all real test cases
- The COPC extraction already stores these fields; the JSON loader passes them through

### Reprojection algorithm
```
Per strip:  linear regression (x, z) → scan_angle
            gradient = cross_track direction
            flight_dir = perpendicular

Per point:  beam = (sin(θ)·ct_x, -cos(θ), sin(θ)·ct_z)
            t = (plane_d - dot(point, plane_normal)) / dot(beam, plane_normal)
            projected = point + t · beam
```

## Today's session changes

### Scoring unification
- Replaced 3-score system (sigScore, asymScore, contourScore) with single
  `combined = 0.5×signal + 0.5×contour`
- Asymmetry removed from scoring (unreliable on sparse planes; contour proximity
  expresses the same boundary signal via grid representation)
- Asymmetry still computed for void-side ρ refinement only

### Edge emission relaxation
- Threshold: 3 contiguous steps with `combined > 0.15` → emit edge
- Removed: length gate, endpoint trim, 3-inlier minimum
- Clean edges handles curation downstream

### Density profile removal
- `densityFindCandidates` removed entirely (1D projection loses spatial info)
- Slab scan restricted to eave + perpendicular directions only (not KDE data peaks)
- Slab no longer deduped against signal edges

### Collinear merge relaxation
- Gap threshold: `max(3×spacing, 1.0m)` (was `3×spacing`)

## Revised Exploration Plan

### Phase A: Void classification (1-2 sessions) — HIGH PRIORITY

Cheapest path to fewer vertices. Sparse voids → fill grid → simpler contour.

**A1.** For each grid contour concavity, query 3D point cloud in the void via XZ radius
search. Count points from other planes, ground, unclassified.

**A2.** Classify: `real_concavity` (≥N foreign pts/m²) vs `sparse_void` (below threshold).

**A3.** For sparse voids: fill grid cells, recompute contour. Measure vertex reduction.

**A4.** Validate on dahouet: false planes (trees/ground) should stay concave; real
rectangular planes should get simpler contours.

### Phase B: Nearby point boundary signals (2-3 sessions) — HIGH PRIORITY

The adjacent plane often sees what the sparse plane can't.

**B1.** Nearby points already gathered and projected in `boundary-edges/index.js:182-224`.
Run `computeBoundaryAnalysis()` on them separately.

**B2.** Feed both signal sets (group + nearby) to `scanDirection()`. Nearby signals get
lower weight (e.g. 0.5×).

**B3.** Test on sparse planes (walls, dormers). Do we get edges where before we had none?

**B4.** The mixing zone IS the signal — nearby points appearing in the 2D projection
means the eave is there. Flag mixing-zone points, don't discard them.

**B5.** (If B3 shows precision issues) Apply beam-direction reprojection from Phase E
to tighten nearby point placement before feeding to boundary analysis.

### Phase C: Cross-plane edge confirmation (1 session) — MEDIUM PRIORITY

One-pass post-processing, no iteration needed for V1.

**C1.** After all planes run boundary pipelines, iterate ridges in roof graph.

**C2.** If both adjacent planes found a scan edge within `2×spacing` of the ridge →
confirm both, snap to ridge geometry.

**C3.** Where only one plane found an edge → accept it as confirmed for that plane.

### Phase D: Grid fill for sparse planes (1 session) — after Phase A

**D1.** After void classification, for planes with mostly sparse voids: fill grid more
aggressively (lower σ, or binary fill from convex hull of group points).

**D2.** Recompute contour. Should produce simpler, more rectangular shapes.

**D3.** Filled contour becomes topology source for contour-guided line-snap.

### Phase E: Beam reprojection for boundary precision (1 session) — CONFIRMED READY

Not exploratory anymore — the reconstruction is validated.

**E1.** In `boundary-edges/index.js` nearby-point gathering, replace vertical projection
with beam-direction projection using per-strip flight direction.

**E2.** Measure: do beam-reprojected nearby points cluster tighter along confirmed edges
than vertically-projected ones? (std dev of distance-to-nearest-edge)

**E3.** If yes → use beam projection as default for nearby points.

### Phase F: Multi-plane iteration (2+ sessions) — after B+C prove value

Belief propagation on the roof graph.

**F1.** Wrap per-plane boundary pipeline in outer loop.

**F2.** After each iteration, collect confirmed edges + merged corners across planes.

**F3.** Inject as guide constraints (like ridges) into next iteration.

**F4.** Convergence: stop when no new edges confirmed and no corners move >5cm.

### Recommended order

```
A (void classification) ──→ D (grid fill)
                                         ╲
B (nearby signals) ──→ E (beam reproj) ──→ F (multi-plane iteration)
                                         ╱
C (cross-plane confirm) ─────────────────
```

A and D are independent and attack "too many vertices".
B, C, E attack "sparse plane has no edges".
F is the grand unification, depends on B+C results.

**Start with A (cheapest, highest impact on vertex count), then B (biggest new signal
source), then C+E in parallel, then D, then F.**

## Related journal entries

| Entry | Relevance |
|-------|-----------|
| 2026-03-29 plane-geometry-approaches | All 11 polygon approaches documented |
| 2026-03-19 boundary-edges-development | Grid + walk + ridge; nearbyPoints first gathered here |
| 2026-03-16 adjacency-boundary | NOGO zones abandoned; grid-rasterize 68× faster |
| 2026-03-05 PSID metadata analysis | Scan angle ±20°, 2-3 strips, no azimuth directly |
| 2026-03-11 detect-boundary bits | Multi-return beam-split at edges = precise XZ boundary |
| 2026-03-03 wall-roof-detect evolution | 3D kNN failed; 2D kNN works for height discontinuity |
| 2026-03-11 footprint-snap redesign | Detect-then-match strategy (Hough + DP alignment) |
| 2026-03-12 spherical-KDE | Scan-line anisotropy; 3D spherical KDE removes bias |
