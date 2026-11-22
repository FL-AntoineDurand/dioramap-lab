---
id: "2026-03-29-plane-geometry-approaches"
date: 2026-03-29
title: "Plane Geometry Extraction — All Approaches Tried"
tags: [boundary, geometry, polygon, simplification, fusion]
category: review
status: concluded
refs: ["2026-03-25-algo-inventory", "2026-03-31-boundary-next-gen-plan"]
---

# Plane Geometry Extraction — All Approaches

Finding the closed polygon boundary of a roof plane from LiDAR points.
Each approach listed with pseudo code, what worked, what failed.

## Sources available

```
Grid contour     — always complete topology, inflated by ~1.5 cells, jagged staircase
Clean edges      — direction-locked, signal-confirmed, precise but partial coverage
Ridge edges      — exact plane-plane intersections, high confidence
Boundary analysis — per-point edge/corner labels, noisy on sparse planes
Scored scan steps — per-10cm signal + asymmetry + contour proximity (0-1 continuous)
```

---

## Approach 1: Pure Visvalingam

```
contour = gridContour(points, spacing×0.8)        // 198 jagged points
for targetV = 3 to 14:
    simplified = visvalingam(contour, targetV)      // remove smallest-triangle-area vertex
    score = containment×0.6 + areaRatio×0.4 - vertexPenalty
    keep best scoring candidate
return bestPoly
```

**Result:** No signal awareness. Simplifies by triangle area only — a real corner with
small area gets removed before a zigzag vertex with large area. Cuts concavities.

**Verdict:** Abandoned. Score doesn't prevent shape destruction.

---

## Approach 2: Visvalingam + corner pinning

```
contour = gridContour(points)                      // 198 points
corners = boundaryAnalysis → convex/concave labels  // ~32 corners
pinned = snapCornersToContour(corners, contour)    // 32 pinned vertices
for targetV = 3 to 40:
    simplified = visvalingam(contour, targetV, pinned)  // pinned cost = ∞
    score = coverage×50 + tightness×20 + area×15 + perim×15
    keep best
zigzagCollapse(bestPoly, spacing)
```

**Result:** Visvalingam is a no-op — can't simplify below 32 pins. Output is just the
pinned corners connected by straight edges. All contour shape between pins lost.
A section with no corners (concavity on sparse area) gets bridged by one long edge.

**Verdict:** Visvalingam + pinning are incompatible. Pinning dominates, Visvalingam is dead code.

---

## Approach 3: Zigzag collapse (fixed threshold)

```
ring = 32 pinned vertices (from approach 2)
repeat until stable:
    for windowSize = 6 down to 2:
        for each position on ring:
            if all interior vertices deviate < spacing from first→last chord:
                remove them
```

**Result:** Cascading — removes small zigzag in pass 1, creates longer spans,
pass 2 removes vertices along the longer span, eventually eats deep concavities.
P5: 32→13v in one pass, 7m-deep concavity collapsed because each 4m local window
had only 18cm deviation.

**Verdict:** Fixed threshold can't distinguish "zigzag noise" from "gentle curve."

---

## Approach 4: Zigzag collapse (proportional threshold)

```
thresh = min(span × 0.15, spacing × 3)
// 4m span → 0.60m threshold
// 0.5m span → 0.075m threshold
```

**Result:** For long spans, proportional makes threshold BIGGER (more permissive).
The concavity vertices at 18cm deviation on 4m spans → threshold 60cm → still collapsed.
Wrong direction — long spans need TIGHTER threshold, not looser.

**Verdict:** Proportional helps short spans (prevents removing tight corners) but
doesn't help long gentle curves.

---

## Approach 5: Zigzag with angle guard

Tried three variants:
```
// A: check turn at each interior vertex (>30° = keep)
//    Problem: zigzag vertices have 15-25° turns individually. Too conservative.

// B: check total turn across window (entering→leaving)
//    Problem: measures polygon's overall curvature, not zigzag. Always >30°.

// C: check turn at endpoints if we collapse
//    Problem: every collapse changes endpoint turns. Always >30° on curved sections.
```

**Verdict:** Angle checks either block everything or catch nothing. Abandoned.

---

## Approach 6: Raw contour + pinned zigzag (skip Visvalingam)

```
contour = gridContour(points)                     // 198 raw points
corners = boundaryAnalysis → corners
ridgeEnds = ridgeEdges → endpoints
snap contour vertices TO corners/ridges (not the other way)
pin snapped vertices (cost = ∞ in zigzag)
zigzagCollapse(contour, spacing, pinned)          // 198 → 59 → 40 → 39
```

**Result:** Concavities preserved (pins anchor the curve). Staircase removed.
But still 39-50 vertices — buildings should have 5-8.

**Verdict:** Correct shapes but insufficient simplification. Fixed threshold too conservative.

---

## Approach 7: Contour snap to clean edges

```
contour = gridContour(points)                     // 198 points
for each contour vertex:
    if corner within 0.5m → snap to corner
    elif edge within 0.5m → project onto edge
    elif ridge within 0.5m → project onto ridge
    else → keep contour position
dedup consecutive vertices within spacing/2
iterative collinear merge (12° threshold)
```

**Result:** Vertices land on precise edge positions. Topology from contour,
precision from edges. 15-50 vertices depending on plane.

**Verdict:** Good idea but still too many vertices. Need further simplification.

---

## Approach 8: Contour snap + pinned RDP

```
// After approach 7's snap+dedup+merge:
pinnedIndices = vertices that were snapped (to edges/corners)
for each arc between consecutive pinned vertices:
    rdpSimplify(arc, epsilon=spacing)   // pins are arc endpoints, always kept
reassemble arcs into ring
```

**Result:** Better simplification within arcs. P2: 34→10v, P3: 29→20v.
Pinned vertices survive, staircase between them gets simplified.

**Verdict:** Good combination but pinned RDP alone doesn't reduce enough.
Many "pinned" vertices are on scan edges that don't need to be pinned.

---

## Approach 9: Edge-first closure

```
vertices = dedup(all edge endpoints, within 1.5×spacing)
adjacency = edges connecting vertex pairs
openEnds = vertices with degree 1
while openEnds >= 2:
    connect nearest open pair with gap-fill edge
cycle = findLongestCycle(adjacency)
polygon = cycle vertices in order
```

**Result:** Very few vertices (5-7). Precise because built from edge geometry.
But fails when edges don't form a closeable cycle — falls back to raw contour.
Some planes get great polygons, others get 84-144v fallback.

**Verdict:** Best results when edges close. Worst when they don't. Fragile.

---

## Approach 10: Contour-guided line-snap (current)

```
contour = gridContour(points)                     // topology source
allEdges = cleanEdges + ridges                    // precision source
scoredSteps = all non-zero-score scan walk points  // secondary precision

for each contour vertex:
    if ridge/edge within 3×spacing → project onto it
    elif scored step within 3×spacing → snap to it
    elif on same line as previous vertex → extend along line
    else → keep contour vertex
dedup + collinear merge
```

**Result:** Follows contour path (correct topology, concavities preserved) with
vertices positioned on precise line geometry. 15-35 vertices.

**Verdict:** Best fusion of topology + precision. But needs further simplification
to reach 5-8 vertex building polygons. The simplification problem remains unsolved.

---

## Approach 11: Pareto vertex optimization (refine-polygon)

```
polygon = contourSnap output
for each pass:
    for each vertex:
        candidates = [snap to edge, snap to corner, slide, push perpendicular]
        for each candidate:
            evaluate 4 criteria: signalProximity, dirAlignment, collinearity, cornerAttraction
            hard constraint: no group points cut off
            accept if: no criterion degrades >5%, at least one improves >5%
    collinearMerge(12°)
repeat until no moves
```

**Result:** Reduces vertex count by merging collinear runs. Vertices move to better
positions on edges/corners. No arbitrary weight blending.

**Verdict:** Promising approach. Needs tuning — candidate generation could be
smarter (use contour+score data), and the accept threshold affects aggressiveness.

---

## The Phase 4 dilemma: edge-first vs grid-first

Two sources, complementary weaknesses:

**Grid contour** — always exists, even on 20-point wall planes. Gives correct topology
(concavities, holes, overall shape). But inflated ~1.5 cells by Gaussian blur, jagged
staircase vertices, 50-200 points where the true polygon has 4-8.

**Clean edges** — precise direction-locked lines with sub-cell accuracy. But partial
coverage: a plane needs enough boundary-analysis signals AND contour proximity to emit
edges. Sparse planes (walls, small dormers, 30-80 points) often get 0-2 edges. Can't
form a closed polygon from 2 edges.

The fundamental tension: **edge-first approaches produce the best polygons when edges
close, and nothing when they don't. Grid-first approaches always produce something,
but struggle to simplify from 100+ vertices to 4-8 without destroying shape.**

### Grid-first attempts (start from contour, try to simplify/improve)

| # | Approach | Result |
|---|----------|--------|
| 1 | Pure Visvalingam | No signal awareness. Removes real corners before zigzag noise. |
| 2 | Visvalingam + corner pinning | Pins dominate (32 locked), Visvalingam is dead code. |
| 3 | Zigzag collapse (fixed threshold) | Cascading — eats deep concavities. |
| 4 | Zigzag collapse (proportional) | Long spans get looser threshold — wrong direction. |
| 5 | Zigzag + angle guard (3 variants) | Either blocks everything or catches nothing. |
| 6 | Raw contour + pinned zigzag | Correct shapes but 39-50 vertices. |
| 7 | Contour snap to clean edges | Precise positions. 15-50 vertices. Still too many. |
| 8 | Contour snap + pinned RDP | 10-20v. Good but too many pins from scan edges. |
| 10 | Contour-guided line-snap (current) | Best fusion of topology + precision. 15-35v. |
| 11 | Pareto vertex optimization | Post-process slide/snap/merge. Promising but needs work. |

### Edge-first attempts (start from edges, try to close)

| # | Approach | Result |
|---|----------|--------|
| 9 | Edge-first closure | 5-7v when edges close. Falls back to raw contour when they don't. |

### The score

- **Grid-first**: 9 attempts, always produces output, never precise enough or simple enough
- **Edge-first**: 1 attempt, sometimes perfect (5-7v), sometimes useless (0 edges → fallback)

### The unsolved problem

No approach bridges the gap. The ideal algorithm would:
1. Use edges where they exist (precise, direction-locked, few vertices)
2. Use contour where edges don't exist (topology, shape, always available)
3. Produce 4-8 vertices for simple rectangular planes
4. Preserve concavities on complex planes (L-shapes, T-shapes)

Approach 10 (current) is closest — it does 1+2 but fails on 3 (too many vertices).
Approach 9 nails 3 but fails on 2 (no fallback strategy beyond raw contour).
Nobody has solved 3+4 together.

---

## Key insights

1. **Topology and precision come from different sources.** Grid contour has topology
   (concavities, holes). Clean edges have precision (direction-locked positions).
   No single source has both.

2. **Simplification and topology preservation are contradictory.** Every simplification
   method that's aggressive enough for 5-8 vertices also destroys concavities.
   Every method that preserves concavities produces 30+ vertices.

3. **Per-step combined score drives edge emission.** A single continuous score
   (50% signal proximity + 50% contour proximity) is computed at each 10cm walk
   step. Contiguous runs of ≥3 steps above threshold emit edges. No binary gate,
   no separate length/trim/inlier filters — cleanEdges handles curation downstream.

4. **The grid contour inflates and blurs.** Gaussian smoothing + marching squares
   placement + ICM cleanup spread the contour ~1.5 cells beyond the true boundary.
   The 0.5-cell shrink fix helps but the remaining ~1 cell inflation from the
   Gaussian is inherent and variable (depends on local point density).

5. **Two edge detection methods complement each other:**
   - Signal scan (greedy): boundary-analysis signals vote via Hough ρ KDE, directions
     processed by weight, consumed signals removed after each direction
   - Slab scan: slides from outside inward along eave and perpendicular directions only
     (the two main roof axes), no signal consumption
   Density profile was removed (1D projection loses spatial info, unreliable on sparse planes).
   Both feed into the same evaluation pipeline (scanDirection walk with combined scoring).

6. **Density asymmetry was replaced by contour proximity in scoring.**
   Asymmetry (point count imbalance across the line) is unreliable on sparse planes.
   Contour proximity expresses the same thing (where points end) but via the grid
   representation which handles sparsity better. Asymmetry is still computed for
   void-side detection (ρ refinement) but no longer affects edge emission.
