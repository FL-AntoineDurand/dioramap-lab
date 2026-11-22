---
id: "2026-02-25-overlap-sort-bug"
date: 2026-02-25
title: "Overlap Sort Bug: Hull Area vs Point Count"
tags: [bugfix, overlap-resolution, hull, point-count]
category: bugfix
status: concluded
---

# Overlap Sort Bug: Hull Area vs Point Count

## Question

Why does `flat_large_parapet` show a catastrophic -78.5 pt regression with V3?

## Setup

V3 combined pipeline on 240 synth test cases. One test case (`flat_large_parapet`) dropped from ~95 to ~16.

## Results

After bisecting, the bug was in `resolveOverlappingPlanes()` at `plane-intersection.js:713`. The function sorts planes by area (largest first) and lets larger planes eliminate contained smaller planes. But it used **hull area** as the size metric.

The failure case: Plane 1 has 43,722 points but a compact hull (1,460 m²). Plane 2 has only 915 points but a sprawling hull (1,487 m²). Because 1,487 > 1,460, Plane 2 was treated as "larger" and eliminated Plane 1 — deleting 43K real points in favor of 900 noise points.

## Fix

Sort by **point count** (primary) with hull area as tiebreaker only.

## Impact

**+1.0 pt across ALL algorithms** (V2, V3, V4). V3 benefited most (+1.4) because its histogram seeding creates large-hull/few-point planes more often than RANSAC.

## Root Cause

Hull area is a poor proxy for plane importance. A plane with many points but a concave footprint can have a smaller convex hull than a plane with few scattered points spanning a large area. Point count directly measures how much evidence supports the plane.

## Implication

Any post-processing that ranks or filters planes should use point count, not geometric area. This applies to overlap resolution, obstacle filtering, and future merge decisions. The bug was present since Feb 22 (overlap resolution was first added) but only triggered when histogram seeding created the pathological large-hull/small-pointcount pattern.
