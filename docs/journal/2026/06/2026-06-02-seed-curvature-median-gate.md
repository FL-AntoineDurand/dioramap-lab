---
id: "2026-06-02-seed-curvature-median-gate"
date: 2026-06-02
title: "Seed curvature gate: area-weighted mean → median (border-robust), the 0c3e28a regression fix"
tags: [normal-cascade-seed, curvature-gate, ts-weights, segmentation, regression-hunt, labelling, v7]
category: finding
status: concluded
refs:
  - "2026-06-01-grm-rms-weighting-and-density"
  - "2026-03-15-v7-compare-segmentation-arena"
pipeline: v7-bp2
---

## Why this entry

Follow-up to [[2026-06-01-grm-rms-weighting-and-density]]. The user kept
seeing a roof patch (a dormer-area strip) that "used to be a plane" go
missing. We traced it end-to-end and found the cause is **not** the
plane-fit weighting (the prior suspect) but the **seed curvature gate**, and
specifically the `0c3e28a` decision to make that gate **area-weighted**. The
fix is to gate on the **median** point curvature instead of the
area-weighted mean. Validated against a hand-labelled segment set on the
dahouet building.

## The trace: where the plane dies

The lost feature is **hemisphere segment 6** — a 162-pt nearly-flat strip
(tilt ~1°). Per-point owner inspection (lab-ui picker → `grm/owner` resolver)
showed it unassigned in both weighted- and unweighted-refit builds, so the
**refit weight was exonerated** (byte-identical owner either way). Adding a
compile-time trace (`V7_TRACE`, `V7_SEED_TRACE=<local idx>`; see the trace
infra note below) to `normal-cascade-seed` pinned the exact gate:

```
[SEED] seg src_id=6 n_pts=162 sum_weight=6.3893 rank=10/73
[SEED]   curvature gate: w_mean_curv=0.03093 max=0.03000 tilt=1.1 -> REJECT
```

Segment 6 fails the curvature gate by a hair. And it's the **weighting** that
tips it:

- area-WEIGHTED mean curvature = **0.03093** → REJECT
- UNWEIGHTED mean curvature   = **0.02936** → would pass
- **MEDIAN** curvature        = **0.0129**  → unambiguously planar

The high-curvature *border* points carry slightly above-average Voronoi area
(ts-weight), so area-weighting nudges the mean over `max_seed_curvature=0.03`.

## The real problem: the gate statistic, not the threshold alone

The curvature gate is genuinely needed (it removes false planes on
ridges/valleys). But the **mean is fragile**: border points have high
curvature, and the border/inner point ratio grows as a plane shrinks, so
small *legit* planes get a structurally inflated mean. Area-weighting makes
this worse (it up-weights the sparse, curvy perimeter).

Confirmed in the data — big vs small segments, mean(median_curv) is similar
but the mean is inflated for small ones; the `mean − median` gap is the tell
(seg 6: +0.018).

## Labelled evaluation (dahouet building)

The user hand-labelled every segment: roof / roof-curve / wall / BAD
(ridge-valley) / ground / tree. Goal: separate **GOOD building**
(roof+roof-curve+wall) from **BAD** (ridge/valley); ground/tree are
don't-care for now. Best-threshold sweep, walls tilt>70° exempt:

| gate | errors | good lost | bad kept |
|------|--------|-----------|----------|
| current: area-weighted mean @ 0.03 | 7 | 6, 57, 58, 39, 49 | 68, 71 |
| weighted-mean @ 0.071              | 2 | none | 68, 71 |
| **median @ 0.0655**                | 2 | none | 68, 71 |
| RMS (best @ 0.43)                  | 8 | none | all 8 BAD |

Findings:

1. **The threshold is the dominant lever.** `0.03` is far too aggressive — it
   cuts into real roofs/walls. Raised to ~0.06, every labelled good segment
   passes; only the irreducible cases leak.
2. **Median ≈ mean on this building once the threshold is right, but median is
   the robust choice** — its threshold is border-immune, so it should hold
   steadier across buildings of different size/segment-mix (the mean's
   optimum, 0.071, is itself inflated by border ratio and will drift).
3. **RMS is the WORST discriminator (8/8 errors) — the hypothesis is
   refuted.** Small BAD ridge fragments fit a plane *well* (low RMS: seg 71
   0.003, seg 68 0.033), while big GOOD roofs accumulate honest scatter (high
   RMS: seg 6 **0.106**, seg 29 0.166). RMS would re-reject seg 6 — the very
   segment we want back. Border points are resilient to RMS, yes, but the BAD
   segments don't fail on *border* curvature — they fail on whole-patch
   curvature, and they're small enough to still plane-fit.
4. **Seg 68 is irreducible by any per-segment planarity scalar** — it
   genuinely *is* a little plane (curv 0.014, RMS 0.033), but a valley
   artifact wedged between two roof planes. Rejecting it needs
   adjacency/topology, not a better scalar. One small benign false positive.

## The fix (normal-cascade-seed v5 → v6, 2026-06-02)

Gate on the **median** point curvature (unweighted), `max_seed_curvature`
default **0.03 → 0.055**. Wall exemption (tilt > 70°) unchanged. The gate no
longer reads `point_weights`.

### A/B — median@0.055 vs the prior mean@0.03 (clean batch, both unweighted-refit)

| | mean-gate | median-gate | Δ |
|---|---|---|---|
| seed points assigned | 8848 | 9128 | +280 |
| grm points assigned | 9520 | 9808 | +288 |
| distinct grm planes | 43 | 51 | +8 |
| **points lost** | — | — | **0** |

- **Segment 6 recovered**: 141/162 pts now own GRM plane 10 (was all −1).
- Newly-seeded segments match the GOOD labels: 6, 58 (roof), 39, 49 (wall).
  The rest are don't-care ground/tree. **Zero points lost** — pure recovery.
- Seg 68 unchanged (already passed; still does).

## Conclusions

- The "feels worse / lost plane" regression is the **`0c3e28a` curvature-gate
  area-weighting**, not the plane-fit weighting (which is exonerated and is
  probably correct to leave unweighted on its own merits — see
  [[2026-06-01-grm-rms-weighting-and-density]]).
- **Median curvature** gate @ 0.055 recovers all labelled good segments with
  zero loss; **don't use RMS** as a planarity gate (inverted here).
- Residual false positive (seg 68) is a topology problem for a later pass.

## Trace infrastructure (new, reusable)

`steps/v7/v7-trace.h` — compile-time-gated stderr traces (`V7_TRACEF`,
`V7_TRACE_IF`); `((void)0)` (via unevaluated `sizeof`, args stay
type-checked) unless built `-DDIORAMAP_V7_TRACE=ON`. The old `POLY_TRACE` was
migrated onto it. Seed gates are instrumented; trace one segment with
`V7_SEED_TRACE=<per-building local point index>`.

## Refs / artifacts

- Code: `v3/steps/v7/v7-normal-cascade-seed-impl.c` (median gate +
  `cmp_float_asc`), `v3/steps/v7/v7-normal-cascade-seed.c` (.version 6,
  default 0.055), `v3/steps/v7/v7-trace.h`.
- Picker: `apps/lab-ui/src/viewer/point-picker.js` (full hit-stack +
  visible/HIDDEN + clipboard), `renderers/_helpers/categorical-cloud.js`
  (`grm/owner` → global id + owner-plane resolver).
