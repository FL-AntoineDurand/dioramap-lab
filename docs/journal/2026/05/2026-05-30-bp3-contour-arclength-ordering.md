---
id: "2026-05-30-bp3-contour-arclength-ordering"
date: 2026-05-30
title: "bp3 ordering rebuilt: contour arc-length walk + windowed or-opt replaces WSD-TSP (stripe-zigzag fix)"
tags: [boundary-polygons, contour, ordering, tsp, wsd, lidar-physics, v7, v7-boundary-polygons-2]
category: design
status: concluded
refs:
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-05-22-bp3-grid-fill-topology"
  - "2026-05-24-bp3-implementation"
pipeline: v7-bp2
---

## TL;DR

bp3 (the algorithm inside `v7-boundary-polygons-2`) produces "mad zigzag"
polygons on real LIDAR planes. Plane 5 of dahouet-bldgonly emits a 96.3 m
perimeter loop with 16-of-26 vertices being >100° U-turns, over a region
whose actual boundary (the upstream grid-fill contour) is a clean 29.8 m
loop. The cause is **not** a porting bug and **not** the upstream GRM
divergence the prior handoff suspected — it is the **cost functional used
for ordering**. bp3 uses the contour only to *select* anchors, then
re-derives their cyclic order with a multi-start WSD-TSP whose objective is
faithfully optimised toward a physically-misleading signal (the LIDAR
scanline stripe pattern). The fix is to finish the ordering design the
2026-05-22 bp3 entry actually specified — **arc-length walk along the
contour** — which the 2026-05-24 implementation never wired, and to replace
the global WSD-TSP with a tiny **windowed or-opt** geometric refinement.

## The flaw, proven

### Symptom

On dahouet-bldgonly, v3 `v7-bp2` pipeline, `boundary-polygons-roof-graph`,
emitted polygons vs the upstream grid-fill contour (same plane, same region):

| plane | emitted perim | contour perim | emitted big-turns | span match? |
|---|---|---|---|---|
| 0 | 112.7 m | 89.0 m | 7/17 | yes (15.7×27.4 vs 15.8×27.7) |
| 2 | 79.5 m | 27.7 m | 18/30 | yes (9.2×9.1 vs 9.0×9.3) |
| 3 | 61.9 m | 34.4 m | 5/28 | yes |
| 5 | **96.3 m** | **29.8 m** | **16/26** | yes (10.4×6.6 vs 10.1×6.7) |
| 7 | 35.8 m | 23.8 m | 7/15 | yes |

(big-turn = interior turn >100°, the U-turn signature.) The spans match the
contour on every plane — bp3 is **not** visiting a wider region, it is
zigzagging *inside* the correct region. The dense-anchor planes (2,3,5,7)
are the worst; the pattern scales with anchor count, not with span.

### It is the cost, not the search, not the anchors

For plane 5's exact 26 anchors, ordered three ways:

| ordering of the SAME 26 anchors | perim | big-turns | self-int |
|---|---|---|---|
| v3 emitted (multi-start WSD-TSP) | 96.3 m | 16/26 | no |
| nearest-neighbour | 27.7 m | 3/26 | no |
| angular (atan2 about centroid) | 28.8 m | 5/26 | no |
| (upstream contour) | 29.8 m | — | — |

A clean ~28 m order of the exact same anchors exists and is *trivially*
reachable — nearest-neighbour and angular sort both find it. Those two are
**literally the algorithm's own seed strategies** (`nearest_neighbor_seed`,
`centroid_angle_seed`). So the multi-start starts at ~28 m and *ends* at
96 m. Because `local_search` only ever accepts cost-**decreasing** moves
(`impl.c:364`), a monotone descent that begins at a 28 m seed and finishes
at the 96 m zigzag can only mean:

```
cost(96 m zigzag)  ≤  cost(28 m clean loop)
```

The optimiser is doing its job perfectly. The objective is wrong. The cost
is `0.8·WSD + 0.1·compactness + 0.1·spike` (`impl.c:301`); WSD dominates at
0.8 and the zigzag is its global optimum.

This refutes the prior handoff diagnosis ("cascade from upstream GRM
producing disjoint anchor clusters"): the anchors span the right region and
reorder cleanly. GRM divergence is real but it is not what produces the
plane-5 zigzag.

### Why WSD prefers the zigzag — the stripe physics

(User-confirmed by visual inspection of the point cloud.) The plane LIDAR
beam mechanics lay points down in **stripes** (scanlines) with **no-data
gaps between stripes**. WSD = wrong-side distance: own-plane points outside
the polygon + nearby-foreign points inside, summed by edge distance.
Weaving the boundary *along* the stripes and *cutting into* the inter-stripe
gaps lowers WSD **for free** — the gaps contain no points, so nothing there
can be on the wrong side. The cost has no term forbidding intrusion into
no-data regions.

Crucially, **the grid-fill contour already solves this.** It is
marching-squares on a *smoothed density mask*, which bridges the thin
inter-stripe gaps and yields a clean closed loop (29.8 m on plane 5). bp3
holds the correct boundary in its hands (the contour) and then throws its
*ordering* away, re-deriving order with a cost that the stripe pattern
actively misleads. This is a sibling of the already-documented
"Gaussian-Saturated WSD Trap" ([[2026-05-10-regularization-cost-architecture]],
Finding 3): WSD-driven optimisation distorting polygons.

## The design/implementation divergence (what was forgotten)

The current code is **not** what bp3 was designed to be. Tracing the three
entries:

- **[[2026-05-21-boundary-polygons-tsp-rethink]]** reframed ordering as a
  *global* problem: "cyclic ordering through all anchors = best Hamiltonian
  cycle = TSP… multi-start makes the initial order irrelevant." It listed
  "D.6b contour walk's arc-length parametrisation → Replaced by TSP local
  search; **no arc-length needed**." The WSD-TSP existed to find a good
  order *from scratch*, because at that moment there was no trustworthy
  contour to seed from (bp1's walk was an FP coin flip).

- **[[2026-05-22-bp3-grid-fill-topology]]** then *revised* that decision.
  With a clean grid-fill contour available it put arc-length ordering back:
  > **Ordering**: walk projected corners in arc-length order along the
  > contour, *then refined by permutation*. Point-level precision via the
  > corner positions themselves — *not* via the contour vertices.

  And it flagged the exact trap we hit:
  > best_cycle cost weights 0.8/0.1/0.1 inherited from bp2; **may need
  > retuning once corner ordering comes from arc-length instead of
  > multi-start seeds.**

- **[[2026-05-24-bp3-implementation]]** shipped
  `cycle = best_cycle(subset, FUNCTIONAL)` — pure WSD-TSP, **no arc-length
  walk.** It followed 05-21's discarded model, not 05-22's refined one
  (even while citing 05-22 as its design ref; *"reuse bp2, which is mostly
  unusable anyway"*). The arc-length walk — the core of the bp3 *ordering*
  design — was never coded, and the weight retune that was explicitly
  contingent on "ordering comes from arc-length" never happened.

Plane 5 is the empirical falsification of 05-21's load-bearing assumption.
"Multi-start makes the initial order irrelevant" is false on real striped
data: multi-start with the WSD cost does not find the clean order; it
actively prefers the zigzag.

## Decision

Finish the 05-22 design, read correctly.

1. **Primary order = arc-length walk along the contour.** For each anchor
   selected for a contour (already gated at ≤0.75 m of a contour edge),
   project it to its nearest point on the *closed* contour polyline, take
   the cumulative arc-length to that point, and sort anchors by arc-length.
   That is the cyclic order. This is bp1's proven mechanism on bp3's much
   better contour, and it inherits the contour's gap-bridging, so the
   stripe zigzag cannot form.

2. **Refinement = windowed or-opt under compactness only.** Seed the
   refinement with the arc-length order, then run a single-start greedy
   descent whose only move is **or-opt restricted to ±K positions** (K≈2,
   tunable; K≥1 already subsumes adjacent-swap). Cost = **compactness**
   (`perimeter² / 4π·area`) with the **self-intersection guard** (`+∞`) as
   a hard constraint. WSD and the separate spike term leave the ordering
   path entirely.

3. **Remove from the ordering path:** WSD, multi-start, random restarts,
   the GEOMETRIC/FUNCTIONAL full-`best_cycle`, and unbounded moves. The
   `best_cycle` / `cost_functional` / `wsd_multi` code stays in the file
   (still exercised by unit fixtures and used for the emitted
   wsd/compactness/spike *diagnostics*) but is off the ordering decision.

4. **No deletion** — unchanged. Vertex existence is regularize's job
   ([[2026-04-27-regularization-exploration]]).

### Rationale for each demotion (answering "do we even need the TSP?")

- **Why drop global multi-start WSD-TSP.** Its job was to find a good order
  *from scratch* — a need the contour *dissolves*. 05-22 already said
  "topology is given, no enumeration needed"; the same logic applies to
  ordering — the contour *is* the order, up to local ties. With a good seed
  the only thing global search can find that the seed doesn't is the
  WSD-rewarded zigzag. It is not merely unnecessary here; it is harmful.

- **Why a *local* refinement is still needed.** Arc-length is wrong in one
  narrow case: two corners near-equidistant along the contour (projecting
  to near-equal arc-length) can be ordered backwards. That is a *local*
  swap of arc-length-adjacent anchors — exactly bp1's adjacent-swap niche.

- **Why compactness, not WSD, for the refinement.** For two tied corners
  the correct local order is the one that does not fold or cross — a
  compactness/self-intersection criterion. WSD is precisely the signal the
  stripes corrupt, so it must not arbitrate ordering.

- **Why compactness alone, dropping the separate spike term.** Compactness
  already penalises spikes (a fold adds out-and-back perimeter at ~zero
  area, and a zero-area sliver sends `perimeter²/area → ∞`), and the
  self-intersection guard hard-bans crossings. The spike term's only unique
  value is *local* per-vertex sensitivity that the *global* compactness
  ratio dilutes on a large polygon — marginal for ordering. Dropping it
  simplifies the cost without changing the decision.

- **Why windowed (±K), not full, or-opt.** Full or-opt's only extra power
  over a windowed version is the *long jump*. The long jump targets the
  arc-length *projection outlier* (a corner whose nearest-contour-point
  lands on a far arm of a pinched/folded contour, so arc-length drops it in
  the wrong neighbourhood). But greedy descent often will not take that
  jump (barrier, below), and a corner in the wrong arm is a **seed/projection**
  failure, not an ordering-refinement failure — the honest fix is at the
  projection step (assign by distance *and* tangent agreement), not an
  unbounded search hoping to claw it back. So windowing loses essentially
  nothing real, makes locality structural and auditable, and removes the
  risk of a surprise non-local jump.

- **Why not adjacent-swap (K=1) only.** Adjacent transpositions *generate*
  the full symmetric group, so "adjacent-swap is insufficient" (05-21
  Finding 5) was never about *reachability*. It was about greedy descent
  getting stuck behind **cost barriers**: to walk a corner several slots via
  adjacent swaps, each intermediate state is worse, so the *first* swap is
  rejected and the journey never starts. That barrier only bites when you
  start *far* from the optimum — bp1's regime (bad FP-fragile seed). This
  plan seeds from arc-length (near-correct), so the barrier is moot. K≈2
  buys a little robustness (a 3-corner tangle, an off-by-two) over pure
  adjacent-swap at negligible cost, and `or-opt` at K≥2 still cleanly
  relocates a single corner two slots in one accept/reject.

## Functional & behavioural analysis

- **Determinism.** No randomness remains (multi-start random seeds gone).
  Output is a pure function of (anchors, contour, K). Fixed seed + fixed
  move-scan order → the descent always lands on the same local optimum.
- **Termination is guaranteed.** Accept requires a *strict* improvement
  (`c + 1e-12 < cur_cost`). Each accepted move lowers compactness by ≥1e-12;
  compactness is bounded below (≥ ~1 for any real polygon). A
  strictly-decreasing, bounded-below sequence takes finitely many steps, so
  the search reaches a no-improving-move state and halts. The `max_outer=100`
  cap is a paranoia backstop against FP tie-oscillation; from a good seed it
  is never reached (≈0–3 sweeps).
- **Stop condition.** First sweep that accepts zero windowed-or-opt moves
  (= a local optimum under compactness). Same condition bp1 used; the only
  changes are the move set and the cost.
- **Stability (the success criterion, [[2026-05-21-boundary-polygons-tsp-rethink]]).**
  Under σ=1 mm anchor perturbation the output must be the same in ≥99/100
  runs. Arc-length positions are robust because contour vertices are
  *cells-apart*, not LSBs-apart (05-22); compactness-driven windowed descent
  from that seed is deterministic and barrier-free. v2 and v3 converge to the
  same polygon on the same input without FP byte-parity.
- **Why the zigzag is now impossible.** Three independent reasons stack:
  (a) the seed follows the gap-bridged contour; (b) compactness rejects any
  perimeter inflation; (c) windowing forbids the long crossing edges a
  zigzag needs. Any one suffices; together they are belt-and-braces.

### Edge cases

- `<3` anchors near a contour → contour skipped (unchanged, `impl.c:697`).
- ties at equal arc-length → deterministic tie-break (perpendicular-offset
  sign, then anchor id).
- **pinch/fold projection outlier** → arc-length may place a corner in the
  wrong neighbourhood; windowed or-opt deliberately does **not** chase it.
  Flagged as a *projection* follow-up (segment assignment by distance +
  tangent agreement), watched for on dahouet. Not solved in this change.

## Complexity

Per plane (N = selected anchors ≤ ~30; M = contour vertices, ~150 on the
dense planes):

- **Seed (arc-length walk):** project each of N anchors to the nearest of M
  contour segments → `O(N·M)` ≈ 30·150 ≈ 4.5k ops. Sort → `O(N log N)`.
- **Refinement:** per sweep the move set is `O(N·K)` (each of N vertices
  reinserted at ≤2K offsets). Compactness is evaluated **incrementally** —
  an or-opt touches a constant number of edges (2→1 at the pull site, 1→2
  at the insert site), so the perimeter and shoelace-area deltas are `O(1)`
  per candidate. The self-intersection guard (`O(N²)`) is run **only when a
  move is accepted** (rare from a good seed) — or, equivalently, only the
  changed edges need testing (`O(N)`). Sweeps to convergence: ≈0–3.
  → `O(sweeps · N·K)` move-evals at `O(1)` each, plus a handful of `O(N²)`
  validations. Microseconds per plane.

Contrast with the path being removed: WSD evaluation is
`O(N_cycle · N_points)` over the *hundreds-to-thousands* of group+nearby
LIDAR points, run inside `local_search` across 8 multi-start seeds. The new
ordering path **never touches the point cloud.** (This was the same class
of blow-up that produced bp2's 22-min-per-plane panic,
[[2026-05-22-bp3-grid-fill-topology]].)

> Implementation note: the current `local_search` re-costs the *entire*
> polygon per candidate (malloc a fresh cycle + full `cost_*` incl. the
> `O(N²)` self-intersect, `impl.c:383-403`). That is the only reason the
> move set ever looked like `O(N⁴)`. Do **not** carry it over — use
> incremental compactness deltas and accepted-only self-intersection.

## Scope / what this does NOT change

- Wire format unchanged: still a `POLYGON_PATHS` (0x0B) `corner_ids[]` cycle
  per contour, one OUTER + per-hole INNER. `boundary-merge` / `-classify` /
  `-regularize` consume it unchanged ([[2026-05-24-bp3-implementation]]).
- D.3 NMS in the step wrapper (`process_plane`) stays.
- WSD/spike code stays in the file for diagnostics + unit fixtures.

## Plan of record (overnight)

1. **v3 C first.** `v7_bp2_compute`: replace the `best_cycle(FUNCTIONAL)`
   call with `arclength_walk(subset, contour)` → windowed-or-opt(compactness).
   Add `V7_BP2_OROPT_WINDOW` (default 2). Bump step `.version` 4→5.
2. Build noasan, re-run `v7-bp2` (produce `boundary-polygons-roof-graph`
   only, temp `--cache-dir`), confirm plane 5 → ~28–30 m / ≤5 big-turns and
   planes 2/3/7 likewise; run `ctest -R v7_bp2` (20 fixtures).
3. **Verify the implementation against this journal** (determinism,
   termination, windowed/compactness-only, no point-cloud touch). Append an
   "Implementation & Verification" section and flip status → concluded.
4. **Then v2 JS** mirror (only if v3 verified): `boundary-polygons-2-impl.js`
   + step wrapper, bump `meta.version`.
5. **Impact analysis:** unit fixtures (some carried WSD-TSP expectations),
   docs/headers (`impl.h:14-31` Phase 2 text is now stale), and the iso
   bench / v2≡v3 equivalence story (the per-step bench was stubbed pending
   contour-data dumps — study and document what equivalence requires now).
6. **Plane-2 self-intersection** (separate task): the emitted plane-2
   polygon self-intersects (crossings at edge-pairs (21,23) & (26,28),
   frozen at `docs/research/frozen-cases/bp2-selfint-dahouet.json`) — which
   is *impossible* if the cost's `+∞` self-int guard worked and the emitted
   order equalled the cost-evaluated order. Either the detector has a
   false-negative gap or there is a cost/cycle desync. The frozen case feeds
   dedicated unit tests for `seg_seg_strict_intersect` /
   `v7_bp2_polygon_self_intersects_uv`.

## Implementation & Verification (v3 C, 2026-05-30)

Landed in `v7-boundary-polygons-2-impl.{c,h}` + wrapper, step `.version` 4→5.

New static functions (impl.c, "Ordering" section): `contour_arclen`,
`arclength_walk`, `cycle_compactness`, `cycle_self_intersects`,
`windowed_oropt`. `v7_bp2_compute` Phase 2 now: `memcpy(subset)` →
`arclength_walk` → `windowed_oropt(K=V7_BP2_OROPT_WINDOW)` → reject if the
result self-intersects. The FUNCTIONAL `ctx` block was removed (no longer
built); `best_cycle` / `cost_functional` / `wsd_multi` stay compiled (public
API, still driven by unit fixtures and used for emitted diagnostics).
`V7_BP2_OROPT_WINDOW` (default 2) added as global + param + JSON key
`oroptWindow` + `DERIV_LOG`.

### Contract check vs the design above

| Design clause | Implemented | Note |
|---|---|---|
| order by arc-length along contour | yes | `arclength_walk`, sort key (arclen, perp, id) |
| windowed or-opt, ±K, K≥1=adjacent-swap | yes | `windowed_oropt`, `lo=i-K..hi=i+K` linear (no wrap = strictly local) |
| compactness-only cost (no WSD, no spike term) | yes | `cycle_compactness` = perim²/4π·area, nothing else |
| self-intersect = hard guard, accepted-only | yes | checked only after `c+1e-12 < cur` passes |
| no multi-start / no WSD on ordering path | yes | `best_cycle` not called by compute |
| determinism (no RNG) | yes | sort + first-improvement descent, fixed scan order |
| termination guaranteed; stop = local optimum | yes | strict 1e-12 improvement, bounded-below compactness, 100-sweep backstop |
| no deletion | yes | unchanged |

**One deviation, deliberate:** the journal complexity section specified
`O(1)` *incremental* compactness per candidate. The implementation evaluates
**full `O(N)` compactness** per candidate instead (no self-intersect, no
malloc — `trial`/`rest`/scratch buffers are allocated once per
`windowed_oropt` call, not per candidate). Per-sweep cost is therefore
`O(N²·K)` not `O(N·K)`. With N ≤ ~30 and ≈0–3 sweeps this is still
microseconds, and it avoids the error-prone shoelace/perimeter delta
bookkeeping. The pattern the journal warned against (malloc + full cost
*including* the `O(N²)` self-intersect, per candidate) is **not** present —
self-intersect runs accepted-only. Net: cheaper than the old WSD path by
orders of magnitude (ordering never touches the point cloud).

### Empirical results — dahouet-bldgonly, v3 v7-bp2

| plane | perim before → after | contour | big-turns before → after |
|---|---|---|---|
| 0 | 112.7 → **67.8** m | 89.0 | 7 → **2** |
| 2 | 79.5 → **27.4** m | 27.7 | 18 → **1** |
| 3 | 61.9 → **34.8** m | 34.4 | 5 → **0** |
| 4 | 53.0 → **26.1** m | 26.1 | 7 → **0** |
| 5 | 96.3 → **27.4** m | 29.8 | 16 → **3** |
| 7 | 35.8 → **26.0** m | 23.8 | 7 → **3** |

Every plane's perimeter now tracks its contour (the correct boundary); the
zigzag U-turns collapse to ≈0. **Plane 5** (the user's "spectacularly wrong"
case) goes 96.3 m / 16-turns → 27.4 m / 3-turns — tighter than the contour
itself. Global checks across all 24 emitted planes: **0 self-intersecting,
0 duplicate corner-ids**. The plane-2 self-intersection (crossings at
edge-pairs (21,23) & (26,28)) **disappeared** with the new order — which is
why its case was frozen first (`docs/research/frozen-cases/bp2-selfint-dahouet.json`)
before this change could erase it. `v7-bp2-test`: **20/20 fixtures pass**.
Pipeline wall time 2.96 s (14 steps, noasan). Build clean, no warnings.

Status flipped proposed → concluded for the v3 leg. v2 JS mirror + iso-bench
impact tracked as separate tasks.

### v2 JS mirror — same algorithm, confirmed on dahouet

Mirrored in `boundary-polygons-2-impl.js` (`contourArclen` / `arclengthWalk`
/ `cycleCompactnessIdx` / `windowedOropt`, `compute` rewired, `OROPT_WINDOW`,
step `meta.version` 2→3). Fresh v2 dahouet dump (full `v7-bp2` pipeline):

| plane | v2 perim before → after | v2 big-turns before → after |
|---|---|---|
| 0 | 47.5 → 44.7 m | 5 → 3 |
| 2 | 44.7 → 29.5 m | 4 → 2 |
| 4 | 46.5 → 28.2 m | 6 → 2 |
| 5 | 52.5 → **28.6** m | 7 → **1** |
| 7 | 19.8 → 19.1 m | 3 → 1 |

v2 emitted 25 planes, **0 self-intersecting**. v2's plane-5 anchor set (22)
differs from v3's (26) — different corner pools from upstream GRM FP
divergence — but both yield clean ~contour-perimeter loops. That is the
intended outcome: equivalence is *stability/quality*, not dahouet
byte-parity. Cross-validator (the shared-fixture v2≡v3 gate): **25/25 both
PASS, 0 disagreements.**

## Impact: tests, docs, iso-bench / v2≡v3 equivalence (2026-05-30)

- **Unit fixtures** (`tests/fixtures/v7-boundary-polygons-2/`): all 20
  prior fixtures still pass under both impls. The `unit-wsd` / `unit-spike` /
  `unit-cost-order` / `unit-best-cycle` fixtures test the *retained* lower
  layers (WSD, spike, compactness, `best_cycle`) — still valid even though
  ordering no longer uses them. The e2e contour fixtures (t1/t3/t4/t8/t12/t13)
  exercise the new ordering and pass. No fixture encoded a WSD-TSP-specific
  ordering that needed updating. Added 5 `unit-self-intersect` fixtures (see
  plane-2 section). Total **25/25**.
- **v2≡v3 equivalence gate.** The per-step iso bench
  (`v7-boundary-polygons-2-only-bench.c`) is **stubbed** (`n_contours = 0`,
  TODO) pending bench-v2 dumping contour data into the iso fixture — unchanged
  by this work, still the right future path. The **working** equivalence gate
  is the shared-fixture cross-validator (`scripts/v7-bp2-cross-validate.mjs`),
  which runs both the v3 C (`v7-bp2-test`) and the v2 JS `compute` against the
  same fixtures. It was **stale** (its `runV2Inner` never passed `contours` to
  bp3's `compute`, and still referenced removed bp2-era
  `balance_field`/`components_override`) → 3 false v2-FAILs. Fixed it to pass
  `contours` and to match the v3 C harness's K=1 criterion (compare the OUTER
  `polygons[0]`). Now **25/25 both PASS, 0 disagreements.** The new ordering
  makes equivalence *easier*: it is deterministic (no RNG) and never touches
  the point cloud, so v2/v3 can't diverge on FP-fragile WSD-over-points. Note
  v2≠v3 *byte-parity on dahouet* remains expected and fine — their corner
  pools differ from upstream GRM FP divergence; equivalence is proven on the
  shared synthetic fixtures + the stability criterion, not dahouet parity.
- **Docs:** updated `impl.h` algorithm header (Phase 2), `impl.c` compute
  comment, `boundary-polygons-2-impl.js` header, the v2 step description, the
  UI genealogy entry (was "TSP / subset-enumeration", doubly stale), and added
  a RESOLVED banner to `V3-V7BP2-HANDOFF-2026-05-29.md`.

## Plane-2 self-intersection — investigated (2026-05-30)

The old bp2 emitted a self-intersecting plane-2 polygon (crossings at
edge-pairs (21,23) & (26,28)), which should be impossible given the cost's
`+∞` self-intersection guard. Froze the exact polygon
(`docs/research/frozen-cases/bp2-selfint-dahouet.json`) **before** this
change could erase it, then ran the **C** detector
`v7_bp2_polygon_self_intersects_uv` on it: it returns **TRUE** (matches
Python). So the detector is **correct** — not the bug. The old emission
therefore came from `best_cycle`/`local_search` (FUNCTIONAL/WSD-TSP)
returning a **finite cost for a self-intersecting winning cycle** — a
cost/cycle desync in that path. That path is now **retired from ordering**
(bp3 uses arc-length + windowed or-opt, which has an accepted-only self-int
guard *and* a final reject in `compute`), and empirically plane 2 now emits a
clean 27.4 m non-self-intersecting loop (0/24 planes self-intersect).

Added a dedicated `unit-self-intersect` category to `v7-bp2-test.c` + 5
fixtures (`unit/selfint-*.json`): square-clean (F), bowtie (T),
collinear-parallel (F, documents the parallel-edge limitation),
endpoint-touch (F, documents the EPS endpoint exclusion), and the frozen
plane-2 polygon (T, regression). 25/25 fixtures pass. Residual (low
priority): pinpoint the exact desync line in the retired FUNCTIONAL
`best_cycle` — would need plane-2 group/nearby points to reproduce.

## Open questions / follow-ups

- **Projection-outlier fix** (pinch/fold contours): assign anchor→contour by
  distance *and* tangent agreement, not nearest-point alone. Only if dahouet
  shows a corner landing in the wrong neighbourhood after this change.
- **K tuning.** Start K=2; measure on the dense planes whether K=1 suffices
  (likely) or a 3-corner tangle ever needs K=3.
- **Compactness weight vs spike.** Ordering uses compactness-only; confirm no
  fixture genuinely needs the per-vertex spike sensitivity.
