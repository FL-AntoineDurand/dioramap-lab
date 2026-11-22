---
id: "2026-03-02-cascade-sensor-derived-params-todo"
date: 2026-03-02
title: "TODO: Re-introduce Sensor-Derived Parameters in Cascade Steps"
tags: [v6, cascade, sensor-params, experiment, todo]
category: idea
status: proposed
refs: ["2026-03-02-cascade-step-split", "2026-03-02-sensor-aware-parameter-derivation"]
pipeline: v6
design_doc: ""
---

# TODO: Re-introduce Sensor-Derived Parameters in Cascade Steps

## Context

The cascade step split (2026-03-02) hardcoded all internal parameters as constants in `cascade-shared.js` to keep the split behavior-preserving. The `sensor-params.js` derivation functions exist but are not called by the cascade steps.

## Current state (hardcoded)

| Param | Hardcoded value | Derivation function in sensor-params.js |
|-------|----------------|----------------------------------------|
| `offsetBinSize` | 0.15 m | `deriveOffsetBinSize(zNoise)` |
| `offsetTolerance` | 0.20 m | `deriveOffsetTolerance(zNoise)` |
| `minRegionPts` | 15 | `deriveMinRegionPts(density)` |
| `minSegmentPts` | 10 | `deriveMinRegionPts(density)` |
| `peakMinFrac` | 0.05 | `deriveMinPeakCount(density)` |
| `offsetPeakMinFrac` | 0.03 | derived from density |
| `kdeBwMin` | 0.05 m | `deriveOffsetKdeParams(zNoise)` |
| `kdeBwMax` | 0.30 m | `deriveOffsetKdeParams(zNoise)` |
| `kdeNoiseStd` | 0.05 m | `deriveOffsetKdeParams(zNoise)` |
| `kdeMinPeakPts` | 15 | `deriveMinRegionPts(density)` |

## Experiment plan

1. Create a new pipeline variant (e.g. `v6a-sensor-derived`) that is identical to v6a-base but reads `lidar-density` and `lidar-z-noise` artifacts in the cascade step and calls the derivation functions instead of using hardcoded values.

2. Run on the full test suite (643 cases) against v6a-base.

3. Analyze W/T/L. The derived values may differ from hardcoded ones depending on the test case's point density and noise characteristics:
   - High-density scans (>50 pts/m2): derived `minRegionPts` may be higher, possibly losing small planes
   - Low-noise scans: derived `offsetTolerance` may be tighter, possibly finding more offset peaks
   - Noisy scans: derived KDE bandwidth may be wider, possibly merging nearby peaks

4. If net positive, apply to all 15 V6 pipelines. If mixed, consider making it a per-pipeline option.

## Key risk

The hardcoded values (15 pts, 0.15 m bin, 0.20 m tolerance) were tuned on synthetic test cases which have uniform density and noise. Real IGN LiDAR HD data has variable density (4-70 pts/m2) and noise (0.02-0.10 m sigma). Sensor-derived params should help real cases more than synthetic ones.

## Dependencies

- `lidar-density` and `lidar-z-noise` artifacts must be present (written by `extract-denoise`)
- `cascade-shared.js` needs to accept an optional `sensorParams` config to override defaults
- No step version bump needed if done via a new pipeline variant; version bump required if modifying existing cascade steps
