---
id: "2026-03-25-algo-inventory"
date: 2026-03-25
title: "Boundary Algo Inventory — Strengths, Weaknesses, Upgrade Path"
tags: [boundary, algos, inventory, edge-clean]
category: review
status: concluded
refs: []
---

# Boundary Algo Inventory

After building the clean signal pipeline (boundary-analysis → eave-scan → edge-clean),
we have ~80% of the perimeter on typical planes (dahouet P2-P5). This document
reviews each remaining algorithm for upgrade opportunities.

## Utilities (not standalone algos)

- **Boundary-Analysis**: per-point CoM + open angle → edge/corner/interior labels
- **Eave-Scan**: N×G eave direction → KDE ρ voting → density-asymmetry walk → edge segments
- **Edge-Clean**: dedup vs ridges, collinear merge, corner detection, void trimming, snap

---

## Visvalingam (241 lines, score ~85.5)

**What it does:**
Rasterizes group points on a density grid at two cell sizes (0.7× and 0.9× spacing).
Each grid gets: 2-pass 3×3 Gaussian smoothing → binarize at 10% of max density →
10 ICM morphological iterations to clean up salt-and-pepper noise. Marching squares
extracts a contour (16-case lookup table), segments are chained into the longest
closed loop.

Then Visvalingam-Whyatt simplification runs at 12 target vertex counts (3 to 14).
At each level it iteratively removes the vertex with the smallest triangle area
(formed with its two neighbors). Each simplified polygon is scored:
`containment × 0.6 + areaRatio × 0.4 - vertexPenalty` where containment is the
fraction of sampled group points inside the polygon, areaRatio is
`min(polyArea, hullArea) / max(polyArea, hullArea)`, and there's a 0.012 penalty
per vertex above 10.

Best scoring candidate wins. Fallback: 32-bin radial max from centroid.

**Key parameters:** grid cellSize, density threshold (10% max), ICM iterations (10),
containment sampling (every n/200 points), vertex range 3-14, score weights.

**Strengths:**
- Works on ANY shape — concave, convex, L-shaped, irregular
- Grid contour is robust and deterministic
- Multi-scale grid catches both fine and coarse features
- Visvalingam preserves topology (no self-intersection)
- Proxy IoU scoring is interpretable

**Weaknesses:**
- Zero signal awareness — ignores eave, ridge, boundary labels
- Grid resolution is spacing-dependent — can miss thin appendages
- ICM smoothing can fill small concavities
- Centroid-angle fallback is terrible on concave shapes
- No direction regularization — edges at arbitrary angles

**Upgrade path:**
Best kept as universal fallback. Could post-process its output: snap vertices
to edge-clean corners, align edges to dominant directions. But the core
grid-contour approach doesn't benefit much from signals — it's purely geometric.

---

## Signal-SDF (458 lines)

**What it does:**
Builds a density grid (0.6×spacing cells), smooths, binarizes at 8% of max.
Builds a signal field: each signal point spreads via Gaussian kernel (σ=3×cellSize).
Computes signed distance field (SDF) via BFS from density boundary cells.

The key trick: **modifies SDF with the signal field**. Outside cells with strong
signal (>0.1 normalized) get SDF reduced by 1.5×signalNorm — this pulls the
boundary outward toward detected signals. Inside cells get SDF increased by
0.5×signalNorm — pushes boundary inward to tighten.

Marching squares at threshold -0.5 extracts the modified contour. Snap contour
vertices 40% toward nearest signal point within 3% of diagonal. Multi-epsilon
RDP simplification (6 values from 1% to 4.5% of diagonal), targeting 6 vertices.

Fallback: 64-bin radial max from centroid.

**Key parameters:** cellSize (0.6×spacing), signal σ (3×cellSize),
SDF modification (outside: -1.5, inside: +0.5), marching threshold (-0.5),
snap blend (40%), snap radius (3% diag), RDP epsilons (6 values), target verts (6).

**Strengths:**
- SDF naturally handles ANY topology — concave, convex, multi-connected
- Signal field deformation is elegant — boundary migrates toward detected edges
- Snap-to-signal adds precision where signals are reliable
- Multi-epsilon RDP finds good simplification level

**Weaknesses:**
- SDF modification weights are fixed (1.5/0.5) — too aggressive on weak signals
- Signal field is isotropic (Gaussian) — doesn't encode direction
- 6-vertex target is arbitrary
- Grid resolution limits detail on thin features
- Slow (458 lines, multiple grid passes)

**Upgrade path:**
Could feed edge-clean edges as a directional signal field instead of isotropic
Gaussian. Where an edge exists, the SDF gets pulled strongly in the perpendicular
direction. This would make Signal-SDF respect eave/perp/ridge directions while
still handling the gaps where no edges were detected. Good candidate for the
"close the last 20%" problem.

---

## MRF-Regularize (641 lines)

**What it does:**
Extracts a grid contour (same as Visvalingam). Chops it into ~40 segments, fits
a line to each via PCA. Builds a discrete angle label set:
- 8 cardinal angles (0, 22.5, 45, 67.5, 90, 112.5, 135, 157.5°)
- Ridge directions + perpendiculars (if ridge > 5% of diagonal)
- RANSAC-discovered dominant orientations (5000 iterations, cluster top 3 + perps)

Runs ICM (Iterated Conditional Modes) optimization for 30 iterations. Each segment
picks the angle label minimizing total energy:
- Data cost: angle difference from PCA-measured direction × 2.0
- Signal bonus: nearby signal support × 1.5 (reduces cost if signals agree)
- Smooth cost: penalty for disagreeing with neighbors × 3.0
  - Same angle: 0 cost
  - Right angle (90°): 0.3 penalty (encouraged!)
  - Other angle: 1.0 penalty (discouraged)

Merges consecutive segments with same label, fits weighted line to each merged
range. Intersects consecutive merged lines → polygon vertices. Optional:
snaps vertices to RANSAC line intersections within 6% of diagonal.

**Key parameters:** 8 base angles, ridge threshold (5% diag), RANSAC (5000 iters),
segment length (~contour/40), ICM weights (data=2, smooth=3, signal=1.5),
right-angle bonus (0.3 vs 1.0), snap radius (6% diag).

**Strengths:**
- Explicit regularization toward building-like angles
- Right-angle preference is architecturally correct
- Signal support weighting guides toward real edges
- RANSAC direction discovery is data-driven
- Energy minimization is principled (not greedy)

**Weaknesses:**
- 641 lines of complexity for moderate results
- RANSAC direction discovery is redundant with boundary-analysis
- 40-segment chopping is heuristic — wrong segment count loses detail
- ICM can get stuck in local minima
- Smooth weight (3.0) can overpower data (2.0) on noisy contours

**Upgrade path:**
Replace RANSAC direction discovery with boundary-analysis outward KDE peaks.
Replace signal support computation with boundary-analysis edge labels. The MRF
regularization itself is valuable — it's the only algo that explicitly enforces
architectural angle constraints. Could run on edge-clean's edge list instead of
grid contour: each edge already has a direction, MRF regularizes toward discrete
set, then intersects for vertices.

---

## Brute-Shrink (778 lines, 5-phase lifecycle)

**What it does:**

**Phase A — Wild Search:** Starts with a triangle. Optimizes each vertex via
coarse grid search (diag/18 step) + BFS refinement (0.3m→0.02m). Up to 50 passes.
Score = coverage×50 + tightness×20 + area×15 + perimeter×15.

**Phase B — Grow:** Incrementally adds vertices by splitting the longest edge.
Optimizes after each addition (50 passes). Stops at 16 vertices or when score drops.

**Phase C — Concavity Hunt:** For each edge, builds 1D density profile in a
perpendicular band (2×spacing wide). Samples along edge, counts group points at
each sample. Finds gaps where density < 30% of median for ≥3 consecutive samples.
Inserts 2 vertices at gap boundaries (the L-shape / T-shape detector). Quick
optimize (5 passes).

**Phase D — Aggressive Simplify:** Greedily removes vertices where coverage loss
< 3%. Re-optimizes after each removal. Accepts if score stays within 0.5 of best.

**Phase E — Snap to Boundary:** For each vertex, finds the outermost group point
within 5×spacing (closest to polygon edge, must be inside). Snaps vertex there.
Fine BFS refine (0.1m→0.005m).

4-second time budget across all phases.

**Key parameters:** TIME_BUDGET (4s), grid step (diag/18), BFS range (0.3→0.02m),
grow limit (16 verts), concavity band (2×spacing), gap threshold (30% median),
simplify tolerance (3% coverage), snap radius (5×spacing).

**Strengths:**
- Phase C concavity hunt is unique — detects L/T shapes from density gaps
- Phase E snap-to-boundary is universally useful (any polygon in, better polygon out)
- Time-budgeted — predictable runtime
- Score function is interpretable
- Works on any input (triangle start needs no prior knowledge)

**Weaknesses:**
- 778 lines of complexity, zero signal awareness
- Triangle start + grow is slow to converge (phases A+B eat most of time budget)
- Concavity hunt is fragile (density profiling fails on sparse data)
- Phase D can over-simplify what Phase C added
- No direction regularization at all

**Upgrade path:**
Skip phases A+B entirely: initialize with edge-clean polygon skeleton (edges +
corners already give a near-complete polygon). Run phase C on the gaps only.
Phase E snap-to-boundary could use boundary-analysis labeled points (snap to
edge/corner points, not arbitrary group points). The concavity hunt idea could
be extracted as a standalone utility for the closer algo.
