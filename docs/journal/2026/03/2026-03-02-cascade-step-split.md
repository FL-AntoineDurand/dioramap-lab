---
id: "2026-03-02-cascade-step-split"
date: 2026-03-02
title: "Split normal-cascade-segment into 3 Mode-Specific Steps + Dedicated Ridge Detection"
tags: [v6, cascade, refactor, architecture, ridge]
category: development
status: concluded
refs: ["2026-03-02-v6g-kde-pipeline-variants", "2026-03-02-sensor-aware-parameter-derivation"]
pipeline: v6
design_doc: ""
---

# Split normal-cascade-segment into 3 Mode-Specific Steps + Dedicated Ridge Detection

## Motivation

The `normal-cascade-segment` monolith had grown to 1027 lines with 31 parameters covering 4 responsibilities: histogram Stage 1, KDE Stage 1, Stage 2 (histogram or KDE), Stage 3 spatial clustering, and ridge detection. Adding KDE variants required parameter-switching within one step, making it hard to reason about which params affect which code paths.

## Architecture

Split into:

```
Step wrappers (~40 lines each)           <- only expose key tuning params
  | call
cascade-shared.js (~920 lines)           <- shared cascade orchestration
  | call
lib/geometry/histogram.js                <- generic KDE/histogram primitives
```

### New file structure

| File | Role | Version |
|------|------|---------|
| `normal-cascade-segment.js` | Histogram S1 + Histogram S2 wrapper | v5 (unchanged) |
| `normal-cascade-segment-kde-offset.js` | Histogram S1 + KDE S2 wrapper | v1 |
| `normal-cascade-segment-kde.js` | KDE S1 + KDE S2 wrapper | v1 |
| `cascade-ridge-detect.js` | Dedicated ridge detection (3 methods) | v1 |
| `cascade-shared.js` | Shared orchestration module | n/a |

### Pipeline mapping

| Cascade step | Stage 1 | Stage 2 | Pipelines |
|------|---------|---------|-----------|
| `normal-cascade-segment` | Histogram | Histogram | v6a, v6b, v6c, v6d, v6e, v6f, v6x (7) |
| `normal-cascade-segment-kde-offset` | Histogram | KDE | v6g-kde-silverman, -iqr, -physical, -adaptive, -posterior (5) |
| `normal-cascade-segment-kde` | KDE | KDE | v6g-kde-s1-2d, -s1-physical, -full (3) |

All 15 V6 pipelines have `cascade-ridge-detect` inserted between cascade segment and cascade seed.

### Param reduction

Each wrapper exposes only 3-5 params vs the original 31. Internal constants that never varied across pipelines (nearHorizThreshold, adaptiveBinRmsMult, spatial cell size, etc.) are hardcoded in `cascade-shared.js`.

## Regression Found and Fixed

During cleanup, 5 params were accidentally removed from pipeline defaults that are consumed by **non-cascade** steps:

| Param | Value | Consumed by |
|-------|-------|-------------|
| `normalK` | 15 | compute-normals |
| `distThreshold` | 0.15 | competitive-grow, ransac-seed |
| `cascadeMinSeedPts` | 15 | cascade-seed |
| `growK` | 15 | competitive-grow |
| `minRegionPts` | 15 | cascade-seed, ransac-seed |

Most damaging: removing `normalK: 15` caused compute-normals to auto-derive k=24, producing fundamentally different normals. v6c-adaptive on multi_section-clean dropped from 94.1 to ~51 (3 planes instead of 4).

Root cause debugging: ran the original monolith on identical inputs and confirmed cascade code was correct -- the regression was entirely from the missing pipeline defaults.

## Intentional azimuth KDE fix

The KDE S1 code path was improved: replaced manual triple-padding + `gaussianKDE` + `findKDEPeaks` for azimuth wrap-around with dedicated `circularKDE` + `findCircularKDEPeaks` functions. This fixes edge cases at the 0/360 degree boundary.

S1 KDE pipelines show small score differences (max 0.4 on one case) with identical plane/edge counts. This is expected and intentional.

## Verification

Compared all 15 V6 pipelines on 7 quick test cases against baseline (run 9, git 492f4b9):

- **Histogram pipelines (7)**: 44/49 exact match, 5 tiny float diffs (max 0.05)
- **S2-only KDE pipelines (5)**: 35/35 exact match
- **S1 KDE pipelines (3)**: 18 diffs from intentional azimuth fix, all same plane/edge counts
- **All 105 test cases**: identical plane and edge counts

## Deviation from plan

The plan specified that cascade-internal params (offset tolerance, bin size, min peak pts, etc.) would be **auto-derived from sensor artifacts** (density, z-noise). In practice, they are **hardcoded as constants** matching the original monolith defaults. Switching to derived values changes behavior and breaks the "behavior-preserving split" requirement. Sensor-derived parameters are tracked as a separate follow-up experiment.
