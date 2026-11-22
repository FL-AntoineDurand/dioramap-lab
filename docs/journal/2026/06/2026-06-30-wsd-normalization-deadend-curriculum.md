---
id: "2026-06-30-wsd-normalization-deadend-curriculum"
date: 2026-06-30
title: "WSD normalization is a dead end — the un-normalized sum is an implicit per-plane curriculum"
tags: [boundary-regularize, wsd, cost-function, normalization, curriculum, coverage, corner-insertion, v7]
category: finding
status: concluded
refs:
  - "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
  - "2026-06-29-regularize-corner-insertion-edge-fit-metric"
pipeline: v7-bp2
---

## Summary

The corner-slide audit ([[2026-06-29-corner-slide-runaway-and-wsd-coverage-audit]])
proposed **WSD normalization (÷Σw)** as the "foundational" track #0 to fix the runaway
and the cross-plane size-coupling. We prototyped it (param `wsdNormalize`, default-on
+ `lambda_wsd=2`, step v17) and it *looked* like a clean win on the probe corner: the
id=30 runaway vanished and the consistency gate stayed green. **It is wrong.** A
per-plane coverage dump + a per-iteration curriculum measurement show normalization
**broadly destroys coverage**, and — more importantly — reveal *why* the un-normalized
"simple sum" is actually the correct, well-calibrated cost. **Normalization is reverted
and recorded here as a dead end.** This entry supersedes track #0 of the audit.

## What normalization actually did (broad coverage regression)

A new `BR_DUMP_COVERAGE` dump (per-plane FINAL polygon vs its owner points) comparing
un-normalized λ1 → normalized λ2:

| plane | n_out% old→new | mean misfit (m) old→new | polygon UV old→new (owners) |
|---|---|---|---|
| 3 | 9% → 35% | 0.010 → 0.294 | 10.7×6.4 → 9.9×5.2 (11.1×6.3) |
| 4 | 7% → **73%** | 0.012 → **1.539** | 6.4×8.2 → **4.1×5.3** (6.8×8.7) |
| 8 | 16% → 57% | 0.013 → 0.387 | 4.6×5.6 → 2.8×5.3 (4.6×5.8) |

Not a few planes — a **regression scan flagged 10+** (43: 4→89%, 20: 33→94%, 0: 31→72%,
…). Polygons shrank and shed points everywhere; mean misfit went 0.01 → 0.3–1.5 m. The
consistency gate missed it because it checks structure, not coverage quality.

## Why — the per-iteration curriculum measurement (`BR_PROFILE`)

`[br-allmetrics]` (per-iter, cost-weighted term totals) on the **un-normalized** config:

| iter | total | WSD (share) | other terms |
|---|---|---|---|
| 1 | 173412 | 172586 (**99.5%**) | 826 |
| 50 | 48047 | 47717 (99.3%) | 330 |
| 200 | 10823 | 10536 (**97.4%**) | 287 |

Globally WSD stays ~98% — *not* a clean "WSD-then-others" curriculum. But the **per-plane**
WSD trajectories (`[planewsd]`) tell the real story:

| plane | own_out iter1 → iter200 | reading |
|---|---|---|
| 3 | 9.2 → 8.7 | already covered by bp2; stays low → its corners are driven by **alignment** the whole time |
| 4 | 8.7 → 7.5 | already covered |
| 8 | 5.5 → 2.2 | already covered |
| **43** | 26 → 9 → **~0** | **the curriculum in action**: had a gap, drove WSD to zero, *then* alignment refines |
| **0** | 21412 → 7878 → **1700** | the pathology: dominant, declining, **never converges** |

So the curriculum is **real but per-plane**: for any plane whose cloud *can* be enclosed,
WSD self-extinguishes through iterations and hands its corners to the bounded terms
(plane 43 is the clean proof). The global share stays WSD-dominated only because **plane 0
never converges** and drowns the sum.

## The insight: the un-normalized sum is correctly calibrated

`own_out = Σ_outside w·d` is **not** proportional to plane *size* — it's proportional to
coverage *failure*. A big, well-covered plane has *low* own_out (plane 4: 748 pts →
own_out 6.5). And a well-covered plane's own_out (~6) sits on **the same scale as the
alignment terms** (~a few per corner), so they balance and coverage holds. The "46×
size-coupling" flagged in the audit was mis-framed: it is **failure-coupling**. Only the
genuinely-broken plane 0 dominates, and it *should* — it is the worst defect in the
building (1510 pts ~7 m outside).

That is exactly why normalization breaks everything: dividing by Σw makes a well-covered
plane's WSD ~1000× too small (plane 4: 6.5 → 0.012), dropping it far below the alignment
terms → the optimizer trades coverage away → polygons shrink. λ=2 only scaled it ~2×;
matching the extensive pressure would need λ ≈ Σw (hundreds), which re-breaks everything.
The extensive form's "unfairness" is **load-bearing**: it is what enforces coverage.

## The runaway is a missing-capability problem, not a weighting bug

Plane 0's FINAL un-normalized polygon: **POLY 69.7×78.6 m** vs OWNERS 22.6×31.5 — i.e. it
ballooned to 70 m, which **is** the id=30 spike. Plane 0's WSD is irreducible (4807 pts
over 22×31 m can't be enclosed by the corners bp2 gave it; own_out 21412 *before*
regularize even starts), so the optimizer keeps trying and its only lever — slide a vertex
— produces the spike. It is **not** a cost bug; it is a missing capability. **Corner
insertion ([[2026-06-29-regularize-corner-insertion-edge-fit-metric]], #25)** gives it the
right move → WSD drops like plane 43 → curriculum completes → no spike.

## Decisions / strategy pivot

- **Normalization (÷Σw): CANCELLED.** Reverted `wsd_normalize`→false, `lambda_wsd`→1,
  step version 17→16. The cost is correct as a simple sum. (Param machinery left dormant
  + uncommitted; excluded from any commit.)
- **Global compactness term: CANCELLED.** Same distortion risk as normalization — it would
  penalise legitimately non-compact roofs. Any shape guard must be **local/per-vertex**.
- **In-loop corner insertion (#25): PRIMARY root fix.** Elevated from "future idea" — it is
  the principled cure for the runaway / under-covered-plane class.
- **Slide-clamp + margin (#26): interim guard.** It stops the spike *rendering* without
  touching the WSD scale (so coverage is untouched — unlike normalization), making it the
  right cheap stopgap until #25 lands.
- **Upstream plane-0 polygon (#27):** still worth diagnosing — bp2 handed regularize a
  polygon already at own_out 21412, so part of the fix may belong in `boundary-polygons-2`.

## Instrumentation added (all env-gated, uncommitted)

`BR_DUMP_COVERAGE` (per-plane final coverage: n_out%, mean misfit, poly-vs-owner bbox),
`[planewsd]` per-plane WSD trajectory inside the `BR_PROFILE` `[br-allmetrics]` block.
These are directly reusable for the #25 edge-misfit work.
