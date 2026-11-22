---
id: "2026-03-10-building-rms-edge-type-metrics"
date: 2026-03-10
title: "Building RMS and edge type accuracy metrics"
tags: [metrics, building-rms, edge-type, ui]
category: development
status: concluded
refs: ["2026-03-07-scorer-v3-23-metrics", "2026-03-04-edge-accuracy-metric"]
---

## New metrics

### Building RMS (`lib/metrics/building-rms.js`)

Whole-building point-to-nearest-plane distance. Computes min distance from each building point to any detected plane, then takes RMS. Score derived from raw RMS via 5.0 m cap.

Bugfix in follow-up commit: previously returned `buildingRms=0` (perfect) when `detectedPlanes` was empty, masking total detection failure. Now returns capped max (5.0 m) → score=0.

### Edge type accuracy (`lib/metrics/edge-type.js`)

Geometric classification (ridge/hip/valley/eave/step) compared against GT edge types. 266-line module.

## UI changes

Replaced confusing "lower is better" sub-metrics with click-to-expand raw diagnostic values on parent metrics (residual, boundary, edge, alignment). Clicking a parent metric row expands to show its raw diagnostic numbers.

## Files

`building-rms.js` (new), `edge-type.js` (new), `lib/metrics/index.js`, `lib/benchmark/db.js` (new columns), `scripts/benchmark.js`, `ui/public/app.js`, `ui/public/style.css`. 7 files, +484 −16 lines.
