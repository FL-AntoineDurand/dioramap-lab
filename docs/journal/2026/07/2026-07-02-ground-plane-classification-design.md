---
id: "2026-07-02-ground-plane-classification-design"
date: 2026-07-02
title: "Ground-plane classification — signal survey + placement decision"
tags: [design, boundary-classify, boundary-topology, boundary-regularize, ground-plane, model-validity, stream-b, v7]
category: design
status: proposed
pipeline: v7-bp2
refs: []
---

## 0. Why this exists

Stream B (v3 model-validity harness — see `v3/tests/v7-model-validity/`)
now runs end-to-end and scores `flat-default` against analytic ground
truth. The scorecard is honest but misleadingly red because our first
metric, `roof_count_match`, reports **expected=1, actual=2** on a
synthetic flat roof.

Root cause is not the algorithm getting the roof wrong — it's the
**ground around the building** appearing as a distinct fitted plane
alongside the flat roof. Ground truth doesn't consider the ground a
"roof plane"; v7 doesn't consider it a "wall"; both roof and ground
have `tilt_deg = 0°`, so the existing wall/roof discriminator can't
tell them apart.

Broader question: how should v7 **classify** a fitted plane as ground
vs. roof, without deleting the plane (v2's behavior), so downstream
consumers (scorer, viewer, wall-detect, regularize) can decide what
to do with it?

## 1. Investigation — why doesn't the existing class filter catch this?

`v7-boundary-topology` (Phase A.3) already computes per-plane
classification stats from LAS point-class labels and candidate-rejects
any plane where the ground+veg+water fraction (`gvw_frac`) exceeds a
threshold (default 0.50). Then a **ridge-neighbour rescue** re-admits
candidates that share a ridge with any non-rejected plane. Direct port
of bench-v2 boundary-3d.js.

Traced on `flat-default` (per-plane, printed from
`v7-boundary-topology-impl.c:636+`):

| pid | tilt | cz | n_pts | cs_ground | gvw_frac | cand_rej | rescued? | final       |
|----:|-----:|-----:|------:|----------:|---------:|:--------:|:--------:|:-----------:|
| 0 (ground) | 0° | -2.33 | 10866 | 10865 | **1.000** | **1** | **1** | live (wrongly!) |
| 1 (roof)   | 0° | +2.67 |  9262 |    14 |     0.003 | 0 | 1 | live |
| 2 (wall)   | 90° | +0.27 |   261 |    10 |     0.046 | 0 | 1 | live |
| 3 (wall)   | 90° | +0.06 |   199 |     2 |     0.015 | 0 | 1 | live |

The class filter fires cleanly (`gvw_frac=1.000`) — but the rescue
overrides it because ground shares a ridge with the walls (their
bases sit at the ground plane's elevation). Rescue was designed for a
**different** failure mode: real-world class-2 mislabelling of
partial roof surfaces (10-40% wrongly labelled by the LiDAR
classifier). It's correct behavior on real data; it's the wrong
behavior when the class signal is unambiguous (`gvw ≥ 0.90`).

Currently `rejection_reason` is reset to 0 after rescue — the fact
that the class filter FIRED is erased from the wire. Consumers can't
tell "class filter never tripped" from "tripped and was rescued."

## 2. Parallel to perimeter-edge classification

`v7-boundary-classify` solves a structurally similar problem for
edges. For each edge on a roof plane, it iterates other candidate
roof-plane edges and tests:

- **SHARED**: 3D-collinear (`|cos θ| ≥ cos 15°`), 3D distance
  `< 0.30 m`, endpoint overlap `≥ 0.30 m` on the primary edge's
  parametric axis
- **OVERHANG_TOP/BOTTOM**: XZ-collinear (same 15° threshold), XZ
  distance `< 0.30 m`, overlap threshold; sign of `Δy` distinguishes
  top from bottom
- **PERIMETER** (default): none of the above match within thresholds

Match candidates are pre-filtered to the **outward XZ side** of the
edge, computed from the plane's polygon centroid relative to the
edge midpoint (`compute_outward_xz`, line 137).

Aggregation: union of coverage in edge-parameter space, threshold on
`covered_fraction > 0.20`; strongest match type wins the tag.

**The key insight — this pattern also solves the ground problem.**
An edge is PERIMETER when it has no matching neighbour on its
outward side. If there IS a plane below at that outward location,
that plane isn't roof structure — it's ground. Same primitives
(polygon centroid, outward XZ, `point_in_poly_xz`), just applied to
plane-vs-plane instead of edge-vs-edge.

## 3. Signal survey

Everything the pipeline already computes that could feed a
ground-vs-roof classification, ranked by usefulness for THIS problem:

### Strong

| # | Signal | Source | Wire? | Notes |
|---|---|---|---|---|
| S1 | `cs_ground / cs_total` (LAS class-2 fraction per plane) | boundary-topology (computed, discarded) | ✗, reserved via `RG_PLANE_HAS_CLASS_STATS` | Nearly determinant when it fires. `HAS_CLASS_STATS` flag exists but nothing writes the stats. |
| S2 | "Below-and-outward of a perimeter edge" | derivable in boundary-classify from PERIMETER tags + `outward_xz` + polygon vertices | Computable in-step | Positive-geometric-evidence signal. Uses signals already produced by classify. |
| S3 | Polygon peripherality (this plane's polygon vertices lie OUTSIDE another plane's polygon in XZ) | reuses `point_in_poly_xz` on POLYGON_PATHS | Computable in-step | Coarser but plane-only variant of S2, works when perimeter tags aren't available. |

### Corroborating

| # | Signal | Source | Wire? | Notes |
|---|---|---|---|---|
| S4 | Centroid z relative to median of horizontal planes | derived | ✓ centroid on wire | Ground is the lowest. Cheap sanity check. |
| S5 | "Class filter tripped" (whether `gvw_frac > threshold`) | boundary-topology | Would need a new flag bit `RG_PLANE_CLASS_FILTER_TRIGGERED = 0x0020` | Independent of rescue outcome. Downward vote if set. |

### Discarded

- **Tilt angle** alone: flat roof and ground both `tilt=0`; useless.
- **Absolute centroid z**: 5m-building ground at z=0 is
  indistinguishable from a 5m-shorter building's roof at z=0.
- **Point count / density**: ground and roof have similar densities;
  walls are grazing-angle sparse. Too noisy.
- **Building footprint from IGN**: too misaligned (5m+ possible) and
  only reflects rough shape; ruled out by user.

## 4. Proposed composition

**Two-tier: decisive positive test + corroborating vote.**

### Tier 1 — decisive

- **S2 (perimeter-outward)**: for each roof plane R with any
  PERIMETER-tagged edge, look outward-of-that-edge in XZ, at
  plane-below-R elevations. If a candidate horizontal plane G's
  polygon vertices project into the outward region of R's perimeter
  edges → G is ground.
- **S3 (polygon peripherality)**: fallback for planes where S2
  can't be evaluated (missing polygon, missing perimeter edges).

**Any Tier-1 fire → tag as ground with confidence ≥ 0.9.**

### Tier 2 — corroborating

- **S1** (class-2 ratio): `cs_ground / cs_total > 0.5` → vote.
- **S4** (relative z): plane is lowest of the horizontal planes → vote.
- **S5** (class filter tripped) → vote.

**All Tier-2 votes agree → tag with confidence 0.8.**
**Only partial Tier-2 → uncertain (0.5), leave alone.**

### Wire format

Non-destructive tagging. Two additions to `rg_plane_record_t.flags`:

- `RG_PLANE_LIKELY_GROUND = 0x0020` — set when confidence ≥ 0.8.
- `RG_PLANE_CLASS_FILTER_TRIGGERED = 0x0040` — set independently of
  the rescue outcome. Consumers can distinguish "class filter never
  fired" from "fired and rescued."

Optionally emit `cs_ground`, `cs_total`, `cs_building` as an overlay
section when `HAS_CLASS_STATS` is set — future-proofs consumers that
want to reweight.

**Geometry is NEVER deleted.** Consumers (metric, viewer, wall-detect,
regularize) decide what to do with the tag.

## 5. Placement — before or after regularize?

Original hunch was to add a Phase G to `boundary-classify` (which
runs AFTER regularize). Reconsidering with cost in mind:

**boundary-regularize is expensive** (make-shared cascade, DOF, corner
merge — the pipeline's largest single step by Frama-C wall-time). It
processes every live plane's polygon and cranks through the merge and
snap operations. Running it on planes we're about to tag as ground is
**pure waste** — the regularized geometry of a ground plane isn't
consumed by anything except (eventually) the tag itself.

**Move the ground classification BEFORE `v7-boundary-regularize`.**

Concretely, the pipeline order becomes:

```
… → v7-boundary-corners → v7-boundary-polygons-2
  → v7-classify-planes  ← NEW, tags ground planes
  → v7-boundary-regularize (skips ground-tagged planes)
  → v7-boundary-classify
  → v7-wall-detect (also skips ground-tagged planes)
  → v7-score-intrinsic-postwall
```

### Complication — S2 depends on perimeter tags, which don't exist yet

The primary strong signal (S2, below-and-outward of a PERIMETER edge)
needs perimeter-edge tags. Those are computed inside `boundary-classify`.
If we put ground-classification BEFORE regularize (and therefore before
classify), S2 isn't directly available.

Three ways out:

1. **Compute perimeter-edge tags twice** — once early on the raw
   bp2 polygons for ground classification; once again inside classify
   on the regularized polygons for the fine perimeter/shared/overhang
   distinctions. Wasteful but simple.
2. **Split classify** — extract the perimeter-edge phase into its own
   step (`v7-perimeter-edges`?) that runs before regularize. The
   overhang/shared/level-transition passes stay in boundary-classify
   downstream. Cleanest architecturally but non-trivial surgery.
3. **Ground-classify with S3 alone** — drop S2, rely on polygon
   peripherality (S3) as the primary geometric signal + Tier 2 votes.
   Simplest to build first; may miss some ground cases where S3 is
   ambiguous but S2 would have been decisive.

**Recommendation**: start with option 3 (ship the step with S3 + S1
+ S4 + S5, no S2 dependency), measure how flat-default and a couple
of harder cases behave, then decide whether option 2 is worth the
surgery. Option 1 (double-computation) is a fallback if we run out of
time.

## 6. Pros / cons of moving classification early

**Pros**:
- No wasted regularize compute on ground planes (roughly 20% of
  flat-default's regularize wall-time by point count).
- The tag is available to more downstream steps (regularize,
  classify, wall-detect, score) — better information flow.
- Cleaner semantics: "ground" is a property of the plane's geometry
  + supporting points, not a post-hoc interpretation.

**Cons**:
- Loses direct access to perimeter-edge tags (see §5 complication).
- If S3 alone isn't discriminative enough, we ship a false-positive
  ground tag and lose a real roof plane's regularization. Fallback:
  regularize could ignore the tag and process everything, which
  reverts the "wasted compute" argument.
- Adds a step; slightly more pipeline surface to keep valid.

## 7. Limits + open questions

- **Multi-story buildings**: does S3 misfire when a lower roof
  section's polygon extends outside an upper section's? Probably yes;
  S3 needs at least "z is meaningfully below" as a co-requirement.
- **Adjacent ground / grass classes**: our synthetic data uses class-2
  cleanly; real-world class-2 also covers ground-adjacent surfaces
  (grass, low veg). S1 will fire on those too. Is that desirable
  (they're not roof either) or noise (they're not "the ground" for
  metric purposes)?
- **What about planes with no polygon?** bp2 might not produce a
  polygon for very small planes. Those can't be S3-classified;
  fall back to Tier 2 votes alone.
- **How to calibrate the confidence threshold** without a big test
  set? We have one case (`flat-default`). Need at least 3-5
  synthetic cases to hand-tune weights.
- **Do we need a "confidence" scalar** on the wire, or is the
  binary flag enough? Two-bit encoding (both flags set = high
  confidence, LIKELY_GROUND alone = medium, TRIGGERED alone = low)
  could work without a scalar.

## 8. Plan

1. **First step**: add `HAS_CLASS_STATS` overlay writer to
   `boundary-topology` (~30 lines). Emit `cs_total`, `cs_ground`,
   `cs_building` alongside the flag. Independent of the ground step
   — future-proofs any consumer.
2. **Add `RG_PLANE_CLASS_FILTER_TRIGGERED` flag** in
   `boundary-topology` (~5 lines). Set when `gvw_frac > threshold`,
   regardless of rescue.
3. **Build `v7-classify-planes` step** between `v7-boundary-polygons-2`
   and `v7-boundary-regularize`. Start with S3 + Tier-2 (option 3).
   Runs on the flat-default case; sets `LIKELY_GROUND` on the ground
   plane.
4. **Update Stream B scorer** (`v7mv-score-vs-gt`) to skip planes
   with `LIKELY_GROUND` set when counting roofs. `roof_count_match`
   should read expected=1, actual=1 ✓ on flat-default.
5. **Teach `boundary-regularize` to skip `LIKELY_GROUND` planes** —
   preserves geometry, skips expensive regularization ops on them.
6. **Later**: evaluate whether S2 is worth the classify-split
   (option 2). Only if a harder case surfaces where S3 alone
   misclassifies.

## 9. No-regression strategy

- Before landing: current `plane_count_total_match` metric on
  `flat-default` reports actual=4. After: same (we're TAGGING, not
  deleting; total remains 4).
- After landing: `roof_count_match` on `flat-default` becomes
  1 = 1 ✓ (ground plane skipped by scorer).
- `boundary-regularize` still processes 3 planes (roof + 2 walls),
  not 4. Compare Frama-C wall-time before/after — should drop
  proportionally to ground's regularization cost.
- Real-data smoke on the `dahouet` case (from `v7.json`'s default
  spec): dump per-plane `LIKELY_GROUND` decisions; visually verify
  none of the actual roof planes get tagged.

## 10. Related

- `v3/tests/v7-model-validity/tools/score-vs-gt.c` — the Stream B
  metric tool that surfaced the miscount.
- `v3/steps/v7/v7-boundary-topology-impl.c:546-663` — class filter
  + rescue.
- `v3/steps/v7/v7-boundary-classify-impl.c:108-154, 328-527` —
  perimeter-edge algorithm this design draws from.
- `v3/steps/v7/v7-roof-graph-format.h` — plane flags. Reserved
  `HAS_CLASS_STATS = 0x0004` was never wired.
- bench-v2 `lib/pipeline/steps/boundary-3d.js` — original class filter
  + rescue logic; deletes ground planes instead of tagging.
