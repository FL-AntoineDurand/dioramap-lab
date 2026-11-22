---
id: "2026-03-10-physics-multireturn-edge-classification"
date: 2026-03-10
title: "Physics-based multi-return and automatic edge classification"
tags: [multi-return, edge-classification, procedural, ground-truth]
category: development
status: concluded
refs: ["2026-03-05-wall-roof-detect-v12-v13-multireturn-raw", "2026-03-04-edge-accuracy-metric"]
---

## Multi-return beam physics

Replaced teleport-to-ground with beam-width offset + ray-cast for second returns:
- Beam radius derived from Riegl VQ-780i divergence × flight altitude (~0.25 m at 1000 m)
- Second return offset in outward direction perpendicular to nearest MR-eligible edge
- Plane-ownership check: MR only triggers when first return's plane borders the edge
- Eliminated 139/144 false deep-inside-footprint ground returns (dormer cheek issue)

## Edge classification system

New module `lib/geometry/edge-classify.js`:
- Automatic edge extraction from plane boundary geometry
- 11 edge types: ridge, hip, valley, flashing, step, corner, eave, rake, dormer-ridge, dormer-base, base
- `multiReturnSegments()` returns MR-eligible edges with outward normals + planeIds
- `buildTestCase()` now auto-classifies edges via `classifyEdges()`
- Wall planes added to `dormer_house` procedural generator

## UI

- GT edges registered as artifacts (`input-gt.gt-edges`) with legend
- `EDGE_TYPES` color palette (12 types) for both detected and GT edge rendering
- Edge legend with sorted type counts
- Corner/base excluded from edge quality metrics

## Pipeline fixes (8 pre-existing test failures)

- `wall-roof-detect` v14: reads `boundary-candidates` from `detect-boundary`
- Pipeline registry updated: 20 pipelines (removed v2-co-register/v2-dsm-coreg, added exploration)
- `extract-denoise`: provide per-point metadata for conditional artifact writes
- `obstacle-filter`: test survival + original area (step restores after temporary Infinity)
