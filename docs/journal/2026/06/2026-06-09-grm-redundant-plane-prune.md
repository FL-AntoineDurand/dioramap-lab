---
id: "2026-06-09-grm-redundant-plane-prune"
date: 2026-06-09
title: "The real problem was spurious PLANES, not the search radius: a GRM redundancy prune"
tags: [grow-refit-merge, segmentation, redundant-planes, search-radius, seed-gate, v7]
category: development
status: concluded
refs:
  - "2026-06-02-ts-weights-dimensional-audit"
  - "2026-06-02-seed-curvature-median-gate"
pipeline: v7-bp2
---

## The real problem

A small junk plane kept appearing in roof valleys / at plane junctions (the
"seg 68" class). We spent a long detour on the **GRM connectivity search
radius** — and learned a lot about its dimensions (see
[[2026-06-02-ts-weights-dimensional-audit]]) — but the radius was the **wrong
lever**. Uniform radius "fixed" it only by luck: it happened to starve the
spurious seed's growth. The actual problem is **point assignment / spurious
plane identification**, and it needs a real gate.

## Why these planes exist, and why size + curvature can't catch them

The spurious plane (`seg 68`: a flat patch wedged in a valley) **passes every
intrinsic gate**:

- size: 22 pts ≥ 3 (seed) and ≥ 1.0 m² (hemisphere) — **pass**;
- median curvature: **0.0143** ≪ 0.055 (it's genuinely flat) — **pass**.

There is no per-segment scalar — size, area, curvature — that separates "a
small *legit* plane" from "a small *spurious* plane", because geometrically
they're identical: both small and flat.

## Why the spurious plane WINS the contested points (fit, not reach)

GRM growth lets several planes *claim* a point (reach + inlier within
`dist_threshold`), then a mini-resolve assigns it to the plane it fits **best**
(lowest residual). The spurious plane wins because it fits those points
**tighter** — almost tautologically: it was *fitted to those very points*. A
tight local plane beats a loose global one on residual, **every time it gets
to exist**. So the real neighbour reaching the points is useless; it loses the
residual contest. The fix is not to help the neighbour win — it's to stop the
spurious plane from **existing** as a competitor.

## The gate: redundancy prune (the signal is relational, not intrinsic)

Spuriousness is **relational** — a redundant plane is one a neighbour explains
*almost as well*. Per plane, over its owned points:

```
own_RMS = RMS distance of the points to THIS plane
alt_RMS = RMS distance of the points to their next-best CLAIMING plane
redundancy = (alt_RMS − own_RMS) / dist_threshold      ← tombstone if < ~0.5
```

In plain English: a plane only earns its existence if it explains its points
clearly better than the next plane (by more than ~half the inlier tolerance,
≈6 cm). If a neighbour is almost as good, dissolve it — its points fall to
their next-best owner. This reuses the existing `dist_threshold` as the scale
(no new magic number) and is **size-independent**: a 3-pt real detail with
100% exclusive points survives; a 28-pt junk plane at 7% exclusive dies.

**Validation (own-RMS vs next-claiming RMS, dahouet):** the two known-spurious
planes are the two *smallest* margins in the whole building — plane 47 (seg 68)
`redundancy = 0.03`, plane 46 (seg 58) `0.42` — vs every labelled-legit plane
≥ 0.78.

### Where it lives

GRM is a convergence loop (mini-resolve → grow → refit → **coplanar-merge**),
and the merge phase already tombstones planes whose points re-resolve next
iteration. The prune is a **sibling of merge**, in-loop, **skipping iter 0**
(a real plane mid-growth can transiently look redundant before claiming its
exclusive points). `v7-grow-refit-merge` `.version` 10→11; params
`redundancy_tau = 0.5`, `redundancy_far = 1.0` (0 disables). The alt residual
is computed on-demand from each owned point's other claiming planes — no
persistent 2nd-best array.

## It is COMPLEMENTARY to the curvature gate, not a replacement

| | curvature gate (seed) | redundancy prune (GRM) |
|---|---|---|
| catches | **non-planar** junk (ridges, noise) — even isolated | **planar duplicates** a neighbour already explains |
| seg 68? | misses (flat) | **catches** |
| isolated curvy ridge? | **catches** | misses (no neighbour to compare) |

Keep both: curvature stops non-planar seeds *before* they grow; redundancy
stops planar duplicates *after* the competition exists.

## Result — per-seed-plane fate (dahouet, labelled)

Prune fired **exactly twice** — planes 47 (seg 68 sliver) and 46 (seg 58) —
**zero legit planes harmed**. Distinct planes 50 → 48.

```
 label    | killed by
 ---------+-----------------------------------------------------------
 roof     | survives: 6 23 26 29 35 36 37 ; merge-pair 51+57 ; absorbed 44 52
 roof     | CURVATURE: 57 (mislabelled-curvy, med 0.0655) ; REDUNDANCY: 58 (→52)
 roofcurv | survives: 7 9 10 11
 wall     | survives: 21 22 34 47 49 55 ; merge-pairs 15+43, 20+60 ; absorbed 39 42
 BAD      | CURVATURE: 8 12 13 14 16 61 ; growth/redundancy: 68 (sliver plane 47), 71
```

(seg 68 reads "absorbed" at the *segment* level because its points live in the
multi-segment Frankenstein plane 47 — which is exactly what the redundancy
prune dissolved.)

## Search radius — settled

Reverted to the dimensionally-correct `scale · max(√w, 1/√density)` (adaptive
local spacing, floored at global spacing). The radius no longer has to "starve
spurious seeds" — the redundancy prune handles them — so it can stay the
principled form.

## Open / next

- Threshold `tau = 0.5` is calibrated on one building (cleanly separates 0.03,
  0.42 from ≥0.78). Validate across more buildings before locking.
- `redundancy_far = 1.0 m` only affects fully-exclusive planes (keeps them);
  insensitive as long as it's ≫ dist_threshold.
- RMS aggregation is fine here (no border effect, unlike curvature). Median is
  an option if a building shows outlier sensitivity.
