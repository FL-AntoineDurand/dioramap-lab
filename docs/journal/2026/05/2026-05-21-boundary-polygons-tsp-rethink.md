---
id: "2026-05-21-boundary-polygons-tsp-rethink"
date: 2026-05-21
title: "Boundary-Polygons Algorithmic Rethink — TSP not Dijkstra, FP-stability not Byte-parity, Topology before Ordering"
tags: [boundary-polygons, TSP, design, FP-precision, topology, v7, open-question, review]
category: review
status: concluded
refs:
  - "2026-05-17-wrong-side-corner-killing-review"
  - "2026-05-10-regularization-cost-architecture"
  - "2026-04-27-regularization-exploration"
  - "2026-04-14-roof-graph-prepass-boundary-3d"
pipeline: v7
design_doc: ""
---

## Why this entry

Five days into chasing the `v2 ↔ v3` byte-divergence on `boundary-polygons`
output for the dahouet test case, we hit a conceptual wall. Five different
floating-point precision bugs got found and fixed; the iso-bench polygon
match rate moved from 6/27 to 7/27. Each fix made the trace traces cleaner,
but the underlying disagreement persisted on the same planes for the same
reasons.

This entry captures the rethink that came out of that wall: what the
algorithm should actually be doing, why the v2 implementation (and our
v3 port) are wrong by construction, and what replacement we converged on.
It also catalogues bugs that DID get fixed along the way (those fixes
remain valid because they were real precision violations, even though
their effect on output will be largely subsumed by the rewrite).

The conversation that produced this is itself the artifact — the
chronological discovery of each layer of mistake is the load-bearing
content, more than any forward-looking design doc would be.

---

## Findings

### 1. The Dijkstra was already gone — we chased a stale comment

The `boundary-polygons.js` meta header described "**D.6 Cycle extraction
(Dijkstra shortest-cycle)**". Multiple investigations (months apart)
assumed that step existed and tried to trace its decisions.

Commit `284428f` (2026-04-11, "feat(boundary-3d): balance ridge walk, loop
extraction, contour-ordered polygon") removed the Dijkstra search entirely
and replaced it with the contour walk. The comment was never updated.
When `boundary-3d.js` was split into `boundary-polygons.js` (`52040a6`,
2026-04-26) the stale comment was carried over.

When porting to v3 we copied the same stale comment into `v3/steps/v7/v7-
boundary-polygons-impl.c`'s header. Multiple agents traced the code
looking for Dijkstra; nobody noticed the search loop didn't exist.

The actual code structure since April:

| Slot in the meta-comment | What's actually there |
|--------------------------|------------------------|
| `D.6 — Dijkstra shortest-cycle` | leaf-prune only (degree-1 vertex removal) |
| `D.6b — contour fallback` | the **only** initial-cycle producer |
| `D.6b fallback when no contour` | centroid-angle ordering of all anchors |

Lesson: stale comments are landmines. Both `boundary-polygons.js` and v3's
doc.json have been updated in commit `aa70e9a` (this session) to describe
the real code.

### 2. Byte parity with v2 was chasing a coin flip

The unstated assumption behind the v2-v3 divergence investigation: **v2's
output is right and v3 should match it**. That assumption is wrong.

The boundary-polygons algorithm depends on sub-mm floating-point precision
at multiple stages:

- D.6b walk: cell-index `floor((u - g_min_u) / cell_size)` can flip when
  the exact quotient sits within ~1e-7 of an integer.
- D.6b smooth × 2 + DP + snap-close: accumulates ~50–100 µm precision
  noise at chain vertices.
- D.6b arc-length anchor projection: when an anchor projects equidistantly
  onto two adjacent contour segments meeting at a vertex, the closest-
  segment tie-break flips on a 3rd-decimal `d²` difference.
- D.7 permute + delete: a single FP-driven swap in the early permutation
  pass changes which vertex looks deletable, deleting different vertices
  in different runs.

Each of these is sub-mm in the floating-point variable that drives the
decision. **The output is therefore a coin-flip on FP rounding**, not a
function of the input geometry.

v2's choice on each plane is one outcome of those coin flips. v3's choice
is another. Neither is "correct" — they're two samples from a Bernoulli
distribution defined by which `d²` comparison happens to round which way.

**Insight:** the actual correctness criterion is *stability*, not parity.
A correct algorithm produces the same polygon when its inputs are
perturbed by sub-millimetre noise. By this criterion, **v2 fails too** on
the same planes where v3 fails. Chasing v2's specific output means
codifying its particular coin-flip residuals into a port.

This invalidated five days of FP-precision archaeology as a primary goal.
The fixes that did land remain valid as cleanups; they just don't deliver
the v2 parity they were aimed at.

### 3. Five real FP precision bugs found and fixed

Even with the wrong success criterion, the investigation surfaced five
real precision violations in v3. All landed:

| # | Bug | Where | Commit |
|---|-----|-------|--------|
| 1 | `source_per_plane` multi-entry override picking convex over concave | v3 iso bench loader, `v7-bench-v2-loaders.c::v7_loader_load_bc_seed_corners` planeAnchors overlay | `115f158` |
| 2 | Snap insertion order — v3 was doing snap-end + insert, then snap-start on modified chain; v2 queries both first then inserts | `v7-boundary-polygons-impl.c::snap_and_extract_loops` | `115f158` |
| 3 | `uv2_t` struct field types `float` → `double` | `v7-boundary-polygons-impl.c` | `115f158` |
| 4 | Snap query `qu, qv` were `float` (truncating the now-double chain coords) | `v7-boundary-polygons-impl.c::snap_and_extract_loops` | `aa70e9a` |
| 5 | `PLANE_GRIDS` metadata `cell_size`, `g_min_u`, `g_min_v` were `float` in the in-memory struct | `v7-boundary-corners-impl.h` + iso loader | `cffc454` |

The cumulative effect on iso-bench plane-match was 6/27 → 7/27 — modest,
because each fix only addresses one layer of the algorithm's general
sensitivity to FP. **All five are sub-symptom corrections.** The disease
is the algorithm itself.

These commits stay in the tree because:
- They make v3 more correct on its own terms (matching v2 byte-for-byte
  on the data path that doesn't depend on the fragile inner loops).
- They are prerequisites for the rewrite (clean inputs make the rewrite
  easier to test).
- They document the failure modes for future readers.

Documentation lives in `v3/docs/plan/THRESHOLD-INVESTIGATION-DAHOUET.md`
§10–§13.

### 4. The contour walk was conflating two unrelated jobs

The contour walk's output was used for two distinct things:

| Job | What it answers |
|-----|-----------------|
| Topology discovery | "Which corners belong to the same boundary group?" |
| Cyclic ordering | "Within that group, what sequence makes a polygon?" |

These are independent problems. They're separable. **The walk did them
together by physically tracing a closed loop and then arc-length-sorting
anchors against that loop.** Both pieces are FP-sensitive in the same
inner loop, so noise from the walk leaks into both topology and ordering
decisions.

The walk's *real* job is the first one: group corners that are near the
same band of high balance-signal. The ordering came along as a free
side-effect of having traced a path — but the side-effect is what made
the algorithm fragile.

If we use the walk *only* to spatially filter corners (an unordered set
of "corners near this signal fragment"), and use a separate proper
algorithm for ordering, the walk's FP fragility stops mattering for
algorithm correctness.

### 5. Permutation + deletion mixing is asymmetric and lossy

`D.7` alternates two move types until no improvement is found:
- Adjacent-pair swap (`permutation pass`) — accept-or-reject under "no
  harm on any axis" gate.
- Single-vertex removal (`deletion pass`) — pick highest-gain accepted
  deletion.

Two failure modes:

**Asymmetry.** Permutation is reversible (swap back). Deletion is not
(can't reinsert). A vertex deleted in round 2 cannot be recovered by
the round-3 permutation. So **information loss is permanent and
order-dependent**.

**Topology contamination.** A vertex looks "deletable" (low gain, nearly
collinear with neighbours) only because the current ordering positions
it next to neighbours that make it look collinear. A different early
permutation produces a different "collinear" vertex. With FP-driven
initial ordering, v2 and v3 delete different vertices, producing
topologically distinct polygons from the same input anchor set.

The fix is not to make deletion smarter. The fix is to **separate
deletion from boundary-polygons entirely**, mirroring the D.7b corner-
nudge decision (deferred to boundary-regularize because vertex existence
is a global decision, not a per-plane one). Deletion is a regularization
concern: regularize sees ridge-snapping, shared-edge merging, and
multi-plane coherence — none of which boundary-polygons can evaluate.

### 6. The problem class is TSP, not Dijkstra

Cyclic ordering through all anchors on a plane = **find the best
Hamiltonian cycle through n nodes**. That's the Travelling Salesman
Problem. Dijkstra finds shortest A → B *paths* and visits only a subset
of nodes; that was the wrong tool, which is why it was removed in April.

Naming the problem correctly gives us the standard algorithm menu:

- **small n** (n ≤ 8 with our cost; 8!/2 = 20160 cycles): exact full
  permutation enumeration (Heap's algorithm) is microseconds.
- **larger n**: multi-start local search (2-opt, or-opt, 3-opt) with
  several initial cycles. Random restart escapes local optima.

> Footnote on Held-Karp: the canonical TSP textbook answer is the
> Held-Karp DP (`O(n² · 2ⁿ)`). It doesn't apply here — our cost isn't
> a sum of per-edge weights, so the DP state can't summarise the
> not-yet-visited future. We're stuck with full enumeration up to its
> practical cutoff, then heuristic local search above. Don't reintroduce
> "Held-Karp" — it's been removed everywhere it once appeared, same
> way the Dijkstra references got purged.

The buildings in our dataset rarely have more than ~30 corners per plane.
At our scale **TSP is essentially solved**. The "how do we get a good
initial order?" question that motivated the contour walk simply
dissolves: multi-start makes the initial order irrelevant. The algorithm
becomes deterministic up to true ties in the cost function (which we
break by anchor id).

### 7. Topology has real cases beyond "one outer perimeter"

A plane can have:

- **One outer perimeter** — typical roof face.
- **One outer + one or more inner holes** — court, chimney footprint,
  skylight cutout.
- **Multiple disconnected boundary components** — rare on a single plane
  but possible.

Pure TSP through all corners would weave between components, producing
a self-intersecting polygon. **Topology must be decided before ordering.**

The corner-clustering approach (Finding 4) extends naturally: walk
fragments produce candidate component sets. For each component, build a
provisional cycle (geometric cost: compactness + spike-only angle
penalty, no WSD). Then classify each cycle:

- The same cycle scores well as **OUTER** when group-points-inside +
  nearby-outside; it scores well as **INNER** when reversed (group-
  outside + nearby-inside).
- WSD of the cycle under both orientation hypotheses tells us which.

Merge same-class components, run final TSP on the merged anchor set,
produce the polygon. For now we ship K=1 only (single outer perimeter,
no holes); holes become a separate emit later.

---

## The architecture that emerged

```
boundary_polygons_for_plane(plane):

    # Phase 1: Topology — corners-to-components via spatial filter
    fragments  = walk_balance_grid(plane.balance_field)
                 # unordered set of cells per fragment; FP-fragile
                 # ordering doesn't matter — we use it as a spatial
                 # filter only.
    components = [ corners within RANGE of fragment.cells for fragment in fragments ]
                 # each component is an UNORDERED corner set.
                 # corners may belong to multiple components.
                 # corners belonging to no component are dropped for now.

    # Phase 2: per-component candidate cycle, GEOMETRIC mode
    candidates = [ (comp, best_cycle(comp, mode=GEOMETRIC)) for comp in components ]

    # Phase 3: classify each candidate by orientation-WSD test
    outer_anchors = ∅
    for (comp, cycle) in candidates:
        wsd_outer = wsd(group, nearby, cycle, holes=∅)
        wsd_inner = wsd(group, nearby, reverse(cycle), holes=∅)
        if wsd_outer < wsd_inner:  outer_anchors ∪= comp
        elif wsd_inner < wsd_outer: inner_anchors ∪= comp  # collected; unused for now
        # AMBIGUOUS (equal) — dropped

    # Phase 4: merge outer fragments + recompute final cycle
    if |outer_anchors| < 3: return null
    return best_cycle(outer_anchors, group, nearby, holes=∅, mode=FUNCTIONAL)


best_cycle(anchors, group=∅, nearby=∅, holes=∅, mode):
    cost_fn = (mode == GEOMETRIC) ?
                cycle → 0.9 * compactness(cycle) + 0.1 * spike_penalty(cycle)
              : cycle → 0.8 * wsd(group, nearby, cycle, holes)
                      + 0.1 * compactness(cycle)
                      + 0.1 * spike_penalty(cycle)
    if has_self_intersection(cycle): cost_fn returns +∞

    if n ≤ FULL_ENUM_MAX:  return full_enum_best_cycle(anchors, cost_fn)
    else:                  return multistart_local_search(anchors, cost_fn,
                                  seeds=[NN(0), NN(n/2), centroid_angle,
                                         random(0..K_RANDOM-1)])


wsd(group, nearby, outer, holes):
    # Multi-polygon WSD. group should be inside outer AND outside every hole.
    # nearby should be outside outer OR inside some hole.
    # Existing implementations in v2 (lib/pipeline/polygon-wsd.js) and v3
    # (v7-boundary-polygons-impl.c::score_cycle) are single-polygon; the
    # rewrite generalises with the optional `holes` arg (empty for K=1).


spike_penalty(cycle):
    # Penalises only acute interior angles (folded-back spikes).
    # Does NOT penalise flat angles (near-180°) — those are legitimate
    # corners; if truly collinear, regularization removes them.
```

### What disappears from the old step

| Old | Why it goes |
|-----|-------------|
| D.4 bisector compute | Already cached upstream (`has_com_bisector_by_plane`) |
| D.5 candidate-edge graph | TSP doesn't need it (multi-start handles initial order) |
| D.6 leaf-prune | Required only for D.5's graph; gone with the graph |
| D.6b contour walk's arc-length parametrisation | Replaced by TSP local search; no arc-length needed |
| D.6b centroid-angle fallback | Folded into TSP as ONE seed of the multi-start, not a separate path |
| D.7 permute + delete | Replaced by 2-opt + or-opt (no deletion in this step) |
| D.7b nudge | Already disabled (deferred to regularize) |
| D.8 collinear strip | Folded into spike_penalty term; truly-collinear corners survive (regularize handles) |

### Validation criterion change

Old: byte parity with v2 on iso bench.
New: **stability under sub-millimetre input perturbation**. Run the
algorithm 100 times with Gaussian noise σ = 1 mm on anchor positions;
assert the output polygon's vertex set + cyclic order is the same in
≥ 99 / 100 runs. This is the test that distinguishes "correct algorithm"
from "consistent coin-flip".

---

## Decisions

**New step `v7-boundary-polygons-2`** (not modification of existing).
`v7` is the pipeline name and we don't rename it. Suffix `-2` makes the
discontinuity explicit:

- Architectural change is too significant for in-place edit (removing
  D.4–D.8 leaves almost nothing of the original).
- A/B testability: keep `v7-boundary-polygons` for comparison; pipeline
  switch chooses which writes the per-bldg roof-graph polygon section.
- Mirrors the v2 history (boundary-3d → boundary-polygons split).
- Bench harness reuse: new step gets its own iso harness for free.

**Dual implementation: v2 (JS) + v3 (C) in parallel.** v2 stays in active
development because it is still the reference for downstream steps
(`boundary-merge`, `boundary-classify`, `boundary-regularize`). v3 cannot
yet replace v2 end-to-end — the comparison harness needs both. So the
TSP-based step ships in both:

- `v2`: `lib/pipeline/steps/boundary-polygons-2.js`
- `v3`: `v3/steps/v7/v7-boundary-polygons-2.{c,h,impl.c,impl.h}` + bench

Both implementations target the same algorithm. **The stability criterion
(perturbation test) ensures they converge to the same polygon on the same
input** without needing FP byte parity — they're allowed to differ on FP
LSB as long as both pick the same global optimum.

**Drop the WSD-only-on-group-points constraint.** Actually verified
during this work: the existing WSD (`computeWSD` in v2, `score_cycle` in
v3) IS two-sided. It includes nearby-foreign points (`includeForeignIn`,
defaults to true except for walls). No work needed here — earlier notes
suggesting otherwise were wrong. The cost function in the rewrite uses
the same two-sided WSD signature.

**Synthetic unit tests as the new oracle.** dahouet has no ground truth.
We define synthetic test cases with hand-known correct outputs:

- T1: 10 × 10 square plane, 4 corners, 100 group + 20 nearby points →
  expected 4-vertex CCW polygon, WSD = 0.
- T2: T1 with σ = 1 mm Gaussian noise on corners → expected same
  vertex set in same cyclic order in ≥ 99 / 100 runs.
- T3: T1 + 1 extra near-collinear corner → expected 5-vertex polygon
  (deletion deferred to regularize).
- T4: T1 + 2 × 2 interior cutout → expected 2 components (1 OUTER + 1
  INNER), 4-vertex outer polygon emitted now, inner cycle classified
  for future K > 1 work.
- T5: T1 with a realistic signal hole (≥ 1 m — the size of an actual
  missing surface region, not a sub-pixel gap that the walk bridges
  for free) → walk produces two fragments instead of one → expected
  merged 4-vertex outer polygon.
- T6: T1 randomly rotated + translated in 3D, σ = 5 mm corners, σ = 5 cm
  group/nearby → ≥ 99 % of 1000 runs produce WSD < 0.1 m.

These give us the missing oracle. dahouet remains useful as a real-data
regression test, but synthetic cases are how we KNOW the algorithm is
correct.

---

## What stays from this session

The five FP fixes (Finding 3) all land in `main`. They are real
improvements regardless of the rewrite — they make v3 more correct on
its own terms, simplify reasoning about downstream behaviour, and serve
as documented breadcrumbs of the failure modes.

`v3/docs/plan/THRESHOLD-INVESTIGATION-DAHOUET.md` §10–§13 records each
fix with concrete trace data; this document references that one for the
forensic detail.

The CORNERS / CORNER_MEMBERS section unification (`201540e`, `ae673bb`,
`e14a729`) is an unambiguous wire-format cleanup that survives the
rewrite — the inline `members[]` block is consumed by the new step
exactly as by the old.

---

## Open questions (forward work)

- **Component-merging when classification is ambiguous.** A component
  whose `wsd_outer ≈ wsd_inner` (within ε) sits at the boundary — could
  be a degenerate cycle, could be a real ambiguous geometry. Current
  plan: drop it. Future: revisit when we have data on how often it
  happens.
- **Full-enum n cutoff.** At n=8, 8!/2 = 20160 cycles per component — fast.
  At n=10, 10!/2 = 1.8 M cycles — still ms-range. At n=12, 12!/2 = 240 M
  cycles — too slow per plane. Current cap `V7_BP2_FULL_ENUM_MAX = 8`;
  measure on dahouet to see if it can be raised. (Held-Karp DP doesn't
  apply here — see footnote in "the architecture that emerged" section
  for why.)
- **K_RANDOM seeds** for multi-start: 5? 10? More seeds = more robust
  but slower. Empirical.
- **Multi-cycle emit** (real inner holes). Deferred. Need to decide the
  output schema: extend `polygonPaths` to carry holes, or emit a separate
  `polygonHoles` artifact.
- **WSD weight tuning.** 0.8/0.1/0.1 is a starting guess. The synthetic
  tests will surface cases where the weights are off.

---

## Addendum (later same day): classification abandoned

After implementing per-component classify, two problems surfaced:

1. **It's unreliable on arc-shaped fragments.** A walk that produces an
   open arc of the outer ring closes (under best_cycle GEOMETRIC) into
   a thin sliver covering roughly half the group region. The orientation-
   WSD score gives near-equal numbers in both hypotheses — sometimes
   exactly INNER by FP noise — so an arc that's legitimately part of
   the outer perimeter gets dropped. Concretely: a triangle that
   covers half of a square's group cloud has wsd_outer ≈ 470,
   wsd_inner ≈ 240, classifies INNER. There is no published algorithm
   that classifies a single component in isolation in our setup — KSR,
   PolyFit, Geoflow all do JOINT discrete-label optimisation. We were
   solo-inventing.

2. **The AMBIGUOUS-as-OUTER default corrupts.** Anchors of a real inner
   ring that happens to score near-equal both ways under classify get
   merged into the outer union. The resulting polygon is geometric
   garbage (or self-intersects, depending on the geometry).

**Fix: drop classify entirely. Enumerate the 2^K subsets of components,
run best_cycle on each merged subset, pick the lowest-cost cycle.** The
cost function (with self-intersect → +∞) already has all the
information needed to vote: a subset that excludes the real outer has
huge WSD; a subset that includes a hole's anchors weaves and self-
intersects; the right subset wins by direct cost comparison. This is
the same shape as KSR's face-labelling MIP, just with exhaustive
enumeration because K stays small in practice (≤ 10ish).

Outcome — new fixtures landed alongside the pivot:

- `t5-signal-fragmentation`: overlapping arc components → cost picks
  their union = full square. This was the case classify failed on.
- `t7-spurious-noise-component`: real outer + a noise component far
  away → cost rejects {noise} (huge WSD) and {outer ∪ noise}
  (degenerate cycle); picks {outer}.
- `t8-outer-with-two-holes`: K=3, 7 subsets evaluated, {outer} wins.

The CLASSIFICATION enum, classify() function, tie-epsilon, and the
unit-classify fixtures were all deleted. Net code reduction. Doesn't
change the public algorithm API except for the absence of these
internal helpers.

What this leaves open:

- **K cost-cap**. Hard ceiling at K=12 (4096 subsets). Production roof
  data should rarely exceed K=4-5; if dahouet exposes outliers we'll
  revisit.
- **Holes (K>1 emit)**. The enum stays 2^K and picks one outer subset;
  the unselected components stay unused. When we emit holes later we'll
  upgrade to 3^K (outer / hole / drop) — same machinery, different
  selection.

---

## Refs

- v3 commits this session: `201540e`, `ae673bb`, `e14a729`, `115f158`,
  `556c892`, `cffc454`, `aa70e9a`, `d601a02`
- `v3/docs/plan/THRESHOLD-INVESTIGATION-DAHOUET.md` §10–§13 — forensic
  traces of the five FP fixes.
- `v3/docs/plan/PHASE-5-V7-PORT.md` — phase ledger; this rewrite will get
  its own row.
- This journal entry's predecessors:
  - `2026-05-17-wrong-side-corner-killing-review` — same review-of-port
    pattern applied to boundary-corners D.2b.
  - `2026-05-10-regularization-cost-architecture` — adjacent algorithm,
    same WSD primitive.
  - `2026-04-27-regularization-exploration` — pushed corner nudge OUT of
    boundary-polygons; this entry pushes deletion out too, completing
    the symmetric move.
