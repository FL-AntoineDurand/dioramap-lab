---
id: "2026-03-04-live-mode-map-picker"
date: 2026-03-04
title: "Live Mode + UI Map Picker for Real Buildings"
tags: [live-mode, ui, map-picker, copc, ign-api, maplibre]
category: development
status: concluded
refs: ["2026-03-04-flight-strip-simulation", "2026-03-02-ign-lidar-hd-source-characterization"]
---

# Live Mode + UI Map Picker for Real Buildings

## Problem

Testing on real buildings required manually importing test cases. There was no way to
point at a building on a map and immediately run the pipeline on its actual LiDAR data.

## CLI: `--live lat,lng`

```bash
node scripts/benchmark.js --pipelines v6a-base --live 48.87,2.33 --no-db
```

Flow:
1. `fetchBuildingLive(lat, lng)` in `building-importer.js`
2. Fetches building footprint from BDTOPO WFS (with full properties: hauteur, nature, etc.)
3. Discovers COPC tiles via WFS feature intersection
4. `extractLidarPointsFull()` reads all PDRF 6 fields (ScanAngle, PointSourceId, etc.)
5. Returns a complete `RunnerInputs` object
6. Pipeline runs, results displayed in terminal

## Extended COPC extraction

New `extractFromTileFull()` alongside existing `extractFromTile()`:
- Reads ScanAngle (Int16, ×100), PointSourceId (Uint16), ReturnNumber (Uint8),
  NumberOfReturns (Uint8) from PDRF 6 records
- Re-centers to local coordinates like existing extraction
- Returns typed arrays matching the `RunnerInputs` contract

New `queryLidarTilesWithMeta()` returns tile-level metadata (instrument, codebloc,
codemissio, dates) from the WFS tile features.

## BDTOPO full properties

`fetchBuildingFootprint()` now returns the complete BDTOPO properties object in
`building.bdtopo`:
- `hauteur`, `nature`, `usage_1`, `materiaux_de_la_toiture`, `nombre_d_etages`
- `altitude_minimale_sol`, `altitude_maximale_toit`
- These become `bdtopo-metadata` artifact in the pipeline

## UI: Map Picker

### Stack
- MapLibre GL JS (CDN: `unpkg.com/maplibre-gl@3.6.2`)
- IGN ortho WMTS basemap (`data.geopf.fr/wmts`)
- Address geocoding via `api-adresse.data.gouv.fr`

### Implementation: `ui/public/map-picker.js`

`initMapPicker(container, callback)`:
- Renders IGN aerial photo basemap centered on Paris
- Click anywhere → returns `{lat, lng}` to callback
- Search box: type address → geocode → fly to location
- `showFootprint(ring)`: displays building footprint polygon on map

### Integration: `ui/public/app.js`

"Map Picker" button in status bar toggles a slide-out panel. On building click:
1. Sends `POST /api/run-live { pipeline, lat, lng }` to server
2. Server calls `fetchBuildingLive()` → `runPipeline()` → returns result
3. Result flows into existing 3D viewer

### Server: `POST /api/run-live`

New endpoint in `ui/server.js`:
- Dynamically imports `fetchBuildingLive` from building-importer
- Accepts `{ pipeline, lat, lng, params? }`
- Returns full pipeline result including step artifacts

## Limitations

- COPC fetching takes 5-15 seconds depending on tile size and network
- No caching of fetched data (each click re-fetches)
- Building selection is by nearest footprint to click point — may miss if clicking far
  from any building
- Requires internet access for IGN APIs
