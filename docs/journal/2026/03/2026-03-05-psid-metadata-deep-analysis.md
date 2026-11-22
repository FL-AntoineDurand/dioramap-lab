---
id: "2026-03-05-psid-metadata-deep-analysis"
date: 2026-03-05
title: "PointSourceId Metadata Deep Analysis — Ideas, Pipeline Wiring, and Reality Check"
tags: [per-point-metadata, psid, pipeline, wall-roof, ridge, sensor-params, ign, lidar-hd, finding]
category: finding
status: concluded
refs:
  - "2026-03-04-per-point-metadata-pipeline"
  - "2026-03-04-flight-strip-simulation"
  - "2026-03-02-ign-lidar-hd-source-characterization"
  - "2026-03-03-wall-roof-detect-evolution"
pipeline: v6g-kde-full
design_doc: "docs/reference/IGN-LIDAR-HD-SPECIFICATIONS.md"
---

## Context

Following the per-point metadata pipeline work (2026-03-04), the initial analysis document
(`docs/work/2026-03-05-metadata-exploitation-analysis.md`, Section 3) proposed three
PointSourceId-based capabilities rated as "HIGH priority, strongest signal":

- **3a**: Per-strip density analysis to flag single-strip planes as walls
- **3b**: Strip shadow ridge detection (bit 5 in ridge-candidates)
- **3c**: Wall point signature (high scan angle + multi-return + single-strip = wall)

This journal entry documents the full deep-dive: pipeline wiring analysis, confrontation
with IGN LiDAR HD physical reality, and final conclusions on what is and isn't worth pursuing.

---

## Part 1: Do These Ideas Need Flight Direction?

### Initial concern

The PSID ideas in the analysis doc use illumination/shadow metaphors that imply knowledge
of the flight strip direction. The LiDAR beam is orthogonal to the flight direction, but
we don't know the flight heading from the per-point data alone (for a single building).

### Resolution: no, they don't

All three proposed implementations work on **observable statistical correlations**, not
on predicted illumination geometry:

| Idea | What it actually computes | Needs flight direction? |
|------|--------------------------|------------------------|
| 3a — Per-strip density | Count points per PSID per plane | No — pure counting |
| 3b — Shadow ridge | Co-occurrence of strip transition + plane transition at boundaries | No — detects consequence, not cause |
| 3c — Wall signature | Combines per-point observables (scan angle, return number, PSID count) | No — all local features |

The illumination/shadow narrative explains **why** the correlations exist in the data, but
the algorithms are direction-agnostic. They detect effects without modeling the cause.

The one place flight direction **would** matter is if we wanted to **predict** which strip
should illuminate which slope (e.g., "strip A flies NE so it should illuminate SW-facing
slopes"). That's a stronger model but requires inferring heading from scan angle sign
patterns or point spatial distributions across the broader tile — data we don't have at the
single-building level.

---

## Part 2: Pipeline Wiring Analysis (v6g-kde-full)

### Current v6g-kde-full step sequence

```
 1. extract-denoise         → building-points, building-{scan-angles,point-source-ids,return-numbers,number-of-returns}
 2. compute-normals         → point-normals, _kdTree
 3. cascade-segment-kde     → cascade-segments, cascade-segment-list, ridge-candidates
 4. cascade-ridge-detect    → ridge-candidates (bitmask bits 0-2)
 5. normal-cascade-seed     → seed-planes, plane-equations
 6. competitive-grow#0      → owner, ridge-candidates (bit 3)
 7. ransac-seed             → seed-planes, plane-equations (extended)
 8. competitive-grow#1      → owner, ridge-candidates
 9. point-stealing          → owner, ridge-candidates (bit 4)
10. refit-planes            → plane-equations (refitted)
11. ridge-edge-fit          → edges-ridge-fit
12. hull-extraction         → planes (hulls)
13. wall-roof-detect        → wall-roof-candidates, edges-wall-roof
14. ridge-clip → combine-edges → snap-edges → compute-wall-edges → close-corner-gaps
15. clip-to-footprint → obstacle-filter → resolve-overlaps → shape-fitting
16. build-obstacle-records → lod2-simplify
17. compute-ortho-artifacts → fit-solar-panels
```

### Where each idea would wire in (pre-reality-check)

**3a — strip-density-analysis (new step)**: After `refit-planes#0`, before `ridge-edge-fit#0`.
Reads: `building-point-source-ids` (from ED), `owner` (from PS), `plane-equations` (from RP), `building-count`.
Writes: `strip-density-per-plane`, `dominant-strip`, `strip-consistency`.
Consumers: `wall-roof-detect` (for wall flagging), optionally `hull-extraction`.

**3b — enhance cascade-ridge-detect**: Same position (step 4), adds bit 5 to bitmask.
New read: `building-point-source-ids` from ED.
Logic: For each point near a segment boundary, check if neighbors on different segments
also have different dominant PSIDs → bit 5.
Wiring change: Add `'building-point-source-ids': ED` to CRD in `v6StandardWiring()`.

**3c — enhance wall-roof-detect**: Same position (step 13), adds bit 5 to wall-roof-candidates.
New reads: `building-point-source-ids`, `building-scan-angles` from ED, `strip-consistency` from 3a.
Logic: high scan angle + multi-return + single-strip coverage = wall candidate.

No circular dependencies. PSID data flows cleanly: extract-denoise → cascade-ridge-detect
(for 3b) and extract-denoise → strip-density-analysis → wall-roof-detect (for 3a/3c).

---

## Part 3: Reality Check Against IGN LiDAR HD Specifications

### Hard facts from docs/reference/IGN-LIDAR-HD-SPECIFICATIONS.md

| Fact | Implication |
|------|------------|
| Max scan angle ±20° from nadir → beam is 70° from horizontal | Walls are invisible (0.02-5.6% wall/roof ratio) |
| 95-98% single return on rooftops | Multi-return is edge-only, rare |
| 2-3 strips per building, angles like -12°, +1°, +14° | Both slopes of a gabled roof are well-illuminated by ALL strips |
| Dual-channel sensors produce paired PSIDs (1042/2042) | "Strip count" can mislead without channel grouping |

### Empirical wall return rates (from the specs)

| Building | Roof pts | Wall pts | Wall/Roof ratio |
|----------|----------|----------|-----------------|
| Paris Haussmann | 127,111 | 29 | **0.02%** |
| Amiens | 2,445 | 1 | **0.04%** |
| Bordeaux depot | 1,719 | 30 | **1.7%** |
| Nantes pavilion | 3,089 | 172 | **5.6%** |

Walls are essentially invisible to airborne LiDAR at ±20° from nadir.

### Idea-by-idea reassessment

#### 3a — Per-Strip Density → Wall Flag: INVALID

The document says "flag single-strip planes as potential walls." But **we don't have wall planes**.
The pipeline only produces roof planes. There are virtually no wall points (<0.1% for
well-matched buildings). A roof plane seen by only 1 strip is just a plane at the edge of
strip coverage, not a wall.

**What survives**: Per-strip density as a **confidence metric** for roof planes. But this
isn't actionable — the pipeline doesn't have a conflict resolution point that needs plane
confidence. Competitive-grow + point-stealing already handles ambiguity via geometric
proximity. **Not worth a new step.**

#### 3b — Strip Shadow Ridge Detection: MARGINAL / REDUNDANT

The document imagines hard "shadow boundaries" where one strip can't see the opposite slope.
But with ±12-14° from vertical, on a typical gabled roof (30-45° pitch):

- A strip at -12° hits the near slope at ~48° incidence — excellent return
- The far slope is partially occluded by the ridge itself near the ridge line
- But further down the far slope, the beam CAN reach it

The shadow effect IS real at the ridge, but **the boundary is the ridge itself** — which
normal-space + offset-space ridge detection already catches. The PSID signal would be
measuring the same physical boundary through a different lens.

Cascade ridge detection already has 3 independent methods (normal-space, offset-space, k-NN
boundary). Adding a 4th that detects the same boundary differently gives diminishing returns.
Would need empirical evidence showing cases where the existing 3 methods miss ridges but
PSID catches them — likely rare.

**Verdict**: Theoretically sound but practically redundant with existing ridge detection.

#### 3c — Wall Point Signature: INVALID

"High scan angle + multi-return + single-strip = wall." But:
- There are <30 wall points on a 127K-point Paris building (0.02%)
- We don't detect wall planes; these points are noise-level
- Pre-filtering for something that doesn't exist is pointless

**Dead idea.**

---

## Part 4: What PSID CAN Actually Help With (Surviving Ideas)

After ruling out the wall-based ideas, what's left?

### 4a — Inter-strip offset detection → plane refinement

The specs document 2-5 cm residual XY offsets between strips after post-processing (also
identified as Generator Gap 1). Points from different strips on the same plane may disagree
slightly, broadening the cloud perpendicular to the plane.

Could help by fitting plane equations **per-strip** and reconciling. But the 2-5 cm offset
vs 3 cm noise gives SNR ~1:1. The current global fit already absorbs this in the RANSAC
inlier threshold (4.5 × σ_z ≈ 13.5 cm). **Marginal value.**

### 4b — Per-strip density → sensor-adaptive noise estimation

Different strips hit the roof at different angles. But the specs showed ±6% noise variation —
same conclusion as the ScanAngle analysis (Section 4 of the work doc). **Already ruled out.**

### 4c — Dual-channel grouping

Some buildings have 4 PSIDs but only 2 physical overpasses (dual-channel: 1042/2042 +
1010/2010). Matters only if strip count feeds something — and we concluded it doesn't
justify a step.

### 4d — Slope confirmation from strip density + angles (user idea)

**Concept**: A strip at -12° should illuminate a slope facing it more densely than one
facing away. The density ratio between strips could cross-check the fitted plane normal.

**Blocking problem**: We don't know the flight direction. `ScanAngle` tells us the
cross-track angle from nadir, but not which compass direction "cross-track" is. For a
single building, all points from one strip have nearly the same angle (±1-3° variation) —
too small to infer the cross-track direction from the building alone.

Without flight direction, we can observe density asymmetry but can't predict which way it
should go. So we can't confirm slope direction.

**What survives**: A direction-agnostic version — fit the plane equation per-strip
independently and compare. If they agree, good plane. If they disagree, unreliable fit.
But with RANSAC on 100+ inliers, the plane normal is already well-constrained. The per-strip
check would rarely flag anything the residual RMS doesn't already catch.

**Verdict**: Interesting theoretically, not actionable.

### 4e — Post-modeling wall detection from residuals (user idea)

**Concept**: After roof planes are fitted and most points assigned, look at the REMAINING
unassigned points (owner = -1). Do spatial clustering on residuals, fit planes, and check if
any cluster is:
1. Near-vertical (tilt > 80°)
2. Single-strip dominant (one PSID provides >80% of points)
3. Near the footprint edge

**Why PSID genuinely helps here**: Need to distinguish "vertical cluster = wall" from
"vertical cluster = chimney side / dormer side." The discriminator:
- **Wall**: grazing incidence → only the one strip with best angle gets returns → single-strip
- **Chimney/dormer**: on top of the roof, seen from above by all strips → multi-strip

**Obstacles**:
1. **Point survival**: `extract-denoise` clips to the footprint and filters class 6.
   Wall points at the building perimeter should be inside the polygon in plan view
   (vertically below the roof edge), but some may be classified as class 1 (unclassified)
   by IGN and get discarded. The class 67 quick-win would help recover some.
2. **Point count**: Paris has 29 wall points out of 127K. After denoising, maybe 20-25
   survive. That's enough to fit a vertical plane, but many buildings will have 0-3 wall
   points — not enough.
3. **Value when it works**: Wall-roof edge refinement (intersection of roof plane and wall
   plane improves hull boundary by a few cm), building height estimation, LoD2 quality.

**Where in pipeline**: New step `detect-residual-walls` after `refit-planes`, before
`hull-extraction`. Outputs `wall-planes` array for downstream consumption.

**Verdict**: Sound in principle, but likely fires on <20% of buildings. Worth an empirical
probe: run the 42 real test cases through the current pipeline, count unassigned class-6
points per building, check how many are near-vertical + single-strip + near footprint edge.
If >15 buildings have ≥10 such points, worth implementing. If <5, not worth it.

---

## Part 5: Final Verdict Summary

| Idea | Original assessment | After reality check | Action |
|------|-------------------|--------------------|---------|
| **3a — Strip density → wall flag** | HIGH priority | INVALID | No wall planes exist |
| **3b — Shadow ridge detection** | HIGH priority | MARGINAL | Redundant with 3 existing methods |
| **3c — Wall point signature** | HIGH priority | INVALID | <0.1% wall points |
| **4a — Inter-strip plane refit** | Not proposed | MARGINAL | SNR ~1 (2-5 cm offset vs 3 cm noise) |
| **4b — Per-strip noise estimation** | Not proposed | NOT WORTH | ±6% variation, already ruled out |
| **4d — Slope confirmation** | User idea | NOT ACTIONABLE | Requires unknown flight direction |
| **4e — Post-modeling wall detection** | User idea | MAYBE | Empirical probe needed first |

### The one implemented win

The **ReturnNumber edge detection** (Section 2 of the work doc, implemented in
`wall-roof-detect` v12 as bit 4) was the real practical win from the per-point metadata.
Multi-return on rooftops is rare (2-5%) but concentrated at physical edges — a genuine
signal that the height-gap method alone doesn't catch.

### Key lesson

The initial analysis document reasoned about a **generic** LiDAR setup (wall returns,
significant angle diversity, strong shadow effects) rather than the **specific** IGN
configuration (near-nadir ±20°, no walls, modest angle diversity, rotating polygon mirror).
Always confront ideas against the actual sensor specifications before planning implementation.

### Next step (if pursued)

**Empirical diagnostic on 42 real test cases**:
```bash
# Proposed: add a diagnostic mode to benchmark or a standalone script
# For each real test case, after pipeline runs:
#   1. Count unassigned points (owner == -1) after point-stealing
#   2. Of those, how many have tilt > 80° (fit local plane from k-NN)?
#   3. Of those, how many have >80% single-PSID dominance?
#   4. Of those, how many are within 0.5m of footprint edge?
# Report: per-building counts, decide if worth a step
```

If the probe shows <5 buildings with ≥10 qualifying wall points, close this investigation.
If >15, implement `detect-residual-walls` as a post-modeling step.

---

## References

- `docs/reference/IGN-LIDAR-HD-SPECIFICATIONS.md` — Section 5 (Wall Detection & Scan Angle Geometry)
- `docs/work/2026-03-05-metadata-exploitation-analysis.md` — Section 3 (original proposals)
- `lib/pipeline/steps/wall-roof-detect.js` — v12 multi-return implementation
- `lib/pipeline/steps/cascade-ridge-detect.js` — existing 3-method ridge detection
- `lib/pipeline/wiring.js` — `v6StandardWiring()` and `hullChain()`
