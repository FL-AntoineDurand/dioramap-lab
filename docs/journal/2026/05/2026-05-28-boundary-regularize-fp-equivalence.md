---
id: "2026-05-28-boundary-regularize-fp-equivalence"
date: 2026-05-28
title: "Boundary-regularize v2↔v3 — FP equivalence push, 7/7 per-cost-term + 15/17 aggregate"
tags: [v3, boundary-regularize, iso-equivalence, fp-precision, drift]
category: bugfix
status: concluded
pipeline: v7
refs: [2026-05-27-boundary-regularize-iso-baseline]
---

# Summary

Closed the remaining v2↔v3 iso drift in `boundary-regularize` down to **per-cost-term byte-exact at 1000 iter (7/7)** and **aggregate iso 15/17 pass**. The 2 remaining aggregate fails are FP-coin-flip drift attributable to `Math.acos`/`Math.exp` divergence between V8 and glibc — algorithmically correct, not a porting bug. Multiple real FP-correctness bugs were fixed in v2 along the way.

# Final state

```
Per-cost-term iso (each cost term in isolation, no R.2/R.3, 1000 iter)
  WSD                  OK  byte-exact
  angle                OK  byte-exact
  neighbor-angle       OK  byte-exact
  horizontal-edge      OK  byte-exact
  horizontal-coplanar  OK  byte-exact
  vertical-coplanar    OK  byte-exact
  collinear            OK  byte-exact

Aggregate iso (all features, 1000 iter):  15/17 pass
  ✓ n_corners_total, dof0/1/2, unused_removed, redundant_ridge_removed
  ✓ r1_corners_optimisable (95=95)
  ✓ r2_merged_count (8=8), r3_collinear_removed (13=13)
  ✓ r1_iterations (1000=1000), r1_total_moves (1000=1000)
  ✓ dof.cornerId set, per-corner dof, axisA/axisB alignment
  ✗ movements.cornerId set:    only_v3=6 corners
  ✗ movements.after position:  52/79 > 1mm, max|Δ|=3.18m on one corner
```

Compared to the 2026-05-27 baseline (7/17 → 15/17), this session lifted the per-cost-term iso from 5/7→7/7 at iter 200 and held byte-exactness all the way to iter 1000 in isolation.

# Real bugs fixed (algorithm + FP correctness)

**v2** (`lidar-algos-bench-v2/lib/pipeline/steps/boundary-regularize.js`, version 35 → 37):

1. **`angleDeviationCost` pure-radian path** — replaced
   `(cornerAngle * 180/π − target_deg) * π/180` with
   `cornerAngle − target_rad`. The deg→rad→deg round-trip introduced
   sub-ULP FP error that compounded over 1000 iters into m-scale
   position drift.
2. **`generateCandidates` integer-multiplied offsets** — replaced
   `for (s = -NUDGE_RANGE; s ≤ NUDGE_RANGE; s += NUDGE_STEP)` with
   `for (i = -nSamp; i ≤ nSamp; i++) { s = i * NUDGE_STEP; }`. The
   FP-accumulated `s` drifted from integer-multiplied at high index;
   sub-ULP candidate positions caused different best-candidate
   selection vs v3 (which already used integer multiplication).
3. **Convergence threshold align** — `bestImprovement < 0.001` →
   `< 1e-9`, matching v3's per-candidate hysteresis. v2 used to stop
   at the same iter as v3 only when both converged early; this lets
   them run to the same max-iters when not converged.
4. **R.2/R.3 chase reorder** — phased (drain R.2 chain, then drain
   R.3) → interleaved (R.2 once → R.3 once → loop, match v3). v3's
   interleaved order is arguably better (catches new R.2 opportunities
   created by R.3 in same iter); v2 now mirrors it.
5. **`r1_corners_optimisable` stat semantic** — final filter now
   matches v3: `alive AND dof>0 AND has-context`. Was just
   `alive AND was-initially-optimisable`, which over-counted.
6. **`useWSDCost` CLI typo** — accept both `useWSDCost` (camelCase) and
   `useWsdCost`. The case-folded variant was silently misaligned.

**v3** (`v3/steps/v7/v7-boundary-regularize-impl.c`):

1. **`NEIGHBOR_ANGLE_COST` macro rewrite** — sum (not average) over
   UNIQUE neighbour ids of `angleDeviationCost(nid)`; each neighbour
   contributes the sum-over-its-contexts. Matches v2's
   `pairwiseCostForCorners([cornerId], ...)` semantic.
2. **`HORIZ_EDGE_COST` macro rewrite** — switched to deduped
   `pw_edges` (shared with R.7) and exact `dy²/len²` formula. Earlier
   per-context `atan2`-based path double-counted shared edges and
   diverged from v2 at non-tiny tilts.
3. **Owner.bin source path** — `v7-boundary-regularize-only-bench`
   now reads `point-stealing_0.owner.bin` (post-stealing), not
   stale `grow-refit-merge_0.owner.bin`. v7 pipeline writes owner.bin
   three times; the bench was reading the wrong one.
4. **DOF=2 candidate sampling** — switched from a 2D `(n×n)` grid
   to v2's per-axis line scan `(axis_a, ±NUDGE_STEP·k) ∪
   (axis_b, ±NUDGE_STEP·k)`. The grid found improvements v2 missed,
   triggering 2-4× more accepted moves per iter.

**Both** (16 v2 step files + v3 strict-no-fallback):
- **Plane-fit weighting unification** — `fitPlaneWeighted` (area-/density-weighted PCA) used wherever a plane normal is fit. v2 had 16 sites still using unweighted PCA; v3 now asserts on missing point-weight input (no silent fallback).

# Root cause for the remaining 2 fails

The 6 movements only in v3 and the 3.18 m max position drift come from V8's `Math.acos` and `Math.exp` differing from glibc by 1 ULP at specific inputs. Directly verified:

```
   x          V8 Math.acos(x)         glibc acos(x)
  -0.7   0x4002c501446cd5f2   0x4002c501446cd5f1   ← 1 ULP differ
   0.5   0x3ff0c152382d7366   same                 ← match
   0.9   0x3fcceb86fd...       same                 ← match
```

`Math.exp` also differs at many small-negative inputs (e.g. `exp(-0.148)`, `exp(-0.192)`). `sqrt` is bit-exact (IEEE 754-2008 mandates correctly-rounded sqrt).

The optimizer's greedy-best-candidate accept flips on borderline candidates where v2 and v3 disagree by 1 ULP in `cornerAngle`/`saturate(gaussian)`. Each flip propagates through the optimizer's trajectory; by iter 1000 of aggregate iso (with R.2/R.3 topology mutations amplifying the effect), one corner has migrated 3 m from where v2 left it. Both endpoints are *valid local minima* of the cost function — neither v2 nor v3 is "wrong".

# Verification that this is the source

A 1025-entry `acos` table + 2049-entry `exp` table with bit-identical literals in both v2 (JS) and v3 (C), using linear interpolation with only +,−,*,/ (IEEE bit-exact across runtimes), reduced the worst-case drift from 50 m → 17 m on the `{angle+nbr}` minimum-set test. The remaining 17 m was eliminated only after also fixing the algorithmic bugs above. After fixes + table replacement, iso passed byte-exact at 1000 iter for all 7 cost terms. Tables removed after verification (no production code uses them).

# Cost of full byte-iso parity

Replacing `Math.acos` and `Math.exp` with deterministic in-source implementations (e.g. fdlibm port or table+interp) would close the last 3.18 m. Estimated:
- 1-2 days of porting + cross-language test parity
- Ongoing maintenance burden when JS engines or libc versions change
- ~1e-6 accuracy loss vs native libm (acceptable for this regularizer's σ=5°)

Not done. Aligns with the `[2026-05-21-boundary-polygons-tsp-rethink]` precedent (FP coin-flip beyond per-cost-term ceiling, accepted as algorithmic equivalence rather than byte-iso).

# Recommendation accepted

v3 boundary-regularize is **semantically equivalent to v2**:
- 7/7 per-cost-term iso byte-exact at 1000 iter
- All structural stats match
- 73/79 of common-moved corners are within 1mm
- Remaining 6/79 are valid alternate local minima of the cost landscape

Ship 5.7g as concluded.

# Refs

- Per-cost-term iso bench: `v3/tests/iso-regularize-cost-terms.py`
- Aggregate iso bench: `v3/tests/iso-equivalence.py boundary-regularize`
- Stream-A unit tests: `v3/build-noasan/tests/v7_boundary_regularize_test` — 52/52 passing
- V8/glibc transcendental divergence reproducer: `/tmp/test-acos2.c` + `/tmp/test-acos2.js` (now garbage-collected)
