---
id: "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
date: 2026-06-29
title: "Corner slide-to-infinity: a DOF-1 runaway that exposed missing shape-regularization and un-normalized WSD"
tags: [boundary-regularize, boundary-polygons-2, wsd, cost-function, dof, normalization, compactness, runaway, coverage, v7]
category: finding
status: concluded
refs:
  - "2026-05-10-regularization-cost-architecture"
  - "2026-05-30-bp3-contour-arclength-ordering"
  - "2026-05-22-bp3-grid-fill-topology"
  - "2026-06-02-ts-weights-dimensional-audit"
  - "2026-06-20-in-loop-make-shared-cascade-complete"
  - "2026-04-27-regularization-exploration"
  - "2026-06-29-regularize-corner-insertion-edge-fit-metric"
pipeline: v7-bp2
---

## Summary

One corner on dahouet (`RE2+P9C5+RE39`, id=30) ends boundary-regularize **85 m off
the roof** at (−70, −2.9, −51.6). Chasing it down peeled back four layered causes,
each more architectural than the last:

1. **Mechanism** — a **DOF-1 ridge-slide runaway**: 621 monotonic accepted moves
   walk the corner off the end of its ridge. Not a merge, not a solver divergence.
2. **Energy driver** — the cost that "dumbly" improves for all 621 moves is **WSD**,
   specifically its `own_out` (coverage) half: 99.95 % of the cost, and there is **no
   shape-regularization term** in regularize to oppose it.
3. **Why coverage is so bad** — **plane 0's polygon is grossly undersized**: it
   covers ~28 % of its own 4807 points; 72 % sit 5–20 m outside it. An upstream
   `boundary-polygons-2` defect.
4. **Why WSD overwhelms everything** — WSD is **un-normalized** (`Σ w·d`, extensive),
   so a big plane's coverage cost dwarfs every other term *and* every other plane.
   Measured size-coupling: ~46× inflation beyond the genuine fit-quality gap.

A v17 slide-clamp was implemented as a **backstop** (verified, gated, uncommitted),
but the user correctly judged it premature: it treats the symptom. The real backlog
is tracks 0–3 below. The in-loop corner-insertion idea that emerged is written up
separately in [[2026-06-29-regularize-corner-insertion-edge-fit-metric]].

All instrumentation referenced here is env-gated (`BR_*`), zero default output, and
currently uncommitted (see Appendix).

---

## 1. The trigger

`RE2+P9C5+RE39` (id=30, planes {0,9}, DOF-1 on ridge 5 — a type-5 STEP/eave between
roof plane 0 and wall plane 9) lands at (−70.49, −2.88, −51.61), i.e. ridge-5
parameter **t ≈ −85** against a segment of `tmax = 4.26`. The question driving the
whole investigation: where was it **born**, where did it **merge**, where did it
**move**?

## 2. Diagnosis — a DOF-1 runaway slide (not a merge, not the solver)

- **Born on the building.** `BR_DUMP_POOL` (boundary-corners pool) shows the merged
  entry and all three members coincident at `(-7.501, -5.554, -1.405)` (z-up). Rotated
  into the regularize frame (`x, -z, y`) that is `(-7.50, -1.40, 5.55)` = **ridge 5's
  origin, t=0**. The pool merge is correct; nothing is born far.
- **Not the pool/meet solver.** First hypothesis was that `v7p_solve_member_position`
  (`v3/steps/v7/v7-corner-pool.c:205`) diverged on near-parallel ridge axes (RE2 and
  RE39 carry identical axes). Instrumented with `BR_SOLVE_TRACE` → **0 divergences**.
  Hypothesis killed.
- **It slid, gradually.** The trajectory dump (`[ctraj]` inside `BR_DUMP_CM`) shows
  **621 accepted moves, monotone**, each ~0.13 m along the *same* ridge direction,
  carrying t from 0 → −85. It stops only because the 200-iteration budget runs out —
  still improving at the last step. This is the classic unanchored DOF-1 slide: the
  corner's slide axis has no term pinning it laterally.

Confirmed **pre-existing**, not a regression from the v15/v16 make-shared work
([[2026-06-20-in-loop-make-shared-cascade-complete]]): clamp-off reproduces −70 exactly.

## 3. Why the cost keeps improving — WSD / `own_out`

`TOTAL_COST` (`v3/steps/v7/v7-boundary-regularize-impl.c:~1841`) is a weighted sum:
WSD + angle + neighbour-angle + horizontal-edge + MR-fit + R.7 pairwise
(horiz-coplanar / vert-coplanar / collinear). A per-term probe (`BR_COST_PROBE=30`)
decomposes it along the slide:

| t | total | **WSD** | own_out | foreign_in | all other terms |
|---|---|---|---|---|---|
| 0 | 21976 | 21965 | 21965 | 0.05 | ~11 |
| −30 | 3550 | 3546 | 3542 | 4 | ~3 |
| −85 | 1709 | 1704 | 1646 | 57 | ~4 |

- **WSD is 99.95 % of the cost.** Angle/edge/coplanar are O(1) — drowned.
- **`own_out` (owner points left *outside* the polygon, weighted by distance-to-edge)
  is the entire driver.** `foreign_in` (foreign points wrongly inside) opposes it but
  is ~400× too weak, because the spike sweeps into **empty space** with no foreign
  points to push back.
- **The per-move gain is large, not tiny** — ~450/iter early, decaying to ~3.4/iter at
  t=−85, never reaching the 1e-9 accept floor. The vertex doesn't cover points by
  *being* somewhere; its two long **incident edges sweep** across the uncovered mass
  (lever arm ∝ edge length). A degenerate spike is a cheap way to sweep a big triangle.

`WSD_EVAL` is at `v7-boundary-regularize-impl.c:~1722`; the diagnostic
`WSD_EVAL_SPLIT` added beside it separates the two halves.

## 4. Root cause #1 (upstream): plane 0's polygon is grossly undersized

Per-plane breakdown (`[costprobe.plane]` / `[costprobe.hist]` / `[costprobe.bbox]`):

- **Plane 0** (roof, not a wall): 4807 owner points, own_out = 21952 — the entire
  driver. Histogram: **2956 points sit 5–20 m outside** the polygon (98 % of the cost),
  `dmax = 16 m` — **no far outliers**, just systematic under-coverage.
- UV extents: **polygon 5.8 × 18 m** (u∈[5.0, 10.9], one corner) vs **owner points
  22.6 × 31.5 m** (u∈[−11.2, 11.4]). The polygon never reaches the −u half where most
  points are. **Single ring, 10 verts.**
- **Plane 9** (wall): polygon ≈ owners (4.2×7.8 vs 4.3×7.8) — healthy by comparison.

So id=30's slide is the optimizer *correctly* trying to grow plane 0's polygon to
cover its own points — with the only lever it has (one DOF-1 vertex), which produces a
spike instead of a proper enlargement. The real defect is in `boundary-polygons-2`
(under-built polygon, or over-assigned owner set) — a **separate upstream thread**.

## 5. Root cause #2 (energy): no shape-regularization term in regularize

There is **no compactness/spike/displacement term** in regularize's cost to resist the
spike. History (recovered via git pickaxe + RAG):

- **Compactness exists — but in `boundary-polygons-2`, not regularize.**
  `v7_bp2_polygon_compactness_uv` = `perimeter² / (4π·area)`
  (`v7-boundary-polygons-2-impl.c:103`), the isoperimetric quotient (1.0 for a circle,
  →∞ for a sliver/spike). It was even *envisioned* for regularize as far back as
  [[2026-04-27-regularization-exploration]] ("free corners nudged to improve polygon
  compactness") but ended up implemented one step upstream.
- **Two anchors that *would* have bounded this were removed.** `maxDisplacement`
  (hard per-corner drift cap, removed in `5d29a25`) and `useDisplacementCost` /
  `lambdaDisp` (soft "stay near initial" anchor, removed in `3fd069d`), both on the
  judgment that "the remaining fit terms already pull corners to sensible positions."
  That holds **only when bp2's shape is good** — which is exactly the assumption that
  breaks here.

Why compactness is principled in bp2 but dangerous as a blanket regularize term:

- In **bp2** it ranks *orderings of a fixed vertex set* — it cannot distort real extent,
  only de-zigzag. Safe tie-breaker.
- In **regularize** a global compactness *penalty* would distort **legitimately
  non-compact** roofs (a long thin wing has compactness ≈ 2.6, an L-shape worse). The
  safe analog is a **local, per-vertex anti-spike** term (bp2's *spike* penalty), which
  kills the degenerate vertex without shrinking global extent. (Interesting inversion:
  bp3 dropped spike in favour of compactness-only for *ordering* because the global
  ratio subsumes the local one — see [[2026-05-30-bp3-contour-arclength-ordering]]; for
  *positions* the logic reverses.)

## 6. Root cause #3 (scale): WSD is un-normalized — the cross-plane / cross-term defect

Both steps compute the **identical extensive** WSD `Σ w·d` (regularize
`wsd_sum / wsd_sigma_m`; bp2 `v7_bp2_wsd_multi` at `:233`). The only "normalization" is
a fixed constant `wsdSigmaM` (default 1.0 m), and it is **deliberate** — the param doc
(`lidar-algos-bench-v2/lib/pipeline/steps/boundary-regularize.js:51`) says *"NOT divided
by point count — diluting by total plane size masks violation severity."* That reasoning
assumed violations of *a few metres*.

Measured (iter 1, `[costprobe.norm]`):

| plane | n_owner | Σw | **raw WSD** (enters cost) | per-point | **mean misfit (m)** = WSD/Σw |
|---|---|---|---|---|---|
| 0 | 4807 | 3107.9 | **21 952** | 4.57 | **7.06** |
| 9 | 91 | 67.7 | **13.1** | 0.14 | **0.19** |

- **Raw WSD ratio (0:9) = 1673×. Genuine quality gap (mean-misfit) = 36×.** The
  un-normalized sum **inflates the big plane's dominance ~46×** beyond its actual fit
  quality. Two planes of equal fit quality but different size behave very differently
  through the algorithm; a corner shared between them is governed by point *count*, not
  fit quality.

Three distinct defects:

1. **Cross-plane unfairness** — a shared corner is dominated by the largest plane, not
   the worst-fitting one (~46× here).
2. **Cross-term scale is plane-dependent** — WSD enters cost *raw* (21 952) while
   angle/horizontal are saturated to [0,1]; `lambdaWSD = 1.0` therefore means something
   different on every plane. The weight is not physically meaningful.
3. **No ceiling + assumed-small violation** — the doc's "5 m → cost 5 → dominates angle"
   logic has no upper bound; a broken polygon gives 21 952, an unopposed attractor.

**How WSD is *used* differs between the steps** — the lesson regularize never got:

| | regularize | bp2 / bp3 |
|---|---|---|
| WSD's role | **primary fine-move term, every iteration** | best-cycle **selection only** |
| fine refinement | WSD-dominated | compactness + spike — **WSD removed** |
| WSD compares | **across different-size planes** (size does *not* cancel) | cycles of the **same** points (size cancels) |
| search | continuous, unbounded | discrete, bounded |

bp2 explicitly pulled raw WSD out of fine arbitration (*"WSD is precisely the signal the
stripes corrupt"*, [[2026-05-30-bp3-contour-arclength-ordering]]) and uses a normalized
ratio instead; regularize kept raw WSD as its dominant fine-move term, summed across
planes where the size-coupling does not cancel.

**Should WSD be normalized?**
- **By Σw (or count): yes** → weighted-**mean misfit distance** (metres): intensive,
  size-invariant, dimensionally clean, comparable across planes and to a re-tuned σ. The
  "don't mask severity" concern is preserved by σ, not by leaving it count-coupled.
- **By perimeter: no — actively harmful.** Perimeter is a *variable being optimized*;
  normalizing by it makes a longer perimeter *reduce* cost → it would **reward** the very
  spike we are fighting.

This connects directly to the open dimensional thread in
[[2026-06-02-ts-weights-dimensional-audit]] (`w` is a per-point area/`1/density` weight;
remediation tasks already pending there).

**Honest limit:** normalization fixes the *fairness* and *scale-consistency* defects and
is a **prerequisite** for any shape term to bite (today they are drowned 21 952 : 1), but
it does **not by itself** stop this runaway — plane 0's mean misfit is genuinely ~7 m ≫ σ,
so normalized WSD still dominates and the slide is still a descent direction. It must
compose with a shape term and/or corner insertion.

## 7. Interim fix shipped (backstop): v17 DOF-1 slide-clamp

`clamp_dof1_slide` (`v7-boundary-regularize-impl.c:~503`) bounds a DOF-1 corner's slide
to its binding ridge's clipped segment `[0, tmax]`, wired into **both** slide paths — the
single-corner sampler (`:~2769`) and the joint two-corner sweep (`:~2949`). Gated
`BR_SLIDE_CLAMP` (default on). Two false starts worth recording:

1. First version used `ridge_dof_for_corner`, which **requires in-segment**, so it stopped
   matching the moment the corner crossed t<0 — the clamp never engaged. Fixed by finding
   the binding ridge via **axis-parallelism** instead (robust off-segment).
2. First version only clamped the single-corner sampler; the corner escaped via the
   **joint sweep** (`RANDOM_DOF_OFFSET`), the second, unclamped slide path.

Verified: id=30 pins at the building (t=0, perp 0), no outliers remain, consistency gate
green (`dead_id_refs=0 P2sup=0 P3sup=0 edge_orphans=0`), ASan unit tests 163 (regularize)
+ 135 (corners), both builds 0-warning. Step `.version` bumped 16→17.

**Status: backstop, not a cure, and currently uncommitted.** It is `[0,tmax]` with **zero
margin** — too tight, since ridge extents are inexact and refining them is part of
regularize's job. If kept, it needs an **end margin**. It does not address roots #1–#3.

## 8. Open tracks / future work

| # | Track | Addresses | Note |
|---|---|---|---|
| 0 | **WSD normalization** (÷Σw → mean misfit; re-tune σ) | root #3 | **Foundational** — wrong independently of this bug; makes 1–3 evaluable on a sane scale |
| 1 | **Local anti-spike / per-vertex shape guard** | root #2 | bp2's *spike* term lifted; safe (doesn't distort extent) |
| 2 | **Compactness term in regularize** | root #2 | `perim²/4π·area` exists in bp2; global → risk of distorting real non-compact roofs |
| 3 | **In-loop corner insertion + edge-fit metric** | root #1 | recovers undersized polygons; removes the spike *incentive*. Designed in [[2026-06-29-regularize-corner-insertion-edge-fit-metric]] |
| — | **Ridge clamp + margin** | symptom | cheap belt-and-braces backstop (the v17 work, plus a margin) |
| — | **plane 0 undersized polygon** | root #1 (upstream) | `boundary-polygons-2` under-coverage / owner over-assignment — separate investigation |

How they compose: **normalization** makes the cost fair and lets the bounded terms bite;
**anti-spike/compactness** makes the spike geometry costly on that now-comparable scale;
**corner insertion** offers a *better descent move* (bowing the edge to actually cover the
2956 points beats any sliver), removing the incentive to slide. The clamp+margin is the
cheap last line.

## Appendix — instrumentation, code, and references (all `BR_*` gated, uncommitted)

**Diagnostics added** (zero default output):
- `BR_DUMP_POOL` — boundary-corners pool entry + per-member birth pos/axis
  (`v7-boundary-corners-impl.c`, serialization loop).
- `BR_DUMP_CM` `[ctraj]` — outlier-corner full trajectory
  (`v7-boundary-regularize-impl.c`, in the existing `BR_DUMP_CM` block).
- `BR_COST_PROBE=<id>` — per-term + per-plane + distance-histogram + UV-bbox + ring-count
  + Σw-normalization decomposition for one corner; uses `WSD_EVAL_SPLIT`
  (`v7-boundary-regularize-impl.c`).
- `BR_SOLVE_TRACE` — *reverted* (was a dead-end probe in the pure-math `v7-corner-pool.c`).

**Key code:** `TOTAL_COST` (`:~1841`), `WSD_EVAL` (`:~1722`), `ridge_dof_for_corner`
(`:477`), `clamp_dof1_slide` (`:~503`), DOF-1 sampler (`:~2769`), joint sweep (`:~2949`),
`br_slide_clamp`/`br_cost_probe` gates (`:~1027`) — all `v7-boundary-regularize-impl.c`.
`v7p_solve_member_position` (`v7-corner-pool.c:205`). `v7_bp2_wsd_multi`
(`v7-boundary-polygons-2-impl.c:233`), `v7_bp2_polygon_compactness_uv` (`:103`),
cost-functional/select vs geometric/refine (`:~322` / `:~308`). `wsdSigmaM` param doc
(`boundary-regularize.js:51`).

**Commits:** `3fd069d` (drop `useDisplacementCost`), `5d29a25` (toggleable terms +
two-sided WSD + removed `maxDisplacement` cap), `251efb6` (per-point trust weighting),
`9455e2e` (uniform weight = 1/density), `bc8a1cd` (normalised cost terms + linear WSD).
