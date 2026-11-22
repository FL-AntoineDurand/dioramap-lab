---
id: "2026-03-02-sensor-derived-minpeakcount"
date: 2026-03-02
title: "Sensor-Derived minPeakCount + Seed Plane UI Visualization"
tags: [v6, cascade, sensor-params, seed-planes, ui]
category: development
status: concluded
refs: ["2026-03-02-cascade-step-split", "2026-03-02-cascade-sensor-derived-params-todo"]
pipeline: v6
design_doc: ""
---

# Sensor-Derived minPeakCount + Seed Plane UI Visualization

## Context

After the cascade step split (2026-03-02), all internal parameters were hardcoded as constants to keep the split behavior-preserving. This caused `proc-church-large-standard` to detect only 2 of 6 planes — the tower's 4 planes (726 pts at tilt ~49.5°) were filtered by `minPeakCount = max(15, floor(15330 × 0.05)) = 766`.

## Change 1: Sensor-derived minPeakCount

Replaced the hardcoded `max(15, floor(totalPts × 0.05))` formula with `deriveMinPeakCount(density)` from `sensor-params.js` in all 3 cascade step wrappers.

| Formula | Value (density=30, 15330 pts) |
|---------|-------------------------------|
| Old: `max(15, floor(totalPts × 0.05))` | 766 |
| New: `deriveMinPeakCount(30)` = `max(3, round(3.0 × 30 × cos(50°)))` | 58 |

The tower's 726 points easily pass the 58 threshold.

### Files changed

- `cascade-shared.js`: Import `deriveMinPeakCount`, read `lidar-density` artifact in `prepareCascadeInputs`, return `derivedMinPeakCount` in state
- `normal-cascade-segment.js`: v5→v6, use `state.derivedMinPeakCount`, read `lidar-density`
- `normal-cascade-segment-kde-offset.js`: v1→v2, same
- `normal-cascade-segment-kde.js`: v1→v2, same

### Results

- **proc-church-large-standard (v6g-kde-full)**: 2p 82.1 → **6p 93.5** — all tower planes detected
- **Quick synthetics**: Small regressions on KDE-silverman pipelines (mansard: 95.7→90.7 due to more peaks found), S1 KDE pipelines improved slightly (95.7→95.9)

## Change 2: Remove maxPlanes cap

`normal-cascade-seed` had `maxPlanes=10` capping seed planes — arbitrary and harmful for complex buildings. Removed the cap (v2→v3). The cascade segmentation already controls segment count; no reason to artificially limit seeds.

## Change 3: Seed plane UI visualization

Added convex hull computation to seed plane entries (`hull`, `area`, `tilt`, `azimuth`, `inlierCount`). These are additive fields — downstream steps only use `normal`, `d`, `seedIndices` which are unchanged.

Registered `seed-planes` in the UI's `PLANE_ARTIFACT_NAMES` so clicking the artifact renders 3D plane meshes, matching the existing `planes` and `planes-lod2` rendering.

## Known issues identified

1. **Convex hull diagonal**: Seed plane hulls extend across concavities (e.g., tower footprint cutting the main roof), showing regions with no actual points.
2. **Same-tilt-offset non-contiguous planes**: Buildings with multiple bodies sharing identical roof geometry (same tilt, azimuth, offset) get merged into one segment by the cascade. Stage 3 spatial clustering should split them but may not be working for all cases. This affects 13/15 V6 pipelines that use `normal-cascade-seed`.
