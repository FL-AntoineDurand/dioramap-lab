---
id: "2026-05-10-regularization-cost-architecture"
date: 2026-05-10
title: "Regularization Cost Architecture — Saturating Shapes, Sum Aggregation, Linear WSD"
tags: [boundary-regularize, regularization, cost-function, normalization, gaussian, wsd, harness]
category: review
status: concluded
refs: [
  "2026-04-27-regularization-exploration",
  "2026-02-26-idea-angle-snapping-conditional",
  "2026-02-26-idea-global-direction-clustering",
  "2026-02-26-idea-mbr-shape-fitting",
  "2026-02-24-v2-champion",
]
pipeline: v7
design_doc: ""
---

## Why this entry

Two weeks of follow-up work on `boundary-regularize` after the April
exploration entry (`2026-04-27-regularization-exploration`). That earlier
entry concluded R.1 as "global greedy coordinate descent with WSD + angle
displacement cost terms". This entry documents everything since:

- Six rounds of cost-function reform driven by specific observed
  pathologies on the dahouet test case (P3X0, P3C12, P3C2, P9X3, P9C1).
- New algorithm structure: continuous in-loop merge + continuous
  collinear-corner removal, both threaded into R.1's iteration.
- A standalone replay harness so the algorithm can be exercised against
  a frozen pipeline snapshot in ~30 seconds instead of rerunning the full
  ~50 second pipeline.
- The cost function landed at a clean architecture worth recording:
  per-term saturating shapes for snap-targets, linear-unbounded for WSD,
  sum (never average) for aggregation across sub-units.

The work spans regularize.js versions 12 → 29 and commits
ca8473f → bc8a1cd. The reform was empirical — each pathology was
found by inspecting a specific corner, and each fix was validated by
re-running the replay harness and comparing P9C1 / P3X0 outcomes.

## Goals and constraints

What R.1 is trying to do, in plain language:

1. Snap free corner angles to canonical values (90°, 45°, 135°, 180°,
   optionally also 30°/60°/120°/150°). The boundary detector leaves
   corners off by 5–25°; geometric reality has them at canonical
   values for most buildings.
2. Pull near-horizontal edges to exactly horizontal (eaves on tilted
   roofs are typically horizontal in real buildings).
3. Keep polygons fitting their owner point clouds — the boundary
   already does, but per-corner moves can break this.
4. Remove false intermediate corners that the boundary detector
   inserted but that don't represent real building features.

Why all four are needed: boundary-classify hands us a roof graph with
real geometric noise. Corner positions land where the local boundary
trace happened to break, edge directions reflect where the gradient
field happened to peak. R.1 nudges everything toward what the building
*should* look like, within the constraints of each corner's degrees
of freedom (DOF).

### Why global, not per-plane

The April entry already covered this — see the discussion of "wall hit"
attempts there. Short version: the most interesting corners are shared
across planes (ridge corners, hip junctions, eave-shared corners), and
per-plane optimization can't touch them. Global descent with DOF-aware
candidates is the only way to address shared corners.

### Why two-corner joint moves were considered (and what we settled for)

Greedy single-corner descent can't translate an *edge* as a rigid body.
If both endpoints need to move together to slide the edge toward MR
points or onto a collinear group line, single-corner moves can only
rotate the edge (move one end), which is the wrong primitive.

Joint two-corner moves were prototyped in-conversation. The
implementation didn't persist into the committed file — but the
in-loop merge (R.2) accomplishes a related goal: when a single-corner
move brings the corner close enough to another constrained corner, R.2
collapses them rather than letting them shear an edge. For the
edge-slide and collinear-group cost terms (currently off by default),
joint moves remain the right primitive — to be reintroduced.

### Why neighbour angles are also scored

R.1 picks a move at corner C by computing the change in total cost.
Moving C changes the angles at C *and* at C's one-hop neighbours
(because the edges (C, neighbour) rotate). If only C's own angle were
scored, the optimizer would accept moves that improve C's angle by 5°
while bending neighbours by 20° each — which is geometrically a worse
graph. Including neighbour angles in the score prevents that.

Two-hop and beyond are not scored: moving C only changes edges
(C, neighbour), not edges (neighbour, neighbour-of-neighbour), so
two-hop angles are unaffected.

## Prior work and abandoned ideas

The full research-history chain for "regularize building geometry":

```
2026-02-24  V2 Champion (Track A/B/C)                  [v2 lineage, concluded]
2026-02-26  Idea: Angle Snapping (BDTOPO priors)       [v2-era, abandoned]
2026-02-26  Idea: Global Direction Clustering          [v2-era, abandoned]
2026-02-26  Idea: MBR Shape Fitting                    [v2-era, abandoned]
2026-04-27  Building Geometry Regularization           [v7 lineage, concluded]
2026-05-10  Regularization Cost Architecture (this)    [v7 lineage]
```

**V2-era ideas** (`refs:` to all three). All three were "snap to a
discovered structure" — different from the canonical-target-set we
ended up with, but exploring the same goal of forcing regular geometry.
Worth a glance if we ever want another regularisation axis in R.1:

- **Angle Snapping (Conditional, with BDTOPO Priors)** — would have
  used BDTOPO footprint edge angles as priors for what canonical angle
  to snap each polygon corner to. Building-specific targets instead of
  the global {45, 90, 135, 180}° set. Abandoned: BDTOPO footprints turn
  out to be noisier than expected and the prior would have introduced
  more error than it removed.

- **Global Direction Clustering (Soft Constraint)** — cluster all
  edge directions across the whole building to discover the dominant
  axes; pull edges toward the cluster representatives. Captures
  arbitrary building orientations without canonical assumptions.
  Abandoned: orthogonal but related to the canonical-target work — and
  the canonical set turned out to cover most cases.

- **MBR Shape Fitting** — minimum-bounding-rectangle via rotating
  calipers, used to bias the entire polygon toward a regular rectangle
  if it's close. Abandoned: too coarse for the small-scale corner
  nudges R.1 needs to do.

The current cost-function architecture could revisit any of these as
*additional* terms (e.g. add a `useBdtopoAngles` term that pulls each
polygon corner toward the nearest BDTOPO-footprint angle instead of
the canonical set; or a `useDiscoveredDirections` term that clusters
edge directions per-run and pulls edges toward cluster medoids). The
saturating Gaussian + sum-aggregation framework would absorb any of
them cleanly.

**V2 Champion (Track A/B/C)** was the production pipeline before the
v3 / v7 rewrite. Its "regularization" was less ambitious — shape-fit
to a footprint, obstacle-filter, no per-corner snap. Cited here for
completeness of the research history: regularisation in v2 was a
post-shape-fit pass, not a global graph optimizer. The shift to
v7's per-corner optimizer-with-cost-terms is a substantively different
approach.

**V7 lineage proper** is the April→May chain documented in this entry's
body.

## Algorithm structure today

R.0 — DOF tagging. Each corner classified by how many planes it
belongs to:

- **3+ planes → DOF 0**: locked at the algebraic intersection of plane
  equations. Cannot move. Hip junctions, building apexes.
- **2 planes → DOF 1**: can slide along the ridge line between the
  two planes. Ridge end corners, level-transition corners.
- **1 plane → DOF 2**: can move on the plane surface, two axes of
  freedom. Eave/free corners.

R.0b — polygon cleanup. Drops corners that no polygon path uses;
collapses three consecutive ridge corners (the middle one is a
redundant midpoint).

R.1 — greedy coordinate descent loop. Per iteration:

1. For each `dof > 0` corner, generate candidate moves along the
   corner's DOF axes (sample ± nudgeRange = 0.15 m at nudgeStep = 0.03 m).
2. Score each candidate via `totalCost`. Pick the single best move
   across the whole building.
3. Apply it.
4. **R.2 in-loop**: try to merge the just-moved corner with any other
   corner within `proximityMergeDist = 0.30 m` sharing a plane.
   Survivor = lower DOF, more planes, lower id. Chain: a merged
   survivor immediately attempts further merges.
5. **R.3 in-loop**: scan all single-plane corners; remove any whose
   polygon angle is within `collinearRemovalThresholdDeg = 10°` of
   180° (redundant midpoints).
6. If `bestImprovement < 0.001` or `iter = maxIterations`, stop.

Convergence on dahouet: typically 700–1500 iterations, well under the
3000 cap, in 20–40 seconds.

## Cost function architecture

Total cost at corner C is a weighted sum of per-term costs:

```
totalCost(C) = λ_WSD            · wsdCost(C)
             + λ_angle          · angleCost(C)
             + λ_neighborAngle  · neighbourAngleCost(C)
             + λ_horiz          · horizontalCost(C)
```

Four active terms today, each with its own shape:

### WSD — linear, unbounded

WSD is a "less is better, no natural upper bound" loss. Saturating it
makes "very bad" indistinguishable from "bad" — the optimizer happily
collapses polygons once they cross the saturation threshold because no
gradient remains to pull them back.

```js
per_plane_cost = totalWsd / wsdSigmaM       // wsdSigmaM = 1.0 m
wsdCost(C) = sum over planes affected by C
```

`totalWsd` is the two-sided wrong-side distance: own-plane points
outside the polygon (completeness) plus nearby-foreign points inside
(precision, skipped on walls). Per-point distance is weighted by trust
= `1 − clamp(residual / distThreshold)` so a point at 11 cm residual
contributes less than one at 1 cm. Trust comes from the
`point-plane-residual` artifact emitted by grow-refit-merge.

No saturation, no per-point averaging. The "average per point"
attempt (totalWsd / numPoints) was a documented bug — see "WSD trace
on P9" below.

### Angle, horizontal — Gaussian saturating, [0, 1] per item

These terms snap toward canonical targets (angle: 90°, 180°, etc.;
horizontal: edge tilt = 0). The saturating Gaussian is structurally
correct here: far-from-target geometry shouldn't feel an artificial
pull (a 30° hip corner shouldn't get yanked toward 0°), and the
near-target gradient is steep so the optimizer notices small
deviations.

```js
const saturate = (xSq, sigmaSq) => 1 - Math.exp(-xSq / (2 * sigmaSq));
// switchable to 'huber', 'linear', 'quadratic-clamp' via costShape param
```

Per item (polygon context for angle, incident edge for horizontal):

```js
angle_per_context = saturate(deviation_rad², ANGLE_SIGMA_RAD²)  // ∈ [0, 1]
horiz_per_edge    = saturate(sin²(tilt), HORIZ_SIGMA_RAD²)      // ∈ [0, 1]
```

`σ` is set so typical operating values land in the steep mid-range:

- `angleSigmaDeg = 5°` → cost is ~0.39 at 5° dev, ~0.86 at 10° dev,
  saturates near ~3σ = 15°.
- `horizontalEdgeSigmaDeg = 15°` → cost is ~0.05 at 5° tilt, ~0.39 at
  15° tilt, saturates near ~3σ = 45°. Steep edges (typical 30°+ rakes
  on hip roofs) saturate to ≈ 1, zero gradient, no spurious force
  trying to flatten them.

### Angle — frozen target lock

At iteration 0, every corner picks its nearest canonical target if
initial deviation is within `angleSnapThreshold = 20°`. That target is
frozen for the rest of R.1. The cost is `(angle − frozenTarget)²` —
no cliff, no possibility of cross-target drift (e.g. a corner at 158°
locked to 150° can't escape to 180° by drifting through 165°).

Corners with initial deviation > 20° get no target and pay zero angle
cost — they're "genuinely non-canonical" and free.

With `useExtendedAngles = false` (default), targets are {45, 90, 135,
180}°. With true, {30, 45, 60, 90, 120, 135, 150, 180}° — wider coverage
for hip-like geometries.

## Sum, not average

Every aggregation across sub-units sums; nothing averages.

Sub-units per term:

| Term | Sub-units | Typical count | Typical per-corner total |
|---|---|---|---|
| WSD | affected planes | 1–3 | 0.1–3 (linear in m of violation) |
| Angle | polygon contexts | 1 (free), 2–3 (ridge/hip) | ≤ 1, ≤ 2, ≤ 3 |
| Neighbour-angle | one-hop neighbours | 3–4 | ≤ 3, ≤ 4 |
| Horizontal | incident edges | 2–4 | ≤ 2, ≤ 4 |

The sum-vs-average decision was the single most impactful structural
choice. Averaging looked elegant — "each term is in [0, 1], so
λ_X is a dimensionless mixing weight" — but it dilutes signal in three
ways:

1. **Per-point WSD dilution** (the catastrophic one): dividing
   `totalWsd` by `numPoints` made a polygon with 35 violators at avg
   10 cm wrong-side look exactly as "bad" as one with 3.5 violators at
   the same avg. P9 traced from `wsd=0.69 m` (19 points wrong) to
   `wsd=3.32 m` (35 points wrong) during a run, but
   `cost = 0.0038/0.05 = 0.075` — the optimizer accepted single-iter
   jumps of +2 m in total wrong-side distance because the *average* per
   point barely changed.

2. **Per-context angle dilution**: averaging over polygon contexts
   halved a ridge corner's angle cost (a bad angle in plane A averaged
   with a good one in plane B). The corner needs to satisfy *both*
   polygons, not the average.

3. **Per-edge horizontal dilution** (the P9C1 case): the rake edge of
   a 44.6°-tilted plane is saturated at cost ≈ 1 and unmovable. The
   eave edge starts at 9° tilt (movable). Averaging makes the cost
   `(1 + sat(eave)) / 2 = 0.5 + sat(eave)/2`, halving the eave's
   gradient because the rake's saturated 1 is "drag".

Fix in all three places: just sum. Each per-item contribution is
already bounded ([0, 1] for saturating terms, no natural bound for
WSD), and summing produces totals that scale with severity rather than
being averaged down by quantity. Each one of these dilutions was found
by tracing a specific symptom — see the next section.

## Pathologies and their fixes

The dahouet snapshot kept producing visible failure modes. Each one
pointed at a structural cost bug.

### 1. The cliff-flee (pre-Gaussian angle cost, fixed in v15→16)

Symptom (`--lambdaAngle=100`): P3X0 went 93.5° → **107.4°** — moved
*further* from 90°. The original angle cost was a quadratic well
clipped at a 10° threshold, then zero beyond:

```js
if (deviation < 10°) cost += deviation²;
```

Past the cliff the cost was exactly zero, with no force to come back.
The optimizer learned that the cheapest way to kill angle cost was to
**push the angle past 10° dev**.

Fix: frozen target at iter 0, no threshold during R.1. The cost is
now a single quadratic well around the chosen target — no escape
hatch.

### 2. The unit-mismatch lambda race (v17)

Symptom: angle cost contributing ~0.5 to total, WSD cost contributing
~10. Angle had no chance to win any move. Old λ_angle = 0.5 with raw
value `(deviation_rad)²` ~ 0.01 = 0.005 total. WSD `totalWsd / 1`
in metres ~ 10 = 10 total. Three orders of magnitude apart.

Fix attempt #1: crank λ_angle to 100. Worked numerically (matched the
gradient) but obscured the structural problem.

Fix #2 (the real one): normalise all costs to [0, 1] via saturating
shapes. λ becomes a dimensionless mixing weight.

### 3. The Gaussian-saturated WSD trap (v25)

After normalisation, WSD used `1 − exp(−avgWsd²/2σ²)` with σ = 0.05 m
typical and `avgWsd = totalWsd / numPoints`. Looks clean, but:

Symptom: corners moved *up the slope*, eaves no longer covering
their plane's lower points, the polygon shrunk dramatically — and the
optimizer rated this as a *better* configuration than the original.

Tracing WSD on P9 across 1209 iters (instrumented runRegularize with
`traceWsdPlanes: [9]`):

```
iter   | wsd_dist | count | total | avg     | cost(σ=0.05) | mover
-------+----------+-------+-------+---------+--------------+--------
   -1  |   0.69 m |   19  |  883  | 0.0008  |    0.016     | (initial)
  500  |   2.02 m |   28  |  883  | 0.0023  |    0.046     | P87C0
  716  |  15.12 m |   55  |  883  |   ...   |     ...      | P9C1
 1208  |   3.32 m |   35  |  883  | 0.0038  |    0.075     | P3C5
```

At iter 713–716 P9C1 made moves that increased total wrong-side
distance by 1–2 m *each*. Cost barely registered because the
per-point average was diluted by all 883 plane points. The Gaussian
made it worse: at 3σ ≈ 0.15 m avgWsd, the cost was already saturated
to ~1.0 — even a 5 m total violation later in the run had a cost
indistinguishable from a 0.5 m one.

Two bugs in one:

- The Gaussian *shape* is wrong for WSD: WSD has no natural maximum,
  so saturation discards the signal precisely where it should be
  strongest.
- The per-point *averaging* is wrong even with the right shape:
  diluting by total plane size masks severity.

Fix: linear, unbounded, total-not-average.

```js
per_plane_cost = totalWsd / wsdSigmaM   // σ = 1.0 m
wsdCost(C) = sum over affected planes
```

Re-traced after fix: P9 total WSD went *down* from 0.69 m to 0.64 m
across the run. Single-iter WSD jumps capped at +0.12 m (vs +2.4 m
before). WSD-increasing iterations dropped from 30 to 6.

### 4. The averaging-everywhere problem (v29)

Once WSD was fixed, the same dilution pattern was visible in three
other places (angle/polygon-contexts, horizontal/edges,
neighbour-angle/neighbours). Each compounds: a ridge corner's angle
signal halved by averaging across its two polygons; a horizontal cost
halved by being averaged with the unmovable saturated rake edge of
its plane. Fixed by switching all three to sum. See "Sum, not
average" above.

## Continuous in-loop merges and removals (R.2 + R.3)

A specific dahouet case (P9X3) drove the design.

P9X3 was an "intermediate false corner" between two real corners:
P9C1 (a hard corner) and RE20+RE23+MRG14 (a ridge end). P9X3's angle
in P9's polygon was 158°. R.1's angle cost should snap it to 180° and
make it disappear as a redundant midpoint — but with the original
threshold of 10° and deviation 21.6° from the nearest base target
(180°), no lock was applied, so the angle cost contributed zero.
P9X3 then drifted under WSD pressure until it sat 36 cm from
RE20+RE23+MRG14.

This exposed two distinct algorithm gaps:

- **Proximity merge** (R.2): when one corner has slid close to a
  more-constrained corner sharing a plane, they should *merge*. The
  one with lower DOF (or more planes, or lower id) survives. The
  loser's planeIds, ridgeIds, and recorded path are absorbed into the
  survivor. Polygon paths get redirected; degenerate (a, a) edges
  collapse; (loser, neighbour) edges become (survivor, neighbour).

- **Collinear corner removal** (R.3): a single-plane corner whose
  polygon angle is within 10° of 180° is a redundant midpoint. Its
  two incident edges are essentially co-linear. Remove it; the two
  edges become one. Implemented as a "merge into the polygon
  predecessor" via the same redirect+rebuild machinery as R.2.

Both run **continuously inside R.1**: after every accepted move, the
just-moved corner is checked for proximity-merge eligibility (with
chaining — one move can collapse multiple intermediates), then the
whole graph is scanned for collinear corners. This is a major change
from the original post-R.1 single-pass merge — it lets corners
collapse opportunistically as the optimizer pushes them, rather than
all-at-once at the end when the geometry has finished moving and the
optimizer can no longer leverage the simplified topology.

Shared plumbing: a single `redirectAndRebuild(loserId, survivorId)`
helper does the polygon-path/edge/cornerById/cornerContext rewrite
and re-locks angle targets on the new neighbour pairs. Both R.2 and
R.3 go through it.

Effect on dahouet: 5 proximity merges, ~10 collinear removals per
run. P9X3 is consistently absorbed into the polygon as a 180°
redundant midpoint and removed.

## Tooling: standalone replay harness

A diagnostic loop of "edit cost function → re-run full pipeline → see
the effect" takes 50 s per iteration. Far too slow for the kind of
empirical tuning the cost reform required.

Built a snapshot + replay pair:

- `scripts/dump-regularize-input.js`: runs v7 pipeline up through
  `boundary-classify` (the step before regularize) and serialises
  exactly the artifacts R.1 reads —
  `roof-graph`, `positions`, `owner`, `plane-equations`,
  `nearby-projections`, `point-plane-residual`,
  `point-plane-distance-threshold` — to
  `/tmp/regularize-snapshot-<case>.json`. Uses the production
  `runPipeline` with `stopAfter` option so the captured artifacts
  match a real bench run bit-for-bit. ~50 s once.

- `scripts/regularize-replay.js`: loads the snapshot, calls
  `runRegularize(inputs)` directly (no `PipelineContext`, no artifact
  layer), runs the algorithm in ~30 s. Supports `--key=value`
  overrides for every parameter and `--inspect=NAME` for detailed
  per-corner before/after traces.

To enable this, `boundary-regularize.fn(ctx, params)` was split into
`readInputsFromCtx(ctx, params)` + `runRegularize(inputs)` — the
latter is a pure function (no `ctx`, no `addArtifact`, no `log.push`)
that takes a self-contained inputs bundle and returns
`{ roofGraph, logs, stats }`. Pipeline glue is the thin wrapper.

The harness was indispensable for the cost-function reform. Every λ
sweep, every cost-shape change, every "does P9C1 lock to 90° now?"
question went through the harness in ~30 s instead of ~50 s.

It also unlocked **per-iteration WSD tracing**: `traceWsdPlanes`
input param accumulates per-iter wsd_distance for chosen planes and
returns it in stats. This is how the per-point dilution bug was
found — a single instrumented run produced the WSD trace that made
the bug obvious.

## Per-point trust weighting (the WSD fairness layer)

Before R.1, every owner point counts equally in the WSD violation
total. But owner assignment in `grow-refit-merge` is a binary
decision (a point either belongs to plane P or it doesn't), and a
point at 11 cm residual is barely a member while one at 1 cm is firmly
on the plane.

Added a per-point trust weight to WSD:

```js
trust(point i) = 1 − clamp(residual[i] / distThreshold, 0, 1)
```

`residual[i]` is the point's signed distance to its assigned plane's
equation, cached as the `point-plane-residual` artifact emitted by
`grow-refit-merge`. `distThreshold = 0.12 m` is the same constant
used during ownership decisions, so a point at the assignment border
gets weight 0; a point on the plane gets weight 1.

For nearby foreign points (precision side of WSD), trust comes from
the *foreign* point's residual to its own plane (via `meta.srcIdx` on
each nearby item). A nearby point sitting tightly on its own plane is
a strong "don't engulf me" signal; one floating between planes is
noise.

Unassigned points (residual = +Infinity) get weight = **1**, not 0.
We discussed both — weight = 0 would mean the optimizer ignores them
entirely; weight = 1 means the polygon shouldn't engulf them either,
since they exist as physical points even if the segmenter couldn't
classify them.

The trust artifact is also useful for visualisation (colour-coded
per-point reliability) but that's not yet wired in the UI.

## Default parameter set as of this entry

```js
maxIterations             = 1000
nudgeRange                = 0.15 m
nudgeStep                 = 0.03 m

angleSnapThreshold        = 20°    // initial dev for target lock
useExtendedAngles         = false  // {45, 90, 135, 180}

// Active cost terms (all on, normalised, sum-aggregated)
useWSDCost                = true
lambdaWSD                 = 1.0
wsdSigmaM                 = 1.0 m  // linear scale, NOT Gaussian σ

useAngleCost              = true
lambdaAngle               = 2.0    // modest priority for snapping
angleSigmaDeg             = 5°     // Gaussian σ on dev from target

useNeighborAngleCost      = true
lambdaNeighborAngle       = 2.0

useHorizontalEdgeCost     = true
lambdaHorizontalEdge      = 1.0
horizontalEdgeSigmaDeg    = 15°    // Gaussian σ on edge tilt

// Cleanup
useProximityMerge         = true
proximityMergeDist        = 0.30 m
useCollinearRemoval       = true
collinearRemovalThresholdDeg = 10°

// Inactive (under-tested or pending re-introduction)
useDisplacementCost       = false
useMrFitCost              = false  // edge slide toward MR points
useCollinearCost          = false  // cross-plane near-collinear edges

// Shape
costShape                 = 'gaussian'  // for saturating terms only
```

On dahouet with these defaults: ~750 iterations to converge, 89/142
angles within 1° of target (63%), 3 proximity merges, 11 collinear
removals, P9C1 → 91.9°, no polygon-collapse failures.

## Considered but unimplemented / inactive

### Joint two-corner moves

The cleanest fix for translation-of-an-edge (eave slide toward MR
points, collapse onto cross-plane collinear group line). Sketched
in-conversation, not yet committed. Would let the optimizer evaluate
moves of the form "translate corners A and B by the same in-plane
vector" rather than only single-corner moves.

### Cost-shape switching per term

Right now `costShape` is global. Could naturally become per-term
(`wsdShape = 'linear'`, `angleShape = 'gaussian'`, etc.) since WSD
already departs from the global default. Trivial change; not yet
exposed.

### λ as true mixing weights vs σ tuning

The current λ defaults (1, 2, 2, 1) are a small priority bump on
angle. With σs tuned so each term's typical operating value sits in
the steep gradient region (`wsdSigmaM = 1`, `angleSigmaDeg = 5`,
`horizontalEdgeSigmaDeg = 15`), the λs work as true mixing weights —
λ = 2 means "twice as important as λ = 1". Earlier experiments needed
1500 / 12 / 48 / 20 to balance the terms because σs were mis-tuned
(see commit `bc8a1cd` body for the analytical derivation).

### Per-point trust visualisation in UI

The `point-plane-residual` artifact is rendered as a gradient layer
under "scope: full-cloud" but the per-point trust value isn't shown
explicitly. Worth adding a "WSD trust per point" layer for the next
diagnostic loop.

## Open questions / next directions

- **MR-fit cost term**. Implemented (`useMrFitCost`) but currently
  off. Needs joint two-corner moves to be effective — single-corner
  moves can only rotate the edge, not slide it.
- **Cross-plane collinear-group cost term**. Same blocker as MR-fit.
- **Watertighting** (task #6, still pending). Adjacent polygons should
  share exact vertices/edges — currently they often differ by 1–5 cm
  at shared corners. Separate concern from R.1, but the regularize
  output is what drives the watertight pass.
- **Bench score vs algorithm score**. Currently no aggregate metric
  ranks "regularize run A vs B" beyond the snap percentage. Would
  benefit from a per-corner WSD + angle deviation + horizontal tilt
  summary across the whole building.
- **Tuning across cases**. All work to date is on dahouet alone. The
  σ choices are dahouet-typical; other buildings with different
  noise / scale may need different σs. A multi-case sweep would
  validate.

## Commit history (reverse chronological, regularize-touching)

```
bc8a1cd  cost reform: normalised + linear-unbounded WSD + sum aggregation
c4d607a  in-loop merge + collinear removal + path viz + harness
0478a70  standalone replay harness
251efb6  per-point WSD trust weighting + 3 side-quest improvements
5d29a25  toggleable scorer terms + real two-sided WSD
31824c5  R.0b polygon cleanup + DOF viz overhaul
ca8473f  refactor: strip R.0, add DOF tagging, scope R.1 to free corners
c03df90  filter hip ridges + update ridge from/to
ecd89b4  raise convergence threshold to 0.01°
6ed5f81  R.0a as iterative weighted circular mean
f7b7746  simplify downslope arrows
ffda14d  downslope arrows from final plane equations
feecae1  compute ridge Y-slope from geometry, not normals
90fc28c  iterative ridge snap + post-snap viz + plane labels
0717863  R.0 plane equation regularization + viz symbols  [abandoned]
2d5c5f3  constrain corner moves to plane surfaces
e69b325  cap displacement + angle annotations
690bdc2  global corner optimizer with angle regularization
74edc5c  scaffold pass-through step for Pass 4
```
