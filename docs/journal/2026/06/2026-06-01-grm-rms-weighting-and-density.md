---
id: "2026-06-01-grm-rms-weighting-and-density"
date: 2026-06-01
title: "GRM plane-fit: RMS-weight leak fix (geometric gates) + the forced-density experiment"
tags: [grow-refit-merge, density-compensation, ts-weights, rms, plane-fit, regression-hunt, v7]
category: finding
status: concluded
refs:
  - "2026-03-15-v7-compare-segmentation-arena"
  - "2026-03-15-ransac-density-compensation-revisited"
  - "2026-05-30-bp3-contour-arclength-ordering"
pipeline: v7-bp2
---

## Why this entry

User reported v3 pipeline output "feels a bit worse than it used to" and
suspected a param/threshold. Two changes were in the frame: (1) the
2026-05-28 commit `0c3e28a` "unify plane-fit weighting — area-weighted PCA
everywhere", which wired `ts-weights` density weights into the core
segmentation (normal-cascade-seed, grow-refit-merge, density-voids-2,
boundary-signals); and (2) the v3 switch from **full-tile** density to
**per-building footprint** density (`v7-derive-thresholds`). This entry
records the audit, one fix that landed, and two A/B experiments — both of
which **ruled their suspect out** on dahouet.

## Recap: density compensation is SELECTIVE

The governing rule ([[2026-03-15-v7-compare-segmentation-arena]] §4.0,
[[2026-03-15-ransac-density-compensation-revisited]]): the per-point weight
`w = π·d_k²/k` is the **Voronoi cell area** a point represents. Weighting is
**correct** for physical-area estimation and spatial bridging — LS refit,
CC connectivity, size gates, histograms/KDE — and **wrong** for hypothesis
generation (RANSAC sampling) and agreement counting (RANSAC scoring / model
comparison), because the ~180× weight variance lets sparse, noisy swath-edge
points dominate.

Audit verdict on `0c3e28a`: the mass-wiring **follows** the rule. RANSAC in
density-voids-2 stays uniform/count-based (`v7_ransac_plane` takes no
weights); only the post-RANSAC LS refit is weighted. Seed sort is by
weighted area (endorsed); seed inlier counting is raw. boundary-signals
weights the density raster (area estimation) and even calibrates the
nearby-weight to the mean own-weight to neutralise the magnitude bias. So no
gross violation.

## The one real leak: RMS-weighting in the GRM gates

`v7_plane_fit_lsq` returns `rms = sqrt(smallest eigenvalue of the WEIGHTED
covariance)` — a **weighted** residual. GRM's grow loop has two
accept/reject gates that compared this weighted `fit.rms`:

- **degradation gate** (`if fit.rms > plane_rms + max_degradation` revert) —
  detects a plane bleeding into a neighbouring face during growth.
- **coplanar-merge gate** (`if ufit.rms > max(rms_i,rms_j)+max_degradation`
  reject merge).

Two problems: (a) `plane_rms`'s **initial** value (`impl.c:447`) is the
UNWEIGHTED geometric RMS `sqrt(Σdist²/n)`, so on the first iteration the gate
compared weighted-vs-unweighted; (b) `max_degradation = 0.04` is calibrated
as `2·σ_z` in real metres, which only the geometric RMS reports.

**The deeper objection (user's, and it's the right one):** the weight is a
*tangential area* (how much roof a point covers); RMS is a *perpendicular
deviation* (how far off-plane). Using one to scale the other is a category
error for judging fit quality. Worse, the inverse-density weight up-weights
sparse swath-edge points — the *noisiest* ones — so it biases a contamination
test by local density (and is backwards as a reliability weight). The fit
should be weighted (de-biases the sampling when estimating tilt/offset); the
gate's residual readout should not.

### Fix (v7-grow-refit-merge v5 → v6, 2026-06-01)

Added `geom_rms_plane()` = `sqrt(Σ perpendicular_dist²/n)`. Both gates and
the reported `hdr.rms` now use it; the plane FIT stays area-weighted.
`plane_rms` is geometric end-to-end, gates compare like-with-like, and
`max_degradation` means real metres again. Step `.version` 5→6.

## Method note: the content-addressed cache made the A/Bs trivial

The v3 cache is a Merkle DAG, not a version chain:
`identity = xxh3(name, version, params, sorted_input_content_hashes)`
(`runner.c:368`). A step recomputes only when its input *bytes* change, and
the cascade stops the instant a step re-emits identical output. This let us
isolate each change by diffing one artifact (`grm-owner`) between two builds
with a fresh `--cache-dir`.

## Experiment 1 — gate RMS (weighted vs geometric): INERT on dahouet

`grm-owner` v5 (weighted gate) vs v6 (geometric gate): **byte-identical**
(sha256 match). The gate only *fires* when a plane is borderline-contaminated;
dahouet's clean roofs never get near `max_degradation`, so both metrics make
the same decisions. The only byte-change v6 makes is the reported `rms` field
(why `boundary-topology` recomputed — it reads an rms-bearing record — then
emitted identical output, so everything downstream correctly cached).

**The gate-RMS fix is correct in principle but a no-op on clean roofs** — kept
as insurance for dirtier/denser scenes; not the regression.

## Experiment 2 — per-building density (19.87) vs hardcoded 30: REAL but invisible

`v7-derive-thresholds` density on dahouet bldg0 (16092 pts) =
**19.872 pts/m²** (spacing 0.224 m), vs the user's hypothesised full-tile
~30. Density enters only as `1/√density`, so 30 vs 19.87 shrinks every
density-scaled gate ~18% (GRM search radius, topology spacing, contour
cell-size) — i.e. **tighter**.

Temporarily hardcoded `rec.density = 30.0` (derive-thresholds v1→v2, since
reverted). `grm-owner` diff vs the real 19.87:

- **486 / 16100 points (3.0%) reassigned.** Breakdown: 294 assigned→unassigned,
  74 unassigned→assigned, 118 plane→plane (much of it local-index renumbering,
  e.g. a 69-pt "plane 49→50" is one re-indexed face).
- Net **+220 points → unassigned**; every real face shrinks at the edges
  (plane 0 the dominant face: −97). Tighter radius ⇒ fewer claims ⇒ more
  unassigned, concentrated on face edges.

**But the user reports NO visual change in the lab UI.** The 3% shift is
edge-only and doesn't move the visible roof-graph. So **per-building density
is not the regression either.** (It is still arguably mis-calibrated — the
density-scaled constants were tuned when density was tile-wide ~30; if a
regression ever traces here, the fix is to recalibrate those constants for
the correct lower per-building density, not to fake a tile number.)

## Conclusions

- **Neither suspect is the dahouet "feels worse" regression.** Gate-RMS
  weighting is inert here (0% owner change); per-building density moves 3% of
  owner labels but only at face edges, with no visible effect.
- Both changes are nonetheless *real*: the gate-RMS fix (v6) is the correct
  design and will matter on contaminated/denser buildings; the density
  question is a live calibration concern.
- **Next suspect: the weighted FIT itself (`0c3e28a`).** Unlike gates/density
  (which only change *which points are claimed*), the weighted least-squares
  fit changes each plane's **tilt and offset** — the kind of change that moves
  visible geometry (corners, ridges, regularized outline). A/B #1: rebuild
  with the fit forced to uniform weights (≈ pre-0c3e28a) and compare owner +
  visually.
- Side cleanup: the 4 `doc.json` sidecars for the `0c3e28a`-wired steps were
  stale (said "passes NULL / wiring quirk"); corrected to reflect ts-weights
  is wired + required, with the gate-RMS caveat now marked FIXED-in-v6.

## Refs / artifacts

- Code: `v3/steps/v7/v7-grow-refit-merge-impl.c` (`geom_rms_plane`, gates),
  `v3/steps/v7/v7-grow-refit-merge.c` (.version 6).
- Density finding saved to project memory (`finding_v3_perbuilding_density`).
- Read the density yourself: the lab-ui JSON inspector now decodes raw
  ARRAY_INLINE records as F64 lanes (`metadata-json.js`) — bldg-thresholds
  shows `f64: [19.872, 0.224, 0.02, 0.09, 0.12, 0]`.
