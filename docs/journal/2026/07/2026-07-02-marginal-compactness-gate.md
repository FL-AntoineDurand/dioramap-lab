---
id: "2026-07-02-marginal-compactness-gate"
date: 2026-07-02
title: "Marginal-compactness gate: retracting drift-into-void without a shape target"
tags: [boundary-regularize, compactness, cost-function, wsd, drift, development, v7]
category: development
status: concluded
refs:
  - "2026-07-02-corner-insertion-lands"
  - "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
  - "2026-06-30-wsd-normalization-deadend-curriculum"
pipeline: v7-bp2
---

## Rationale — the second failure mode

Regularize fails two ways. *Under-coverage* (polygon too small) is fixed by in-loop
corner insertion ([[2026-07-02-corner-insertion-lands]]). This entry is the second mode:
**drift-into-void** — a DOF-1 corner slides indefinitely into empty space. The canonical
case is id=136 / `P6C3+P0CP0`, a shared corner on ridge 16 (a type-5 step between the
terrain plane 0 and wall plane 6), which slid to t=−25 on a [0, 7.26] segment — 34 m off
the building. The real walls 9 and 15 have the same pathology, milder.

**Why WSD can't see it.** WSD has two halves: `own_out` (owner points outside → grow the
polygon) and `foreign_in` (foreign points inside → don't over-extend). `foreign_in` is
the natural anti-drift — a polygon spiking into occupied space engulfs foreign points and
is penalized. But it only bites *where foreign points exist*. A cost probe on id=136 at
its spiked position:

```
wsd=8.06 (own_out=7.88  foreign_in=0.18)   ang=0.02 nang=1.81 hcop=0.75 ...
```

`foreign_in ≈ 0` — the spike sweeps *empty* space, so the anti-drift term is silent, and
`own_out` is flat (the plane is already covered). Both WSD halves go quiet; the bounded
aesthetic terms (~2.5 total) can't hold it. **Only a point-independent term — shape — can
oppose a drift into emptiness.**

## History — three dead-ends (do not re-walk)

1. **Absolute compactness threshold C₀.** Idea: penalize `perim²/(4π·area)` above a
   cutoff. Killed by the data: the spike scores only **5.47** vs an honest thin roof at
   **3.60–4.30** — no clean gap. Reason: id=136's drift is a large-area **bulge**, not a
   zero-area **sliver**, so `perim²/area` barely moves (perimeter *and* area both grow).
   Compactness cleanly flags slivers, weakly flags bulges.
2. **Weighted `λ · compactness` cost term.** Works in isolation (plane 6 5.47→2.55 at
   λ=3) but **scale-coupled**: dimensionless compactness competes with size-*extensive*
   WSD, so it dominates small planes (which lose coverage) while under-serving big ones.
   This is the same family as the WSD-normalization dead-end
   ([[2026-06-30-wsd-normalization-deadend-curriculum]]).
3. **Relative-to-current-WSD eps.** To decouple scale, make the "real gain" threshold a
   fraction of each plane's current WSD. **Death spiral**: as a plane's WSD shrinks,
   `eps·WSD` shrinks with it, so every incremental covering step (gain < eps·WSD) gets
   vetoed — the gate blocks the very coverage it should allow (plane 0 own_out 7.6 → 231).

## Implementation — the working form

The user's reframe cut through it: **it is not an absolute target — a corner move should
not make the *considered plane's* compactness worse for *nothing* on WSD.** That is a
*marginal, gated* rule, not a weighted sum. Four adjectives, each forced by a failure:

- **Marginal** — compare a candidate's compactness to the *current* position, not a target.
- **Gated (hard veto), not weighted** — decouples scale entirely; no λ to tune against
  plane size.
- **Per-plane** ("the *considered* plane") — check each plane independently. id=136 sits
  on plane 0 (big, noisy WSD ~7.6) *and* plane 6 (tiny); a summed gate let plane 0's WSD
  wobble excuse wrecking plane 6's shape. Per-plane, plane 6 alone vetoes the move.
- **Both accept paths** — the drift's real vehicle is the **joint two-corner sweep**
  (`RANDOM_DOF_OFFSET`, accepted on raw `TOTAL_COST`), not just the single-corner sampler.
  Gating only the single path left the drift untouched (5.47→5.42). Both paths → it stops.
- **Absolute eps** — "for nothing on WSD" = ΔWSD below a small *absolute* floor
  (0.05 WSD units ≈ one point moving ~1 m). Relative death-spirals; absolute doesn't.

Mechanics: `GATE_ADD_PLANES` captures a corner's per-plane (WSD, compactness) reference at
the current position; `GATE_VETO_NOW` re-evaluates at the candidate and vetoes if any plane
has `compact_cand > compact_cur` **and** `wsd_cand ≥ wsd_cur − eps`. Wired into `EVAL_CAND`
(single-corner) and the joint sweep.

## Results (dahouet bldg0, eps = 0.05)

| plane | what | before (POLY / compact) | after (POLY / compact) |
|---|---|---|---|
| 6 | terrain/wall drifter (id=136) | 32.7×7.9 / 5.47 | **7.3×7.4 / 1.24** |
| 9 | real wall, same drift | 14.7×7.9 / 3.02 | **5.9×7.7 / 1.36** |
| 0,2,3,4,5 | real roof facets | — | unchanged |

The 34 m spike retracts to the real extent; wall 9's 9 m of over-extension is removed. No
real roof coverage is lost (the retracted area was empty).

### The methodological lesson

The first verdict was "**the gate breaks 5–9 small planes → it fails**." That was wrong —
an artifact of counting **false-plane coverage as real roof**. Ground-truthing the planes
IRL (see [[dahouet-plane-taxonomy]]): the "regressed" planes are **false detections**
(42/43/45/50 — over-segmentation junk, density 1–6 pts/m² vs 18–20 for real facets) plus
**minor dormers** (48/22). Once only *real* facets count, the gate is a net win: it fixes
the real drifters and leaves real roof alone. **Lesson: before calling a side-effect a
regression, ground-truth the planes — a metric that "gets worse" on a plane that should
not exist is not a regression.**

## Status / open

- Params `compactGate` (default off) + `compactGateEps` (0.05), set in `v7-bp2.json`. Env
  `BR_COMPACT_GATE` / `BR_COMPACT_GATE_EPS` override for CLI A/B. **Uncommitted prototype.**
- Open: plane-9's small over-tighten (7→15 % out) — an eps nuance; the **false planes are
  an upstream GRM problem** (ts-weights, [[finding_ts_weights_dimensional_audit]]), not
  this step's job; broader-than-dahouet validation still owed.
- The weighted `lambdaCompact` term is left in the code but off — kept only as the
  documented scale-coupled counter-example.
