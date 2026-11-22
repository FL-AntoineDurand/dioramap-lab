---
id: "2026-05-22-bp3-grid-fill-topology"
date: 2026-05-22
title: "bp3: grid-fill topology resolves bp2's classification explosion, restores bp1 with stronger permutation"
tags: [boundary-polygons, contour, topology, algorithm-design]
category: design
status: concluded
refs:
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-05-21-bp2-autonomous-run-report"
pipeline: v7
---

## bp2 was correct in spirit, wrong in cost

Classification ("which components are outer perimeter, which are inner
holes") has no published single-component solution — KSR, PolyFit,
Geoflow all do *joint* discrete labelling. bp2's answer was: don't
classify per component, enumerate the 2^K subsets, let the cost
function (`0.8·WSD + 0.1·compactness + 0.1·spike_penalty`,
self-intersect → +∞) vote. The right subset wins by direct cost
comparison. This was sound — same shape as KSR's face-labelling MIP,
just exhaustive because K stays small in theory.

Cost in practice stacked. WSD × compactness × spike inside the cycle
cost, on top of multi-start TSP, on top of subset enumeration. On
dahouet a plane with K=20 → 2^12 = 4096 subsets after clipping K to
8 → **22 min per plane, ~80× slower than bp1**. Untenable for a step
that has to run on every plane of every building.

## bp1 never had bp2's problem

bp1 didn't classify. It walked the balance contour, projected the
corner pool onto it (0.75 m distance threshold), walked corners in
arc-length order along the contour. Its real flaws were narrower:

- **Poor contour determinism** — balance walk's cell-index `floor()`
  flips on sub-mm FP noise; closest-segment tie-break in the
  arc-length projection flips on 3rd-decimal `d²` differences.
- **FP-LSB shifts corner order** — contour simplification (DP + snap)
  put corners on a parameterisation where a one-LSB perturbation
  reordered two adjacent ones.
- **Over-aggressive D.7 deletion** — adjacent-swap permutation
  followed by vertex deletion was asymmetric (deletion irreversible)
  and topology-contaminating (a vertex looks "deletable" only because
  the current ordering happens to make it look collinear; v2 and v3
  deleted different vertices).
- **Open walk → silent centroid-angle fallback** — when the balance
  walk failed (`n_aps < 3`, open contour), bp1 dropped to a polar sort
  around the centroid. 11/27 dahouet planes hit this path in practice.

None of these required bp2's architectural pivot. bp1's architecture
(spatial filter via a contour walk + corner ordering by arc-length
projection) was fine. The *contour input* and the *permutation engine*
were the weak parts.

## Grid-fill contour gives topology by construction

Marching-squares on a density-weighted binary mask emits
deterministically closed contours — outer perimeter + one closed
contour per hole. **Topology is no longer "to be inferred"; it's a
property of the output.** Classification disappears as a concern: the
grid-fill step *physically separates* the outer ring from each hole.

We tried grid-filling a few weeks ago. The retirement reason was
unusually specific about *where* it failed — which is what makes the
revival defensible. From the March 2026 tournament write-up
([`boundary-bench/RESEARCH.md` §2.5](../../../../boundary-bench/RESEARCH.md)):

> **Conclusion**: Grid gives topology (which cells are
> occupied/empty) but direction detection needs point-level precision,
> not grid-cell precision.

**That conclusion doesn't condemn grid-for-topology — only
grid-for-direction.** The earlier work tried to use the grid for
*everything* — topology AND edge direction refinement AND corner
placement. Edge-direction failed at grid resolution (corners
stair-stepped, edges snapped to cell-aligned angles). Post-processing
on point-level signals beat any grid-level direction work. So the
grid retirement was correctly read as "grid lost on direction" — but
the topology use case got buried with it, because back then nobody
needed grid-for-topology-only.

bp3 is exactly the split the prior verdict identified but didn't
take:

- **Topology** (which corners belong to which contour, who's outer,
  who's inner): from grid contour. Pixel-aligned, deterministic,
  closed-by-construction.
- **Ordering** (cyclic sequence around each contour): from the
  existing corner pool, projected onto the contour by distance and
  walked in arc-length, then refined by permutation. Point-level
  precision via the corner positions themselves — *not* via the
  contour vertices.
- **Direction / final edge geometry**: regularize step's job, on
  point-level signals (already established in 2026-04-27).

Two independent lines of reasoning converge here: March's "grid
topology / point-level direction" finding, and the May 21 rethink's
"walk conflated two unrelated jobs — topology discovery and cyclic
ordering". Same insight from different angles. bp3 honors both.

## bp3 = bp1 structure + bp2 escape from local maxima + neither's defaults

- **Topology**: each plane gets one closed outer contour + one closed
  contour per hole from grid-fill. No walk, no inference.
- **Corner→contour**: project the corner pool onto the contour by
  distance threshold (same 0.75 m as bp1). No simplification — raw
  marching-squares cell-edge midpoints; FP perturbations don't shift
  corners between adjacent positions because positions on the contour
  are *cells apart*, not LSBs apart.
- **Ordering**: walk projected corners in arc-length order along the
  contour. Same as bp1, with a much better contour to walk on.
- **Permutation engine**: strong enough to escape local maxima (bp1's
  adjacent-swap couldn't); reuses `best_cycle`'s multi-start machinery
  from bp2 *without* the subset-enum outer loop — topology is given,
  no enumeration needed.
- **No D.7 deletion**: vertex existence is a regularize decision,
  established in 2026-04-27.
- **No silent fallbacks**: a plane that produces no contour aborts
  with a diagnostic rather than slipping to centroid-angle.

The bp1 flaws fixed by bp3, side-by-side:

| bp1 flaw | bp3 fix |
|---|---|
| balance walk fragile, sometimes open | grid contour closed by construction |
| centroid-angle silent fallback (11/27 dahouet) | abort if no contour, diagnose loudly |
| FP-LSB shifts corner order via contour simplification | no simplification; corners projected by distance, not arc-length param |
| D.7 over-aggressive deletion | deletion stays in regularize; bp3 keeps all corners |
| adjacent-swap can't escape local minima | multi-start TSP-style permutation from bp2 |

## What landed today

- `computeGridContourAdaptive` (already in `lib/pipeline/steps/boundary-edges/signals.js`)
  reused as the topology engine. Added `shrink: false` option for
  rough-topology mode — no inward 0.5·cellSize offset.
- v2 `boundary-signals` step (v4→v5): per-plane contour computed in
  the existing per-plane loop, attached to `planeGrids[g].contour`
  with grids (density / smoothed / binary), outer loop, holes.
- v3 `v7-grid-contour.{h,c}`: faithful C port. Adaptive picker has
  Phase 2 (region-count flood-fill) + Phase 3 (length-stability ≤5%
  drop), matching v2. Emits `RG_SECT_PLANE_CONTOURS = 0x11` per
  `ROOF-GRAPH-FORMAT.md §4.17`.
- v3 iso bench (`v7-boundary-signals-only-bench`) now consumes v2's
  `tile-signals_0.point-weights.bin` + `tile-signals_0.lidar-density.json`,
  enabling like-for-like contour comparison.
- Viz in both: per-plane "Plane contours" layer group (density /
  smoothed / binary heatmaps + outer LineLoop + per-hole LineLoops).

**Validation**: iso bench on `/tmp/v2-bldgonly-refs` produces
**27/27 byte-equivalent contour records** vs v2 reference (cellSize,
nCols, nRows, nOuter, nHoles all matching). The topology source is
implementation-correct. The 16-of-26 mismatches in the pipeline-level
comparison are 100% upstream divergence (v3 GRM owner + ts-weights
differ from v2 at FP-precision level), not contour-algorithm bugs —
proven by feeding v2's exact upstream into v3 and getting byte-eq.

## What stays open for bp3 implementation

- **Permutation move-set**: reuse bp2's multi-start full-enum + 2-opt
  pair, or upgrade to or-opt / Lin-Kernighan? The bp2 footnote about
  Held-Karp not applying (cost isn't decomposable per edge) still
  stands.
- **K > 1 emit**: do we emit one polygon-with-holes per plane, or
  separate `polygonPaths` + `polygonHoles` artifacts? Deferred until
  the K=1 path is green.
- **`best_cycle` cost weights**: 0.8/0.1/0.1 inherited from bp2; may
  need retuning once corner ordering comes from arc-length instead of
  multi-start seeds.
- **Stability oracle**: per the 2026-05-21 rethink, success criterion
  for bp3 is stability under σ=1mm perturbation (≥99/100 runs same
  output), not v2 byte-parity. The synthetic test fixtures from bp2
  (`v3/tests/v7-boundary-polygons-2/`) carry over directly.

## Refs

- v3 commits today: `94fd8e2`, `1e06791`, `49d8aa7`, `9ce1508`,
  `9c2973b`, `c11ec70`, `824d26c`
- [`v3/docs/plan/ROOF-GRAPH-FORMAT.md`](../../../../../v3/docs/plan/ROOF-GRAPH-FORMAT.md)
  §4.17 — `PLANE_CONTOURS` wire format
- [`v3/scripts/audit-artifact-wiring.py`](../../../../../v3/scripts/audit-artifact-wiring.py)
  — companion tier-1 hardening, surfaces write-only artifacts that
  miswiring like today's exploits
- v2 utility: [`lib/pipeline/steps/boundary-edges/signals.js`](../../../../lib/pipeline/steps/boundary-edges/signals.js)
  — `computeGridContour` + `computeGridContourAdaptive` (introduced
  Mar 25 2026 as commit `6d3fb8d`, the canonical grid extraction
  promoted out of the tournament — reused, not rewritten)
- [`boundary-bench/RESEARCH.md`](../../../../boundary-bench/RESEARCH.md)
  §2.5, §2.8 — the March 2026 verdict that retired grid-for-everything
  but explicitly left topology-use on the table ("grid wins on
  concave", "topology / direction precision" split)
- [`boundary-bench/algos-disabled/`](../../../../boundary-bench/algos-disabled/)
  — 11 archived grid-based algos (grid-distance-field,
  grid-morphological, grid-dual-contour, …) preserving the lineage;
  bp3 doesn't resurrect any of them — it reuses the
  `computeGridContour` primitive they all built on top of
- [`2026-04-27-regularization-exploration`](../../2026/04/2026-04-27-regularization-exploration.md)
  — established that vertex existence / deletion belongs to
  regularize, not boundary-polygons; bp3 inherits that rule
