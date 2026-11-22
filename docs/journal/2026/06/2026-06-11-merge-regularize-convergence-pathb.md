---
id: "2026-06-11-merge-regularize-convergence-pathb"
date: 2026-06-11
title: "Folding boundary-merge into the boundary-regularize loop (Path B) — a convergent building model"
tags: [boundary-merge, boundary-regularize, boundary-classify, boundary-polygons-2, architecture, design, convergence, v7]
category: design
status: superseded
refs:
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-04-27-regularization-exploration"
  - "2026-05-10-regularization-cost-architecture"
  - "2026-05-27-boundary-regularize-iso-baseline"
  - "2026-05-28-boundary-regularize-fp-equivalence"
  - "2026-06-11-d2b-side-vec-wrong-plane-frame"
pipeline: v7-bp2
design_doc: ""
---

> **Living document.** Started 2026-06-11 as the design record for an ambitious
> restructuring. Sections below are filled along the way: history + symptom
> first, then a measured baseline, then Path-B feasibility, then (later)
> implementation + results.

## Symptom

`boundary-merge` is supposed to produce **one clean merged edge along each
shared interior ridge** between two adjacent roof planes. On rough plane
geometry it fails: the two planes' polygon edges along the ridge are too
jagged to match, so the merge silently produces no shared edge there. The
building model ends up with duplicated, unmerged ridge boundaries instead of a
single coherent ridge.

## The pipeline today

```
… grow-refit-merge → boundary-topology → boundary-signals → boundary-corners
   → boundary-polygons-2 → boundary-merge → boundary-classify → boundary-regularize
   → wall-detect → score
```

- **boundary-polygons-2 (bp3)** — emits one **raw** polygon per plane contour;
  vertices are the corner-pool anchors, un-simplified, not axis-aligned,
  near-collinear vertices preserved on purpose.
- **boundary-merge** — finds polygon edges from two planes that are the *same*
  shared ridge and rewrites both to reference one edge.
- **boundary-classify** — pure **labeling** (perimeter / shared /
  interior-shared / wall / overhang / level-transition); moves no geometry;
  emits per-edge tags + `NEW_ADJACENCIES` that **regularize consumes**.
- **boundary-regularize** — the **global, iterative coordinate-descent** loop
  (R.0 DOF-tag → R.0b cleanup → R.1 greedy nudge, with R.2 proximity-merge +
  R.3 collinear-removal *chasing inside* the loop). The step that all the
  deferred simplification was meant to land in.

## History — what was removed, and why

`boundary-polygons` (bp1, ported from v2 `f111696`) carried in-step geometry
cleanup:

- **D.6b** contour walk with **Douglas–Peucker** simplify (eps = 1.5×cell) +
  moving-average smoothing + snap-close;
- **D.7** permute + **vertex deletion** (WSD/compactness Pareto gates);
- **D.8** **collinear-vertex** removal (cos > 25°);
- **D.7b** ±15 cm bisector **corner nudge** (already disabled earlier).

The **2026-05-21 TSP rethink** rewrote the step as `boundary-polygons-2`
(bp2→bp3) and **dropped all of it, deliberately**. Verbatim rationale
([[2026-05-21-boundary-polygons-tsp-rethink]]):

> "Separate deletion from boundary-polygons entirely… Deletion is a
> regularization concern: regularize sees ridge-snapping, shared-edge merging,
> and multi-plane coherence — none of which boundary-polygons can evaluate."

This mirrored the earlier D.7b decision ([[2026-04-27-regularization-exploration]]):
per-plane refinement "can't reach" shared corners → defer to a **global,
cross-plane optimizer**. The intent was sound: vertex existence is a global
decision.

## The core insight — an ordering inversion

The deferral was right in intent but created a **gap in ordering**:
simplification was pushed to the **last** step (regularize), yet **merge —
which structurally requires clean ridge chains — runs three steps earlier, on
the roughest version of the geometry.** Under bp1, D.7/D.8 straightened ridges
*before* merge saw them; now nothing does. merge was silently downgraded by an
upstream decision that never accounted for merge's dependence on simplified
input.

### Why merge is all-or-nothing on roughness (mechanism)

merge marks a polygon edge "on-ridge" only if **both endpoints ≤ 0.30 m from
the ridge line AND the edge is within 7.5° of it**, then requires
`find_single_run` to find **exactly one contiguous on-ridge run** per plane and
intersects the two runs. `find_single_run` returns nothing if `n_runs != 1`.
So a single jagged vertex (>0.30 m off, or a segment >7.5° askew) **fragments
the run into two → the entire ridge on that plane is silently skipped.** No
partial merge. The per-edge tolerances were sized for *"5–20 cm corner-snap
residual"*, not real jaggedness.

## Connection to the side_vec fix (2026-06-11)

The historical dahouet under-count (7 shared edges vs v2's 11/14, commit
`d0a634a`) was traced to **wrong-side convex corners ~0.5 m off the ridge** —
exactly the jaggedness that trips merge's 0.30 m gate. That bug class was just
fixed in `70aeb15` ([[2026-06-11-d2b-side-vec-wrong-plane-frame]]), so merge's
input is already less rough than when this was last measured. **Re-baselining
post-`70aeb15` is the first measurement below.**

## Decision: target Path B

Two paths were considered:

- **Path A — reintroduce simplification at polygon creation.** Cheap, feeds
  merge straighter edges, but re-litigates the deliberate decision and
  re-introduces the per-plane blindness that motivated removal.
- **Path B — delay classify, fold merge into the regularize loop** so
  edge-merging and regularization **co-converge** toward a clean building
  model. More ambitious; the architecture is friendly to it (regularize is
  already a global, iterative, topology-mutating loop that *already* hosts
  merge-like ops — R.2 proximity-merge + R.3 collinear-removal chase inside
  R.1, with reusable redirect-and-rebuild plumbing).

**We target Path B.** Rationale: the whole point of the 2026-05-21 deferral was
that boundary cleanup needs global, cross-plane, post-merge evidence — which is
exactly what regularize has and merge (running early, per-pair) does not.
Putting edge-merge *inside* the convergent loop resolves the ordering inversion
at the root instead of patching merge's input.

### Structural blockers (validated against the code — see feasibility below)

1. **R.0 DOF-tagging presupposes merge already ran** — DOF comes from
   cross-plane corner membership that merge establishes. **→ Tractable:** with
   merge inside the loop, `n_plane_ids` is built up *during* iteration; R.0
   re-tags on the dynamic plane sets (`planes_dyn`) the loop already maintains.
2. ~~**classify is an *input* to regularize**~~ — **DISPROVEN.** regularize
   consumes *zero* classify outputs (not in `v7_br_compute`'s signature, not
   parsed, only passed through). classify feeds only wall-detect, which runs
   later. The dependency was imaginary; classify simply moves *after* the
   combined step.
3. **regularize's loop is already FP-path-fragile** (1-ULP `acos`/`exp` ties
   cascade to metre-scale drift); a larger move set amplifies it. **→ Real,
   open.** The dominant risk for Path B.
4. **No edge-merge primitive in the loop yet** — only corner-merge; the
   redirect-and-rebuild plumbing + `planes_dyn` + REBUILD_CTXS are reusable, but
   the edge-unification candidate generator is genuinely new. **→ Real, the
   main build effort.**

## Baseline measurements (post-70aeb15)

**Method (reproducible):** produce `boundary-classify-roof-graph` on `v7-bp2`
(fresh `--cache-dir`), strip the 16-byte blob-array header (dahouet = **1
building**: `count=1` + `u64 len`, then `RGRAPH` TLV at offset 16), run
`build-noasan/v7-roof-graph-inspect` on the stripped TLV.

```
tail -c +17 boundary-classify-roof-graph.bin > bldg0.bin
build-noasan/v7-roof-graph-inspect bldg0.bin
```

**Baseline (dahouet bldg0, post-`70aeb15`, 2026-06-11):**

| metric | value |
|--------|-------|
| planes | 59 (34 alive, 25 rejected) |
| **ridges (topology)** | **45** — ridge=21, hip=6, valley=5, step=12, flashing=1 |
| interior-type ridges (ridge+hip+valley) | **32** |
| corners | 298 — RI=14, ridge-end=49, **polygon-merge=7**, concave=169, convex=59 |
| edges | 193 |
| perimeter tags | perimeter=82, **interior-shared=26**, wall=50, overhang-top=26, overhang-bottom=6, level-transition=3 |
| NEW_ADJACENCIES | 0 (section absent — no eave-shared adjacencies synthesized) |

**The merge-effectiveness gap (the headline number):**

- `boundary-merge` splices **two** `polygon-merge` (MRG) corners per successfully
  unified ridge (the overlap endpoints `p0,p1`). Only **7 polygon-merge corners
  exist → merge actually unified ~3–4 ridges** into a single shared edge.
- Yet there are **32 interior-type ridges** in topology and **26 edges** that
  classify still labels `interior-shared` (i.e. interior + multi-plane but
  carried as *separate per-plane edges*, not unified).
- So roughly **3–4 of ~32 interior ridges (~10–15%) get a clean merged edge**;
  the rest remain duplicated. This is the quantitative symptom Path B targets.

**Comparability caveat:** the historical merge.doc.json baseline ("143 edges /
14 shared / 304 corners") predates `70aeb15` and may be the *bldgonly* extract
variant — not directly comparable to this full per-building graph (see the
bldgonly-fixture caveat). The numbers above are the new from-scratch baseline;
future runs compare against *these*.

**Next baseline refinement (TODO):** count EDGES with `n_plane_ids >= 2`
directly (inspect doesn't print it; `v7-boundary-merge-only-bench` computes
`n_shared` as edges with `n_plane_ids>=2`) to get merge's own shared-edge tally
distinct from classify's `interior-shared` tag.

## Path-B feasibility — classify I/O and movability

**Headline: there is no dependency cycle. `boundary-regularize` consumes ZERO
`boundary-classify` outputs.** Verified three ways: classify's two sections
(`PERIMETER_TAGS 0x0C`, `NEW_ADJACENCIES 0x0D`) are **not** in `v7_br_compute`'s
signature, **not** parsed by regularize's reader, and only **passed through**
untouched (`v7-boundary-regularize.c:544-545`, `-impl.h:246-258`). R.0 DOF
tagging derives DOF purely from `corner.n_plane_ids` + plane normals
(`-impl.c:497-546`); R.1 cost terms read corners/edges/polygon_paths/nearby/
positions. None look at tags or adjacencies.

So the chain `merge → classify → regularize` is **not data-coupled at the
compute level**. classify's outputs are consumed only by **wall-detect**
(`tag` + `outward_xz`, hard-required: `v7-wall-detect-impl.c:218-230`), which
runs *after* regularize. score-intrinsic / -postwall read **no** classify
output (postwall parses only PLANES from the wall-detect graph).

### classify I/O (precise)

- **Reads:** `boundary-merge-roof-graph` + per-building records/frames. From the
  graph it uses PLANES (wall test, PIP geometry), RIDGES (`plane_a/b` only, for
  the "both corners share a ridge → interior-shared" gate), CORNERS
  (`point`, `plane_ids`, `ridge_ids`), EDGES (`n_plane_ids` drives wall/multi-
  plane/interior-shared), POLYGON_PATHS (winding + PIP). Does **not** read
  RIDGE_LINES / NEARBY / PLANE_GRIDS.
- **Needs a POST-merge graph** — not via a merge-specific section, but because
  its tagging keys on `edge.n_plane_ids ≥ 2` and corners sharing a `ridge_id`,
  which are exactly what merge synthesizes (unified shared corners + rebuilt
  multi-plane EDGES). On a pre-merge graph those edges are single-plane → the
  interior-shared / eave-shared paths mis-fire. **Classify must run after merge
  has happened** (it will, after the combined step).
- **Produces:** PERIMETER_TAGS (per-edge `tag`, `outward_xz`, coverage) and
  NEW_ADJACENCIES (`plane_a/b, type, edge_id_a/b`). Only wall-detect consumes
  them.

### Relocation verdict: move classify AFTER the combined step

Three options were assessed:

| option | verdict |
|--------|---------|
| (a) fold classify *into* the loop, recompute per iteration | mechanically possible (classify is a pure, stateless, iter-0-free function of the graph) but **pointless + expensive**: the optimizer never reads tags mid-loop, and classify is O(edges²) + PIP-probing — wasteful inside a ≤1000-iter loop |
| (b) **move classify to AFTER** a combined merge+regularize step | **most feasible.** Nothing in regularize needs classify, so nothing breaks; wall-detect still gets fresh tags in time, computed on the *final regularized geometry* (moved + decimated corners) — arguably **more** consistent than today, where classify tags the stale pre-regularize topology |
| (c) synthesize the minimal classify subset regularize needs mid-loop | **degenerate** — the minimal subset is the **empty set**; collapses into (b) |

### Revised Path-B shape

```
bp2 → [ merge folded INTO boundary-regularize loop ] → classify → wall-detect → score
```

The seemingly-circular dependency was imaginary. Concretely the work is:
1. **Add an edge-merge primitive to the regularize loop** (the genuinely new
   piece — see blocker #4). Today the loop has corner-merge (R.2) + collinear-
   removal (R.3) chasing inside R.1; ridge edge-unification (merge's job) is a
   *different primitive* on edges, but the redirect-and-rebuild plumbing,
   dynamic per-corner plane sets (`planes_dyn`), and REBUILD_CTXS are reusable.
2. **Recompute DOF as merges fire** — since merge now lives in the loop, the
   cross-plane membership that DOF tagging needs (`n_plane_ids`) is built up
   *during* the loop; R.0 re-tagging on the dynamic plane sets dissolves
   blocker #1.
3. **Reorder classify after the combined step** — a trivial pipeline reorder,
   since regularize ignores classify entirely.

### Validation caveat (before committing)

classify and wall-detect must still resolve consistent merge-created shared
corners/edges/unified-ids **after** regularize's R.0b/R.2/R.3 corner decimation
(which rewrites CORNERS + POLYGON_PATHS). Confirm the forwarded EDGES section
stays consistent with the surviving corner set — i.e. edge→corner lookups don't
dangle after corner deletion.

## Open questions / next steps

**Resolved this session:** ordering inversion identified; baseline measured
(~3–4 of 32 interior ridges merged); classify is *not* a regularize input, so
Path B has no dependency cycle — classify moves after a combined step.

**Open:**
1. **Edge-merge candidate generator inside the loop** (blocker #4) — design the
   primitive: when do two plane edges along a ridge become one shared edge,
   evaluated *during* descent instead of once up front? Can it reuse merge's
   ridge-line projection (`find_single_run` / interval overlap) but applied
   incrementally as corners move, so a ridge that was too rough at iter 0
   becomes mergeable once regularize straightens it?
2. **FP fragility** (blocker #3) — quantify how much the enlarged move set
   worsens the 1-ULP cascade; decide whether determinism is still a goal or we
   lean fully into stability-under-perturbation (cf. the bp2 success-criterion
   shift, [[2026-05-21-boundary-polygons-tsp-rethink]]).
3. **EDGES↔CORNERS consistency** after R.0b/R.2/R.3 decimation when classify
   runs post-regularize (validation caveat above).
4. **Direct shared-edge tally** — add an `n_plane_ids>=2` count to the baseline
   (inspect doesn't print it) for a cleaner before/after metric than the
   `interior-shared` tag proxy.
5. **Convergence definition** — what does "converged clean building model" mean
   as a stopping criterion when merge + regularize iterate together? (Today
   regularize stops on `bestImprovement < 1e-9` or iter cap.)
6. **Should boundary-topology evidence feed the in-loop merge?** RIDGE_LINES
   (which regularize *does* read) gives the ideal ridge geometry the in-loop
   merge would snap edges toward — possibly the bridge between "rough now,
   mergeable after straightening."
