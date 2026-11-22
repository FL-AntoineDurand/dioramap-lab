---
id: "2026-02-15-lidar-hd-feasibility-research"
date: 2026-02-15
title: "LiDAR HD Feasibility Research"
tags: [lidar-hd, copc, ign, feasibility]
category: development
status: concluded
---

# LiDAR HD Feasibility Research

## Question

Can IGN's LiDAR HD dataset be used for per-building roof plane detection at national scale, and what is the viable extraction strategy?

## Research

Investigated the IGN LiDAR HD programme specifications and access methods:

- **Specs**: 10 pts/m2 density, COPC (Cloud Optimized Point Cloud) format, Lambert93 CRS, classification class 6 = building
- **Endpoint status**: Old COPC endpoint deprecated (returns 404s). Discovered a WFS tile discovery endpoint that provides bounding boxes and download URLs for 1 km2 tiles
- **National coverage**: approximately 60% complete. Paris, Lyon, Toulouse, Strasbourg, Brittany, Nice, and Bordeaux are done. Lille is pending
- **Volume**: Bulk download of the entire dataset is impractical (~100 TB national). Per-building COPC byte-range extraction is the only viable approach -- COPC format supports HTTP range requests to read only the spatial subset needed
- **RGE ALTI**: The 1m DEM (Digital Elevation Model) is sufficient for solar yield estimation but lacks the resolution needed for individual roof plane detection

## Decision

Proceed with COPC byte-range extraction for roof plane detection and PVGIS for solar yield estimation. The pipeline will be: WFS tile discovery (find which 1 km2 tile covers the building) followed by COPC spatial query (extract only building points via byte-range reads) followed by plane detection algorithm followed by PVGIS yield per detected plane.

## Implication

This approach avoids downloading entire tiles (50-200 MB each) and instead fetches only the ~50-500 KB of points within a building's footprint. National scalability depends on COPC server reliability and byte-range read latency, both of which need validation on real buildings.
