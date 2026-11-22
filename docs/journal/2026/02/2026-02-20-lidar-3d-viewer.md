---
id: "2026-02-20-lidar-3d-viewer"
date: 2026-02-20
title: "LiDAR 3D Viewer with COPC Byte-Range Reader"
tags: [viewer, copc, three-js, bdtopo]
category: development
status: concluded
refs: ["2026-02-15-lidar-hd-feasibility-research"]
---

# LiDAR 3D Viewer with COPC Byte-Range Reader

## Question

Can we extract building-specific point clouds from IGN's 1 km2 COPC tiles using HTTP byte-range requests, and visualize them interactively?

## Implementation

Three components built in this session:

1. **COPC byte-range reader** -- Reads the COPC hierarchy to identify which chunks overlap a building's bounding box, then fetches only those chunks via HTTP Range headers. Extracts building points (class 6) from 1 km2 tiles without downloading the full file (typically 50-200 MB per tile, reduced to 50-500 KB per building)

2. **Three.js 3D viewer** -- Interactive point cloud visualization with:
   - Classification-based coloring (ground, vegetation, building, unclassified)
   - OrbitControls for rotation, pan, and zoom
   - Point size adjustment

3. **BDTOPO integration** -- Fetches the building's official footprint polygon from IGN BDTOPO WFS service. Displays the footprint as a ground-level outline in the 3D scene. Detected roof plane convex hulls are rendered as semi-transparent overlays on the point cloud

## Implication

This viewer serves as the primary debugging and validation tool for all subsequent algorithm development. Being able to visually inspect point clouds, footprints, and detected planes side-by-side is essential for understanding algorithm behavior on real buildings.
