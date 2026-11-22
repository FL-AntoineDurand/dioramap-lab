---
id: "2026-03-03-multi-source-edge-detection"
date: 2026-03-03
title: "Multi-Source Edge Detection System"
tags: [edges, pipeline, wall-roof, ridge, architecture]
category: development
status: concluded
refs: ["2026-03-03-immutable-artifact-refactor", "2026-03-02-cascade-step-split"]
---

## Summary

Implemented a three-source edge detection system for LiDAR roof reconstruction:
three independent pipeline steps detect edges from different signals, then a
fourth step merges them into a unified artifact with source tracking.

## Edge Sources

| Source | Step | Signal | Strength |
|--------|------|--------|----------|
| **ridge-clip** | `ridge-clip` (existing) | Plane-pair geometric intersection | Best for clean adjacent planes |
| **wall-roof** | `wall-roof-detect` (new) | Height discontinuity in 2D k-NN | Best for building footprint edges |
| **ridge-fit** | `ridge-edge-fit` (new) | Multi-source ridge candidate consensus | Best for ridges visible in point cloud |

## New Files

| File | Purpose |
|------|---------|
| `lib/geometry/edge-fitting.js` | Shared utilities: `fitLine3D`, `lineExtent`, `clusterAndFitEdges`, `mergeCollinearEdges` |
| `lib/pipeline/steps/wall-roof-detect.js` (v8) | Height discontinuity detection → dual edges (roof + ground level) |
| `lib/pipeline/steps/ridge-edge-fit.js` (v3) | Ridge candidate clustering with segment-pair awareness |
| `lib/pipeline/steps/combine-edges.js` (v1) | Merges all three sources into `edges-all` |

## Pipeline Position

```
...cascade-ridge-detect
→ ridge-edge-fit          (point-cloud ridge fitting)
→ normal-cascade-seed
→ competitive-grow / point-stealing / refit-planes
→ hull-extraction
→ wall-roof-detect        (height discontinuity detection)
→ ridge-clip              (plane-pair geometric edges)
→ combine-edges           (merge all 3 sources)
→ close-corner-gaps
...
```

All V6 pipelines include all three new steps. Non-V6 pipelines get
`wall-roof-detect` + `combine-edges` only (no `ridge-candidates` artifact).

## UI Rendering

`edges-all` artifact color-coded by source:
- Red `[1.0, 0.3, 0.3]` — ridge-clip (plane-pair geometric)
- Green `[0.3, 1.0, 0.5]` — wall-roof (height discontinuity)
- Yellow `[1.0, 0.8, 0.2]` — ridge-fit (point-cloud candidate fitting)

Individual source artifacts (`edges-wall-roof`, `edges-ridge-fit`) also viewable
as separate artifacts in the UI.

## Debugging Infrastructure

Created `scripts/dump-step-input.js` and `scripts/replay-step.js` for fast
step-level debugging without full pipeline re-runs. Dump serializes all artifacts
(including typed arrays) to `/tmp/step-snapshot-{step}.json`; replay deserializes
and re-executes the step with optional `--param` overrides.
