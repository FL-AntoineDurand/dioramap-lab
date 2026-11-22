---
id: "2026-07-03-type-aware-regularization-plan"
date: 2026-07-03
title: "Toward plane-type-aware regularization — state, metric inventory, and the plan"
tags: [design, boundary-regularize, plane-type, wall, ground, roof, cost-function, make-shared, plan, v7]
category: design
status: proposed
refs:
  - "2026-07-02-ground-plane-classification-design"
  - "2026-07-02-corner-insertion-lands"
  - "2026-07-02-marginal-compactness-gate"
  - "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
pipeline: v7-bp2
---

> **This entry is the guiding plan** for the next arc of `boundary-regularize` work.
> It records where we stand across every related subject and the ordered plan we
> committed to, so it can serve as the reference going forward. It builds directly on
> the classification infrastructure designed in
> [[2026-07-02-ground-plane-classification-design]] (Stream B), which this entry
> assumes lands first (it puts plane *type* — ground / wall / roof — on the wire).

## 0. TL;DR

Regularize's cost function is **plane-type-blind**: it applies roof-shaped aesthetics
(clean ~90° corners, horizontal eaves) to *every* plane, including walls and ground.
Chasing better *metrics* (compactness, vertex-support) kept hitting this wall. The fix
is not another metric — it is to make regularization **aware of plane type**:

1. **Type ground planes and exclude them from regularize.**
2. **Refuse corner merges between different plane types** (roof↔wall, wall↔ground) so
   the polygons decouple and move independently.
3. **Don't regularize walls at all at this stage** — a later step (`v7-wall-detect`)
   detects missing walls and cleans wall shape; regularizing them now is wasted and
   actively harmful.

## 1. Where we stand (recently landed)

- **In-loop corner insertion (#25)** — [[2026-07-02-corner-insertion-lands]]. Fixes the
  *under-coverage* failure mode (plane 0 terrain 67% → 1%). Committed, param `cornerInsert`.
- **Marginal-compactness gate** — [[2026-07-02-marginal-compactness-gate]]. Fixes the
  *drift-into-void* failure mode (a corner sliding into empty space where WSD's
  `foreign_in` is silent). Per-plane, both accept paths, absolute eps. Committed, param
  `compactGate`. **Caveat (see §5): every drift win it demonstrated was on a WALL.**
- Both are on branch `regularize-insertion-compactness-gate`, not yet merged to main.
  Uncommitted on that branch: a vertex-support *force* prototype (`BR_LAMBDA_SUPPORT`),
  a `MAXSUP` per-vertex support diagnostic, and a `BR_PIN_TRACE` cost-decomposition.

## 2. The finding that reframed everything — id=166

Corner **P9C3+P9C6 (id=166)** sits ~2 m out in an empty area on wall plane 9. It is a
*static* merge product (a merge of two plane-9 corners) that **never moves** during
regularization (`ctraj=0`), and no cost term dislodges it.

We tried, in order, and each failed for a *diagnosable* reason:
- **Compactness gate (a veto)** — can't retract it; a veto prevents drift, it doesn't
  *pull* a statically-misplaced corner back.
- **Weighted compactness (a force)** — doesn't move it either: plane 9 is already
  compact (1.36); id=166 isn't the compactness driver, and retracting it lowers *both*
  perimeter and area, so `perim²/area` barely changes → no gradient (a real geometric
  fact, not an impl bug).
- **Vertex-support force** — measured id=166 at 2.08 m from the nearest point (worst on
  plane 9; a genuine empty area). The force *works on free corners* (terrain corner id298
  retracted 9.71 → 3.85 m) but **not** id=166 — it's *pinned*.

**The pin trace (`BR_PIN_TRACE`) is the key result.** Stepping id=166 0.5 m toward its
nearest point, per-term Δ (Δ>0 resists):

```
wsd  +0.0063   ← FLAT: coverage doesn't care (empty area, proven)
ang  +0.8450   ← THE PIN: corner-angle term
nang +0.1551   horiz +0.0978   vcop +0.0845
supp −1.0000   ← support correctly pulls in, but is outvoted
TOTAL +0.1888  → inward REJECTED
```

id=166 is held by the **corner-angle reward** — it sits at a clean ~90° corner and moving
it toward the data breaks that angle. **But it's a WALL corner**: its two edges should
tend *vertical* (toward the ground), not form a 90° roof corner. The optimizer is
rewarding a geometry that is correct for a roof and wrong for a wall, in a place with no
data to justify it either way. That is a **type** problem, not a metric problem.

## 3. Metric inventory (what's active, what we tried, good & bad)

Active cost terms (`TOTAL_COST`, each `λ·term`):

| term | λ | good | bad |
|---|---|---|---|
| WSD (own_out + foreign_in) | 1.0, linear/unbounded | load-bearing coverage force | `foreign_in` **silent in empty space** (drift); extensive → scale-couples all terms; spikes when a plane is under-built |
| angle | 2.0 | clean corners | **rewards ~90° in the void → pins id=166**; roof-shaped, wrong for walls |
| neighbor-angle | 2.0 | smooth rings | saturates near max; co-pins |
| horizontal-edge | 1.0 | horizontal eaves | rewards horizontal edges into empty space; **roof-only, applied to walls** |
| horiz/vert-coplanar | 1.0, pairwise | aligns parallel faces | fallback = 1.0 when no partners → constant, non-discriminating |
| collinear | 1.0, pairwise | straightens | fallback = 1.0 constant |

Tried this session:

| metric / op | status | good | bad |
|---|---|---|---|
| compactness — weighted λ | off (kept as counter-example) | a force (retracts) | scale-coupled; pushes toward a roundness *target* → distorts thin shapes; weak on bulges; can't move id=166 |
| compactness — gated veto | committed, on in pipeline | scale-free; cleanly retracts *drift* | a veto not a force → can't retract *static* mis-placement; wins were all on walls (§5) |
| vertex-support — force | prototype (env, uncommitted) | point-aware; retracts *free* unsupported corners (id298) | can't beat the angle-pin; needs tolerance + density-awareness; brute-forcing damages real planes; misfires on sparse-but-legit corners |
| corner insertion (#25) | committed, on | fixes under-coverage | per-insert REBUILD perf |
| slide-clamp (DOF-1 bound) | committed, default-off | directly bounds slide | too blunt — broke 7 legit-slide planes; superseded by the gate |

The recurring lesson: **single global metrics keep failing because the right behavior is
conditional** on data presence and plane type. Three conditioning axes emerged and
compose — gate by *marginal effect* (the compactness gate, landed), gate by *support*
(rejected — see §4), and **gate by plane type** (this plan).

## 4. Decision: NOT support-gating; type is the right axis

We explicitly rejected "gate the angle term by support" (attenuate angle reward where a
corner has no nearby points). Reasons:
- **Support is a symptom, type is the cause.** id=166's angle reward is wrong because it's
  a *wall* corner, not because it's unsupported.
- **Support-gating would misfire** on legitimately sparse-but-real corners (the terrain
  ring corner id298 at 9.71 m is *fine*; a sparse real roof corner would be wrongly
  attenuated).
- The precise defect (roof aesthetics on a wall) has a precise fix (type-awareness); a
  general support lever is a blunter instrument with side effects.

## 5. The plan (ordered — the steps depend on each other)

**Step 1 — Plane typing on the wire (prerequisite).**
Land / consume the ground-plane classification step from
[[2026-07-02-ground-plane-classification-design]]. It tags planes non-destructively
(`RG_PLANE_LIKELY_GROUND`, class stats) and reuses `rg_plane_is_wall`, so it yields
**roof / wall / ground** types that everything downstream keys off. Runs *before*
regularize.

**Step 2 — Regularize skips ground planes.**
Free compute (ground is the pipeline's cheapest to skip and never consumed as roof), and
it removes the fake-extra-roof miscount. Geometry is preserved (tag, don't delete).

**Step 3 — Type-gate `MERGE_CORNER` (decouple polygons).**
Refuse corner merges between different plane types (roof↔wall, wall↔ground). Precedent
already exists: the make-shared cascade type-gates *ridge* welds to `{ridge, hip, valley}`
and **excludes step (roof↔wall) and flashing** — this extends the same gate to the
proximity-based `MERGE_CORNER`, which currently still welds cross-type corners by distance.
Effect: roof and wall carry their own corner at a shared eave and move independently.
**This is a refinement of the make-shared work, not a reversal** (that work was already
same-type allow-listed at the ridge level).

**Step 4 — Skip walls in regularize entirely.**
Walls are handled by the later `v7-wall-detect` (detect-missing + shape-clean), so
regularizing them now is wasted *and* harmful (it's what pins id=166). This becomes
well-defined **only after Step 3**: skipping "walls" is only clean once wall corners are
not fused into roof corners. Pipeline order: **regularize roofs first, wall-detect
follows and re-aligns walls to the finalized roofs.**

Ordering is forced: **1 → 2 → 3 → 4.** (Typing enables ground-skip and the merge type-gate;
decoupling enables wall-skip.)

## 6. Consequences to weigh

- **The compactness gate's scope narrows to roof-only.** Every drift win it demonstrated
  (plane 6, walls 9 and 15) was on a *wall*. If walls leave regularize, the gate becomes
  largely dormant. Keep it committed-but-quiet (the drift mechanism, the both-paths
  finding, and the false-plane methodological lesson all stand); **re-measure whether any
  *roof* drift survives** once walls are excluded (roof-side step-corner drift may still
  occur).
- **Corner insertion is unaffected** — under-coverage is a roof problem, fully in scope.
- **Overhang loss (#31) is part of the decouple story.** Roof planes get cut at wall
  junctions precisely because roof and wall corners merge; Step 3 (decouple) is likely
  part of the overhang fix — a roof eave that legitimately overhangs the wall should not
  be welded back to the wall corner.

## 7. Open questions / risks

- **Coincident corners at decoupled eaves** — Step 3 leaves two corners (roof-side,
  wall-side) at a shared eave. Verify `boundary-classify` / edge classification handle
  coincident cross-type corners (smoke test).
- **Does any roof drift survive** once walls are excluded? If not, the compactness gate is
  effectively retired to a backstop.
- **Wall-detect must re-align walls to the finalized roofs** — confirm it anchors on
  regularized roof edges, or roof/wall could diverge at the eave.
- **Shared roof-wall corners** — post-decouple, the roof-side corner is regularized (as
  roof), the wall-side is skipped (left for wall-detect). Confirm this is the intended
  split for real eaves.
- **False planes (42/43/45/50)** — an *upstream* GRM/over-segmentation problem (density
  1-6 vs 18-20 for real facets; see [[dahouet-plane-taxonomy]]), orthogonal to typing but
  it pollutes any per-plane scoring. Tracked separately.

## 8. Task mapping (see task list)

- **Dropped:** "gate angle by support" — superseded by type-awareness (§4).
- **Spine (ordered chain):** land ground+wall typing → regularize skips ground →
  type-gate `MERGE_CORNER` → skip walls in regularize.
- **Related:** roof-wall merge over-simplification / overhang loss (tied to Step 3);
  productionize the vertex-support force (still useful for *free* unsupported roof
  corners); upstream false-plane over-segmentation.

## 9. Relation to the ground-plane entry

[[2026-07-02-ground-plane-classification-design]] solves the *ground* half (tag ground,
skip it, classify before regularize) and establishes the wire format + the
classify-before-regularize placement. **This plan extends that same architecture to
walls** (type-gate merges + skip walls) and records the *why* (id=166: roof aesthetics on
a wall). They are two faces of the same move: **regularization behavior keyed on plane
type**, with geometry never deleted — only tagged and routed.

## 10. Amendment (2026-07-03) — placement moves EARLIER; the decouple is THREE-site

Tracing the corner-merge sites and reading `boundary-polygons-2` (bp2) internals
refines this plan on two points:

### (a) Classification must land before `boundary-corners`, not before regularize

Corners merge in **two** places:
- **`boundary-corners` — the corner POOL** (`v7-corner-pool.c`, geometric super-merge
  by `merge_dist`, distance-based and type-blind). This is the FIRST merge, where
  id=166 / P9C3+P9C6 was born.
- **`boundary-regularize` — the `MERGE_CORNERS` cascade** (T5–T7). The SECOND merge.

Since the decouple must refuse **cross-type** merges, type must be known before the
FIRST merge → classification lands **at/after `boundary-topology`** (which already
computes the class filter / `gvw_frac` / candidate-reject), **before `boundary-corners`**.
This **revises** [[2026-07-02-ground-plane-classification-design]] §5, which placed
classify *before regularize* — correct for its ground-skip goal, but too late for the
decouple.

**Triple decoupling forces ALL THREE types early.** We need to separate wall/ground/roof
from each other, so at the first decouple site (bp2 collection / pool) every plane must
already carry its full 3-way type. Wall = tilt (free, early). But **ground must also be
typed before `boundary-corners`** — which means it can only use signals available at
`boundary-topology`: **S1 (class-2 fraction, already computed — decisive; its only failure
was the ridge-rescue overriding it, so we tag independent of rescue) + S4 (lowest
horizontal) + S5 (class-filter-triggered)**. The geometric ground signals **S2/S3 need
polygons (post-bp2)** so they cannot gate the decouple — they become a *later refinement*
for ambiguous real-data ground, not part of the first build. (Roof = neither wall nor
ground.) FP-on-roof is structurally unlikely — roof points are class-6, so the class-2
fraction won't fire on them.

### (b) The decouple is THREE-site, and bp2 is the decisive one

bp2 assigns a corner to a plane's polygon **purely by proximity to that plane's CONTOUR**
(within 0.75 m; `v7_bp2_compute` Phase 1) — **type- and membership-blind**. So cross-plane
sharing is **re-derived by bp2**, not inherited from the pool merge. Consequences:
- **Type-gating the two merge sites alone does NOT decouple** — bp2 re-collects a
  coincident roof+wall corner into *both* polygons regardless of whether they merged.
- The decisive locus is **bp2's Phase-1 collection**: a plane's polygon must collect only
  **type-compatible** corners (a roof contour takes roof-side corners, not the coincident
  wall one).
- **Three sites**: (1) **bp2 collection [decisive]**, (2) pool super-merge, (3) regularize
  `MERGE_CORNERS`. All need the type tag → another reason type must exist before bp2.

### (c) Option A (postpone all merging to regularize) — considered, rejected

bp2 is contour-proximity-driven, so it does **not** structurally require the merged set
(it rebuilds membership) — postponing wouldn't break bp2's sharing mechanism. But bp2
would collect **duplicate coincident** anchors → redundant/degenerate vertices, and
regularize would absorb all dedup **and** the pool solver's position-blending. Feasible
but messy, and it *still* needs bp2 type-awareness. Not worth relocating the merge
subsystem. **Chosen: Option B** — classify early (topology), keep the merges in place,
make all three sites type-aware.

## 11. Wire representation + storage (decided 2026-07-04)

- **`plane_type` is a single ENUM FIELD, not flags.** `uint8 plane_type ∈
  {UNCLASSIFIED=0, ROOF, WALL, GROUND}` on `rg_plane_record_t`. Wall/ground/roof are
  mutually-exclusive values of one property — flags would allow invalid both-set states,
  make roof implicit, and scatter precedence. A field is one value, always valid, roof
  explicit, precedence resolved once at the writer. (Supersedes the ground-plane design's
  `LIKELY_GROUND` *flag*.) Wire = roof-graph format **version bump**.
- **`boundary-topology` is the SOLE WRITER** (locks the logic + the value): it resolves
  precedence and emits one `plane_type`. Consumers **read**, never recompute.
- **Orthogonal facts stay flags:** `RG_PLANE_CLASS_FILTER_TRIGGERED` (diagnostic — did the
  class-2 filter fire, independent of final type) and `RG_PLANE_HAS_CLASS_STATS`.
- **Helper → reader.** `rg_plane_is_wall` currently computes from tilt (and regularize
  *re-computes it inline* with a different threshold source — an existing N-encoding
  drift). Demote it to a reader: `rg_plane_is_wall(p) → rg_plane_type(p)==WALL`; migrate
  the ~2 sites. Kills the drift while we're here.
- **UI rendering is a first-class requirement, built WITH the classifier** — color planes
  by `plane_type` (roof/wall/ground) in the lab UI so misclassifications are eyeballable.
  A classifier we can't see is a classifier we can't trust.

