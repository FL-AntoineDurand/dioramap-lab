---
id: "2026-03-13-night-architecture-refactor"
date: 2026-03-13
title: "Night Architecture Refactor: Cache Demotion, Artifact Schema, Declarative Wiring, Global Index Migration"
tags: [architecture, refactor, wiring, cache, artifacts, global-index, kd-tree, pipeline]
category: development
status: concluded
refs: ["2026-03-12-eliminate-transient-artifacts"]
---

## Overview

Autonomous overnight refactoring session (2026-03-12 evening → 2026-03-13
morning) executing four architectural improvements to `lidar-algos-bench-v2`.
All four phases completed with zero regressions: 2067/2067 tests, 147/147
benchmark cases across 21 pipelines.

Branch: `refactor/lidar-bench-architecture`

## Phase 1: R3 — Cache Demotion (commit b2ed733)

**Problem**: The `StepCache` was always instantiated in `runner.js` even when
caching was disabled (the default). This added ~20 lines of cache-wrapping
logic in the hot path and made the execution flow hard to follow.

**Solution**:
- Lazy cache init: `options.cache ? getCache() : null`
- Decomposed `executeStep` into 4 focused functions: `prepareStep`,
  `executeStepCore`, `cachedExecuteStep`, `executeStep`
- Cache wrapping only runs when `cache !== null`
- Zero overhead when disabled (the common case)

**Validation**: 2023/2023 tests, scores ±0.00 vs baseline.

## Phase 2: R5 — Artifact Schema Enforcement (commit 930e6e6)

**Problem**: ~146 artifact declarations across 50 step files had no `type`
field. The artifact system had no way to validate what a step was producing.

**Solution**:
- Added `type` field to all 204 artifact declarations across 50 step files
- Types: `array`, `metadata`, `planes`, `edges`, `pointcloud`, `transient`
- Runtime validation in `context.js addArtifact()`: warns on type mismatch
  between declared and actual
- Updated `ARTIFACT-TYPES.md` with complete inventory

**Validation**: 204/204 typed (Missing=0), scores ±0.00.

## Phase 3: R1 — Declarative Wiring DSL (commit 4f17cc9)

**Problem**: `wiring.js` was 910 lines of hand-written artifact routing — one
giant object literal mapping every step instance to its artifact sources. Every
new pipeline or step required manual wiring additions, and typos were silent.

**Solution**:
- Created `wiring-resolver.js` (249 lines): auto-resolves artifact routing
  from step `meta.artifacts.reads/writes` declarations
- `resolveWiring()` walks the step list, matches each read to the nearest
  upstream producer, handles branches, `INPUT_NODES`, `FRESH_CREATES`,
  `KD_TREE_NON_PROPAGATING`
- Migrated all 21 pipelines to declarative format with `wire()` + `pipeline()`
  helpers in `pipelines.js`
- 201 explicit overrides across 21 pipelines (for non-obvious routing)
- `wiring.js` reduced from 910 lines to a 14-line backward-compat shim

**Validation**: All 21 wirings match old wiring byte-for-byte. 2029 tests,
147/147 bench, scores ±0.00.

## Phase 4: R2 — Global Index Refactor (commits a93d14c + dabefe0)

**Problem**: The pipeline used a dual index space. Producer steps extracted
building points into `Array<{x,y,z,idx}>` (re-indexed 0..M), while downstream
consumers needed to map back to the original cloud via `denoised-to-orig`.
This caused:
- Every step copying point data into object arrays
- O(N) index translation at every step boundary
- `owner`, `ridge-candidates`, `point-normals` all in local index space
- Confusion about which index space a given array uses

**Solution (phases 1-3, overnight)**: Foundation work.
- Dual kd-tree API in `kd-tree.js`: `buildKdTree(Float32Array, Uint32Array)`
  overload alongside old `buildKdTree(Array<{x,y,z}>)`, dispatch via
  `instanceof Float32Array`
- `point-access.js` helpers: `getPoint()`, `getXYZ()`,
  `subsetFromClassification()`, `extractBuildingIndices()`, set operations
- 3 producer steps migrated (`extract-denoise`, `tile-signals`,
  `scope-to-building`) with backward-compat shim publishing both
  `building-indices` (new) and `building-points` (old)

**Solution (phases 4-7, morning)**: Full consumer migration.
- All 30+ consumer steps migrated from `building-points` to
  `building-indices` (Uint32Array) + `positions` (Float32Array)
- `_common.js` dual API: `ransacPlane`, `createPlaneResult`, `stealPoints`,
  `clusterPointsXZ`, `findConnectedComponents`, `fitPlaneLeastSquares`
- `compute-normals` v4: Float32Array normals (interleaved nx,ny,nz, global)
- `owner`: Int8Array(N) globally indexed, sparse outside building subset
- `seedIndices`: global indices everywhere
- `wiring.js` deleted (single consumer updated to derive from `pipelines.js`)
- Steps with complex internals build local object-array shims (pragmatic)
- `planarity-watershed`: builds own local kd-tree (intentionally different
  index space from upstream typed kd-tree)

**Key fix during migration**: `hemisphere-normals-density-aware-segment`
stage 3 used `idxToLocal` mapping local building-point indices (which the
typed kd-tree would never return). Fixed to `globalToCluster` mapping global
indices from kd-tree results to cluster positions.

**Validation**: 2067 tests, 147/147 bench. v2/v3/v5: zero score diff.
V6 pipelines: max 0.98 diff on mansard-default — correct behavior change
where `ridge-edge-fit` now detects 10 edges (edgeQuality=0.75) instead of
silently failing with 0 edges. Core segmentation metrics unchanged.

## Remaining Work

- Producer shims (`building-points`) still published for UI visualization
- `denoised-to-orig` still consumed by `hull-extraction` (could use
  `buildingIndices` directly since they ARE global indices)
- `context.js rebuildTransients` has fallback to old `building-points` path
- R6 (UI Modularization) deferred — not part of this session

## Metrics

| Phase | Commit | Tests | Bench | Score Δ | Lines |
|-------|--------|-------|-------|---------|-------|
| R3 Cache | b2ed733 | 2023 | 147/147 | ±0.00 | -net |
| R5 Schema | 930e6e6 | 2023 | 147/147 | ±0.00 | +204 types |
| R1 Wiring | 4f17cc9 | 2029 | 147/147 | ±0.00 | 910→14 lines |
| R2 Phases 1-3 | a93d14c | 2067 | 147/147 | ±0.00 | +point-access.js |
| R2 Phases 4-7 | dabefe0 | 2067 | 147/147 | ≤0.98 | 36 files, +1103/-489 |
