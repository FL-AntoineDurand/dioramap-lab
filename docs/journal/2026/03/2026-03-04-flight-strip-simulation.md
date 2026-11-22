---
id: "2026-03-04-flight-strip-simulation"
date: 2026-03-04
title: "Flight-Strip Simulation + Harmonized Input Schema"
tags: [flight-strips, synthetic, per-point-metadata, binary-sidecar, live-mode, ui]
category: development
status: concluded
refs: ["2026-03-02-ign-lidar-hd-source-characterization"]
pipeline: v6a-base
---

# Flight-Strip Simulation + Harmonized Input Schema

## Context

The synthetic point cloud generator cast uniform vertical rays — no scan angles, no flight
strips, no per-point metadata. Real IGN LiDAR HD illuminates buildings from 2-4 overlapping
strips at different cross-track angles (±20°), each with its own PointSourceID.

## What Changed

### 1. Flight-Strip Point Cloud Generator (`lib/synthetic/flight-strip-generator.js`)

New physics-based generator modeling IGN LiDAR HD acquisition:
- 3 default strips (PSID 37/38/39) at cross-track angles -12°/+1°/+14°
- Ray-plane intersection with tilted direction vectors (not vertical)
- Angle-dependent noise: σ_eff = σ_z / cos(scanAngle)
- Sparse wall returns at oblique angles
- Multi-return modeling (~5% at roof edges)
- Per-point arrays: scanAngles (Int16, ×100), pointSourceIds, returnNumbers, numberOfReturns

### 2. Unified RunnerInputs Contract (`lib/test-case-spec.js`)

Three spec types:
- **synthetic**: recipe-only (~1 KB), regenerated from deterministic seed
- **real**: binary sidecar (.bin) + slim spec JSON
- **live**: fetched from IGN APIs at runtime

### 3. Binary Sidecar Format (`lib/test-case-binary.js`)

Compact binary format for real test cases: header (16 bytes) + Float32 positions + Uint8
classifications + optional per-point metadata arrays. ~3x smaller than JSON.

### 4. Per-Point Metadata in Pipeline

- **runner.js**: Registers scan-angles, point-source-ids, return-numbers, number-of-returns,
  bdtopo-metadata, lambert93-origin as input artifacts
- **extract-denoise v4**: Forwards per-point metadata through denoising filter using
  denoised-to-orig index mapping
- **wiring.js**: Updated extractDenoiseInputs() to wire the new artifacts from input

### 5. Extended COPC Extraction (`lib/real-buildings/building-importer.js`)

- `extractLidarPointsFull()`: Reads all PDRF 6 fields (ScanAngle, PointSourceId, etc.)
- `fetchBuildingLive()`: Returns RunnerInputs directly without file I/O (for live mode)
- `queryLidarTilesWithMeta()`: Returns tile metadata (instrument, codebloc, etc.)
- `fetchBuildingFootprint()`: Now returns full BDTOPO properties

### 6. `--live lat,lng` Benchmark Mode

`node scripts/benchmark.js --pipelines v6a-base --live 48.87,2.33 --no-db`

Fetches building footprint + COPC data from IGN APIs, runs pipeline, shows results.

### 7. UI Map Picker

MapLibre GL JS map with IGN ortho basemap in the Pipeline Explorer UI.
Click a building → fetch COPC → run pipeline → display 3D result.
Address search via api-adresse.data.gouv.fr.

### 8. Migration Script (`scripts/migrate-test-cases.js`)

Migrates legacy JSON test cases to spec-based format:
- Synthetic: extracts spec (recipe only)
- Real: extracts binary sidecar + slim spec
- `--verify` checks content_hash reproducibility

## Sensor Profile Integration

IGN profiles (`ign-roof`, `ign-clean`, `ign-noisy`) now include `useFlightStrips: true`
with DEFAULT_STRIPS. Drone/legacy profiles remain vertical-ray.

### 9. Per-Strip Phase Offsets (grid alignment fix)

All strips initially shared the same grid origin (0,0), producing an unrealistically aligned
point pattern. Fixed by adding per-strip random phase offsets: `phaseOffsetX = rng() * spacing`,
`phaseOffsetZ = rng() * spacing` for each strip's scan grid.

### 10. Three Input Nodes in Artifact Graph

Restructured from single `input` label to three logical groups:
- `input-lidar`: positions, classifications, per-point metadata (scan angles, PSIDs, etc.)
- `input-bdtopo`: footprint, building metadata, lambert93 origin
- `input-gt`: ground truth planes (when available)

Updated `wiring.js` with `IN_L`, `IN_B`, `IN_GT` constants. Artifact graph view renders
three input boxes instead of one.

### 11. GT Wall Edges in UI

Server now returns `gtEdges` (array of `{type, from, to, planeIds}`) in preview/run responses.
Frontend renders wall edges as orange line segments, other edges as purple, via Three.js
LineSegments with vertex colors.

### 12. Real TC Metadata Re-Import Script (`scripts/reimport-real-metadata.js`)

Existing 42 real binary sidecars were created without per-point metadata. Script re-fetches
from COPC tiles using stored lat/lng and l93_center, then rewrites .bin with scanAngles,
pointSourceIds, returnNumbers, numberOfReturns. Dry-run by default.

## Validation

- All 1674 tests pass (317 suites, 0 failures)
- Quick benchmark: 7/7, avg score 95.4 (unchanged from baseline)
- Backwards compatible: standard (non-flight-strip) test cases produce identical results
- Extract-denoise v4 correctly forwards per-point metadata through denoising

## Files Created

| File | Purpose |
|------|---------|
| `lib/test-case-spec.js` | TestCaseSpec + RunnerInputs contracts |
| `lib/test-case-loader.js` | Central loader (synthetic/real/live/legacy) |
| `lib/test-case-binary.js` | Binary sidecar format |
| `lib/synthetic/flight-strip-generator.js` | Multi-strip point cloud generator |
| `lib/synthetic/mock-metadata.js` | BDTOPO + COPC mock metadata |
| `ui/public/map-picker.js` | MapLibre map picker for live buildings |
| `scripts/migrate-test-cases.js` | Legacy → spec migration |
| `test/flight-strip-generator.test.js` | 10 tests |
| `test/mock-metadata.test.js` | 6 tests |

## Files Modified

| File | Change |
|------|--------|
| `lib/synthetic/sensor-profiles.js` | Added useFlightStrips + strips to IGN profiles |
| `lib/synthetic/test-case-builder.js` | Conditional flight-strip generator |
| `lib/pipeline/runner.js` | Register per-point metadata artifacts |
| `lib/pipeline/steps/extract-denoise.js` | v3→v4: forward metadata through filtering |
| `lib/pipeline/wiring.js` | 3 input nodes (IN_L, IN_B, IN_GT) + per-point metadata wiring |
| `ui/public/artifact-graph-view.js` | Render 3 input nodes instead of 1 |
| `lib/real-buildings/building-importer.js` | Full COPC extraction + live fetch |
| `scripts/benchmark.js` | --live mode + help text |
| `ui/server.js` | /api/run-live endpoint |
| `ui/public/index.html` | MapLibre CDN + map container |
| `ui/public/style.css` | Map picker styles |
| `ui/public/app.js` | Map picker integration |
