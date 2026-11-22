---
id: "2026-02-19-solar-poc-report"
date: 2026-02-19
title: "Solar PoC Report and Business Strategy"
tags: [solar, pvgis, ortho, google-solar]
category: development
status: concluded
refs: ["2026-02-15-lidar-hd-feasibility-research"]
---

# Solar PoC Report and Business Strategy

## Question

Can we build a complete solar potential report using LiDAR roof planes + PVGIS, and how does our approach compare to Google Solar API in cost and quality?

## Implementation

Built four core modules for the solar pipeline:

- **`lib/solar/pvgis-client.js`** -- PVGIS API client with azimuth conversion. Our coordinate system uses 0=North clockwise; PVGIS uses 0=South. The client handles this transparent conversion
- **`lib/solar/ortho-fetcher.js`** -- fetches aerial orthophotos from IGN WMS for visual context in reports
- **`lib/solar/copc-tile-finder.js`** -- WFS discovery to find which COPC tiles cover a given building location
- **`lib/solar/multimodal-fusion.js`** -- combines geometry from LiDAR planes with yield data from PVGIS into a unified report structure

Built a solar report page in the agri-map frontend integrating LiDAR point cloud visualization (Three.js), detected roof planes with hull outlines, and per-plane PVGIS yield estimates.

## Results

PVGIS validation for Brittany (latitude ~48.1N):

| Orientation | Tilt | Annual Yield (kWh/kWp/yr) |
|-------------|:----:|:-------------------------:|
| South | 30 deg | 1,113 |
| Flat | 0 deg | 951 |
| North | 30 deg | 659 |

These values align with published irradiance maps for western France and confirm PVGIS is reliable for yield estimation.

## Google Solar API Comparison

Google Solar API pricing for 846K agricultural buildings (the AgriMap target):

- `buildingInsights` endpoint: $0.01/request = $8,460
- `dataLayers` endpoint: $0.075/request = $63,450
- Resolution: 0.25m in France (vs our 0.1m from LiDAR HD at 10 pts/m2)
- Roof segments: returns center + bounding box only, not polygons

**Recommendation**: Use Google Solar as a fallback only (e.g., for buildings outside LiDAR HD coverage). Our approach is free, higher resolution, and returns full plane polygons.

## Implication

The solar PoC validates the end-to-end pipeline from LiDAR extraction through yield estimation. The main remaining risk is COPC download reliability at scale, which will be tested during the benchmark framework phase.
