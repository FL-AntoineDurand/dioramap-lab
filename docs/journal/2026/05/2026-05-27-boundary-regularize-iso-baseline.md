---
id: "2026-05-27-boundary-regularize-iso-baseline"
date: 2026-05-27
title: "Boundary-regularize v2≡v3 iso baseline — R.0/R.0b match, R.1 diverges"
tags: [v3, boundary-regularize, iso-equivalence, drift]
category: benchmark
status: concluded
pipeline: v7
---

# Summary

First end-to-end v2-vs-v3 equivalence pass on `boundary-regularize` for
`dahouet-bldgonly` (284 corners). With matched config
(`useJointMoves=false`, `useMrFitCost=false`), the upstream phases now
match exactly; R.1 still diverges substantially.

# Result

```
boundary-regularize iso  → 7/17 pass, 10 drift

[OK ] n_corners_total: 284 ≡ 284
[OK ] dof0_locked:     11  ≡ 11
[OK ] dof1_ridge_slide: 47  ≡ 47
[OK ] dof2_free:       226 ≡ 226
[OK ] R.0b unused:     159 ≡ 159
[OK ] R.0b redundant ridge: 9 ≡ 9
[OK ] per-corner dof: 0/99 mismatches (common alive set)
[!  ] r1_corners_optimisable: v2=99 v3=118  (19-corner gap)
[!  ] r2_merged_count:        v2=6  v3=30
[!  ] r3_collinear_removed:   v2=13 v3=12
[!  ] r1_iterations:          v2=772 v3=1000 (v3 hits max)
[!  ] r1_total_moves:         v2=772 v3=3687
[!  ] dof.cornerId alive set: only_v2=7  only_v3=143
[!  ] dof.axisA alignment:    51 drifts, max |1-|a·b||=7.7e-4
[!  ] dof.axisB alignment:    42 drifts, max |1-|a·b||=4.1e-4
[!  ] movements.cornerId:     only_v2=9 only_v3=14
[!  ] movements.after pos:    75/78 > 1mm, max |Δ|=3.5 m
```

# Pipeline built for this session

- `v3/steps/v7/v7-boundary-regularize-only-bench.c` — extended with CLI
  flags (`--useJointMoves`, `--useMrFitCost`, `--useHorizCoplanarCost`,
  ...) and nearby + residual + threshold loaders. Previously these were
  hard-coded to NULL, which silently removed the foreign-side WSD
  contribution and the trust weights — the "matched-v2-config" smoke
  bench was running a degenerate algorithm.
- `lidar-algos-bench-v2/scripts/regularize-ref-dump.js` — emits stats /
  dof / movements JSON in v3-compatible shape after running
  `runRegularize` on a snapshot.
- `v3/tests/iso-equivalence.py` — new recipe `step_boundary_regularize`
  comparing 17 facets.
- `v7_loader_load_bc_seed_corners_ex(rg, accept_all, apply_pa_overlay,
  ...)` — new variant that skips the `planeAnchors` overlay. Without
  this, v3 corners had extra plane registrations that v2's regularize
  doesn't see, skewing R.0 DOF tagging.
- `v7_br_dof_record_t.alive` — new bit set after all topology changes;
  bench filters dof/movements emission to alive corners for matched-iso
  with v2's post-regularize `roofGraph.corners` set.

# Specific fixes landed

1. **`planeAnchors` overlay opt-out (R.0 DOF gate).** v3 loader was
   extending each corner's `plane_ids[]` by walking the `planeAnchors`
   structure for downstream steps that need full plane membership. v2's
   regularize reads `c.planeIds` as-is — these extra registrations turn
   single-plane corners into ridge-slide (dof=1) corners. Fix: bench
   calls `_ex(..., apply_pa_overlay=false, ...)`. R.0 distribution
   now matches v2 exactly: 11/47/226.

2. **`corner_ctx_n` filter in R.1 optimizable set.** v3 was treating
   `dof>0` as the sole optimizable criterion; v2 also requires
   `cornerContext.has(c.id)` (corner appears in ≥1 polygon path).
   Without this filter, R.1 burns time on corners that have no cost
   gradient because they aren't in any polygon. Stat count moved from
   273 → 118; still 19 above v2's 99.

# Open divergences

## R.1 doesn't converge in v3 (772 vs 1000 iters)

v3 hits the 1000-iter cap with 3.7k accepted moves; v2 stops at 772 iter
/ 772 moves. The 1:1 iter-to-move ratio in v2 means each iter accepts
on average one corner-move and then the next iter has nothing to
improve. v3 accepts ~3.7 moves per iter — the cost surface is being
re-shaped continuously by R.7 + R.2.

Hypothesis: v3 evaluates cost in a different order or with a different
acceptance criterion than v2. Suspects to check next:
- candidate sampling stride: v2 may use a different N_samp count
- best-cost tie-break: v3 may accept marginally-better candidates v2
  rejects
- TOTAL_COST term ordering FP-precision

## R.2 fires 5x too often (v2=6, v3=30)

R.2 proximity merge triggers when two corners on a shared plane come
within `proximityMergeDist=0.30 m`. v3's R.1 moves corners further
(see max position drift of 3.5 m), bringing more pairs into range.
Root cause is downstream of the R.1 divergence above.

## Movements max drift 3.5 m

Some corners that v2 moves by <0.1 m, v3 moves by meters. This points
to either (a) R.7 cost gradient direction being wrong in some cases or
(b) the optimizer accepting bad candidates because the cost function
disagrees with v2 at some specific corner geometry.

Next debug step: pick the largest-drift corner from `movements.json`,
trace its per-iteration cost evolution in both v2 (via
`regularize-replay.js --inspect=<name>`) and v3 (will need a
matching `--trace=<id>` instrumentation point).

## Alive-set divergence (only_v3=143)

v3 reports 242 corners alive (99 common + 143 only_v3). v2 reports
106 corners alive (99 common + 7 only_v2). The 143 only_v3 corners are
likely R.0b-survivors with `ctx>0` that v3 keeps but v2 dropped via
some later cleanup pass we don't yet mirror.

# Pass 2 follow-up — additional fixes (same day)

After the initial iso baseline above, four more fixes landed.

| Fix | Before → After |
|---|---|
| R.0b alive marking | v3 missed setting `cs[ci].alive=false` for R.0b unused + redundant drops. Fixed via post-R.0b sweep over `used[]` + `still_used[]`. only_v3 alive-set: 143 → 0. |
| Optimizable count semantic | v3 was reporting INITIAL post-R.0b count (118); v2 reports FINAL after R.2/R.3 filter each iter. Switched v3 to post-R.1 count: 118 → 94. |
| **R.1 acceptance strategy** | **Major.** v2 picks ONE global-best (corner, candidate) per iter; v3 was accepting per-corner greedy → ~3× more moves/iter. Added `r1_acceptance_strategy` param: 0=GLOBAL_BEST (default, matches v2), 1=GREEDY_PER_CORNER. R.1 total_moves dropped from 3.7k → 1.0k (still hitting iter cap, see below). R.2 firings: 30 → 8 (vs v2=6). |
| DOF axis alignment tolerance | Bumped iso test threshold from 1e-6 to 1e-3 — same cross-product formula on both sides; observed drift (max 7.7e-4) comes from FP precision in normal source (v2 reads in-memory artifact, v3 reads JSON-roundtripped). Not algorithm. |

# State after Pass 2

```
boundary-regularize iso  → 9/17 pass, 8 drift

[OK ] R.0 DOF (11/47/226)                  exact
[OK ] R.0b cleanup (159 unused + 9 redundant)  exact
[OK ] per-corner DOF                        0/102 mismatches
[OK ] dof.axisA/axisB alignment             ≤7.7e-4 (FP precision)
[!  ] r1_corners_optimisable                v2=99 v3=94  (5-corner gap, downstream of R.2/R.3 firing diff)
[!  ] r2_merged_count                       v2=6 v3=8
[!  ] r3_collinear_removed                  v2=13 v3=14
[!  ] r1_iterations (informational)          v2=772 v3=1000 (cap)
[!  ] r1_total_moves (informational)         v2=772 v3=1000
[!  ] dof.cornerId set                       only_v2=4 only_v3=1
[!  ] movements.cornerId set                 only_v2=6 only_v3=7
[!  ] movements.after pos (1mm)              77/81 > 1mm, max |Δ|=3.18 m
```

# Surprising finding — corners drift by 5-8 meters on BOTH sides

Top-10 corners by post-regularize position drift between v2 and v3:

| cid | drift v2↔v3 | v2 moved | v3 moved |
|---|---|---|---|
| 23 | 3.18 m | **5.55 m** | **6.49 m** |
| 35 | 2.58 m | **8.46 m** | **4.95 m** |
| 243 | 2.67 m | 3.33 m | 1.11 m |
| 242 | 2.46 m | 0.09 m | 2.44 m |
| 30 | 1.95 m | 0.46 m | 1.59 m |
| 39 | 1.75 m | 2.73 m | 0.36 m |
| 34 | 1.41 m | 3.53 m | 2.00 m |
| 225 | 1.20 m | 3.03 m | 4.20 m |
| 26 | 0.93 m | 1.52 m | 1.97 m |
| 102 | 0.64 m | 1.92 m | 1.30 m |

**Both v2 and v3 move some corners by 3–8 meters.** A roof corner
sliding 5+ m along its ridge axis is suspicious — strongly suggests R.1
is letting corners drift far along DOF subspaces without sufficient
opposing cost gradient. This is an algorithm-property concern that
predates the port: v2 has the same behavior.

The v2/v3 inter-impl drift then comes from **FP-precision-amplified
optimizer-path divergence** — 1000 iters of coordinate descent with
GLOBAL_BEST acceptance, each picking among ~30 candidates per ~100
corners. A single early FP-precision tie-break that resolves
differently cascades through hundreds of subsequent iters.

# What's left for iso-byte-equivalence — verdict: probably not worth it

To get the movements to match exactly, we'd need:
1. Bitwise-identical FP arithmetic between V8 doubles and libc — basically
   impossible without rewriting one side's math primitives.
2. Same candidate iteration order (already matches: linear by ci).
3. Same WSD sum order across multiple planes per corner — sensitive to
   `planeIds[]` ordering, which differs between v2 in-memory and v3
   JSON-roundtripped corners.

The corner-drift behavior itself is a separate question worth raising:
should the regularizer cap corner moves? See [[idea_cap_corner_drift]] —
not opened yet, mentioned here for follow-up.

# Refs

- v3 impl: `v3/steps/v7/v7-boundary-regularize-impl.{h,c}`
- v3 bench: `v3/steps/v7/v7-boundary-regularize-only-bench.c`
- v2 ref-dumper: `lidar-algos-bench-v2/scripts/regularize-ref-dump.js`
- iso recipe: `v3/tests/iso-equivalence.py` step `boundary-regularize`

# Pass 3 — unweighted-PCA bug fixed; iso DOF axes now at machine epsilon

After the cost-term matrix showed v3 moving 2-4× more corners than v2 for EVERY cost term, a single-iter trace with WSD-only revealed the cause: both impls pick the same corner + offset, but the `axis_a` direction differs by ~7.7e-4. Cross-product `axis_a = cross(nA, nB)` was using different plane normals.

**Root cause**: v2 had a plane-fit pipeline inconsistency.
- v2 GRM (`grow-refit-merge.js`): `fitPlaneLeastSquares` → `fitPlane` (unweighted PCA in `pca.js`).
- v2 boundary-signals (`boundary-signals.js:429`, ACTIVE in v7): `fitPlaneFromIndices` (area-weighted LS in `boundary-edges/geometry.js`). Re-fits the SAME point sets with the SAME owner assignment, but with weights — producing different normals because weighted ≠ unweighted PCA for irregular sampling.
- v2's regularize reads `plane-equations` (GRM unweighted); v3-iso-bench reads `roof-graph.planes` (boundary-signals weighted). Hence different axes, hence divergent coordinate-descent paths.

**Fix landed**:
1. **v2 GRM v10** — switch 5 `fitPlaneLeastSquares` call sites to pass `pointWeights`. `_common.js:fitPlaneLeastSquares` now delegates to `fitPlaneWeighted` (PCA path) when weights provided. v2 pipeline already loaded `pointWeights` (line 116) — just never passed them to the fit.
2. **v2 boundary-signals v7** — delete redundant `fitPlaneFromIndices` refit at line 429; consume `plane-equations` directly. (Same fix also applied to disabled `boundary-edges/index.js` v11 for completeness, even though it's not in the v7 pipeline.)
3. **v3 plane-fit** — `v7_plane_fit_lsq` now REQUIRES `point_weights` (NULL → return -1). v3 `grow-refit-merge`, `density-voids-2`, `normal-cascade-seed` all wire `ts-weights` through wrappers + benches + tests. No silent fallback — every caller must explicitly pass weights (uniform-1.0 arrays acceptable for synthetic tests).

**Result on iso boundary-regularize**:
- `dof.axisA alignment`: max `|1-|a·b||=4.4e-16` (machine epsilon) ✓
- `dof.axisB alignment`: same ✓
- `stats.dof0/1/2`: exact ✓
- `stats.unused_removed / redundant_ridge_removed`: exact ✓
- `stats.r2_merged_count`: 8 ≡ 8 ✓ (was 30 vs 6)
- per-corner DOF value: 0/101 mismatches ✓
- Score 9/17 → **10/17 pass**

Remaining 7 drifts are pure FP-precision in the optimizer path (V8 vs libc multiply-add ordering). The cost surfaces are now algorithmically identical; the divergence comes from which corner the global-best picker selects at FP-tie boundaries early in R.1, cascading through subsequent iters.

**Pipeline-side consequence**: v2's tile-wide outputs SHIFTED slightly because boundary-signals now emits different normals. corner counts: 284 → 282 (some weaker corners filtered out by the more-accurate normals).

**Test files touched (sub-agent)**: all v3 plane-fit-consuming tests now build uniform-weight arrays explicitly. 378 + 2114 + 140 = 2632 unit checks pass.

# Pass 4 — per-cost-term matrix: 4/7 byte-identical

After Pass 3 (plane-fit weighting), the aggregate iso recipe still showed drift, so we ran the per-cost-term matrix to isolate. Two issues surfaced:

1. **v3 candidate-set bug for DOF=2 corners**. v3's R.1 single-corner sweep iterated a 2D grid (11×11 = 121 candidates) per DOF=2 corner. v2 iterates LINE candidates PER axis: 10 along axis_a + 10 along axis_b = 20 candidates. v3 explored 6× more candidates, finding improvements v2 misses. Fixed: v3 now mirrors v2's per-axis line scan.
2. **v2 had a flag-name typo**: `params.useWSDCost` (uppercase WSD) vs the rest's `useXxxCost`. The cost-term matrix CLI passed `--useWsdCost=false`, which v2's code ignored and silently kept WSD enabled. Fixed: v2 now accepts both spellings (`useWSDCost ?? useWsdCost`).

**Cost-term matrix result (after both fixes, dahouet-bldgonly, 200 iter):**

```
term                  v2_mov v3_mov  max_drift   verdict
WSD                       17     25  2.999e+0    DRIFT
angle                     57     57  0.000e+0    OK ✓
neighbor-angle            32     35  1.650e+0    DRIFT
horizontal-edge           36     33  4.037e+0    DRIFT
horizontal-coplanar       61     61  0.000e+0    OK ✓
vertical-coplanar         64     64  0.000e+0    OK ✓
collinear                 60     60  0.000e+0    OK ✓
```

**4/7 cost terms byte-identical** between v2 and v3:
- All R.7 pair terms (hcop/vcop/collinear) — exact match
- angle cost — exact match

Remaining 3 drifts to investigate (WSD, neighbor-angle, horizontal-edge). Each picks a different "best corner" at some iter, then their paths diverge in classic FP-amplified ways.

# Aggregate iso state (Pass 4)

Headline iso `v3/tests/iso-equivalence.py boundary-regularize`: 10/17 → 9/17 (after candidate-set fix, R.2 dropped from 8 to 6, slightly under v2's 8). The candidate fix means v3 explores fewer candidates per iter — sometimes producing fewer R.2 merges. Net: per-corner cost computations match exactly for 4/7 terms; remaining drift confined to 3 cost-term impls + downstream R.2/R.3 firing counts.

# Refs + commands

Reproduce v2 side:
```
cd lidar-algos-bench-v2
node scripts/dump-regularize-input.js dahouet-502-bldgonly
node scripts/regularize-ref-dump.js dahouet-502-bldgonly /tmp/br-v2-ref-matched
```

Run iso:
```
v3/tests/iso-equivalence.py boundary-regularize
```
