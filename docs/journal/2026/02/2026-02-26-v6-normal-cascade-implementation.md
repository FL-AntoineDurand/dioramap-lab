---
id: "2026-02-26-v6-normal-cascade-implementation"
date: 2026-02-26
title: "V6 Normal Cascade: Design, Implementation, and 6 Variants"
tags: [v6, normal-cascade, deterministic, orientation, offset, cascade]
category: development
status: concluded
milestone: "V6 Normal Cascade"
pipeline: v6a-base
design_doc: "docs/NORMAL-CASCADE.md"
refs: ["2026-02-26-row-houses-v6-breakthrough", "2026-02-26-cross-plan-structural-limitation"]
---

# V6 Normal Cascade: Design, Implementation, and 6 Variants

## Algorithm Design

Designed three-stage deterministic cascade: orientation (2D normal histogram) -> offset-along-normal (generalized elevation) -> XZ spatial clustering.

Key insight: `offset(p) = dot(p, meanNormal)` generalizes V3's Y-value elevation histogram to ANY plane orientation:
- Flat roof: normal=(0,1,0), offset = Y (reduces to V3 elevation histogram)
- Parapet: normal=(1,0,0), offset = X (horizontal slicing)
- Sloped: offset = distance along the slope's normal axis

## Implementation

- `normal-cascade-segment.js` (~560 lines): three-stage cascade with flat-roof aggregation, 3x3 smoothed peak detection, progressive second-pass mode, adaptive offset bins, ridge candidate detection, k-NN boundary check
- `normal-cascade-seed.js` (~170 lines): bridges cascade segments to seed-planes format for competitive-grow consumption
- Registered 2 new steps in `steps/index.js` (step count 24->26)
- Added 6 V6 pipeline definitions to `pipelines.js` (pipeline count 6->12)

## V6 Variants

| Variant | Key Feature | synth10 |
|---------|------------|:-------:|
| v6a-base | Full cascade + RANSAC fallback | 7/7 |
| v6b-ridge-aware | Ridge k-NN check, exclude ridges from seeds | 7/7 |
| v6c-adaptive | RMS-based adaptive offset bins | 7/7 |
| v6d-progressive | Two cascade passes, relaxed second pass | 7/7 |
| v6e-categorized | v6a + V3 categorize + asymmetric-steal | 7/7 |
| v6f-minimal | No RANSAC fallback, pure deterministic | 7/7 |

## Bug Fixes During Development

1. **Flat roof 0-plane bug (v6f):** Near-flat normals have degenerate azimuth -> points spread across many azimuth bins. Fix: aggregate tilt row 0 total across all azimuths; inject synthetic flat peak if total >= threshold.
2. **Mansard 0-plane bug (v6f):** 8-face mansard has ~1400 pts per face spread across ~4 histogram bins; no single bin reaches 5% threshold. Fix: 3x3 box smoothing of 2D histogram before peak detection.

## Documentation Updates

- Updated `NORMAL-CASCADE.md` status: "Implemented -- 6 variants, all 7/7 synth10"
- Updated `STEP-CATALOG.md`: added 2 entries (step count 24->26)
- Updated `PIPELINE-DEFINITIONS.md`: added V6 family section (pipeline count 6->12)
