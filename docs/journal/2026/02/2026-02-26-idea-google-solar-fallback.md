---
id: "2026-02-26-idea-google-solar-fallback"
date: 2026-02-26
title: "Idea: Google Solar API as Fallback"
tags: [idea, google-solar, api, validation, fallback, research]
category: idea
status: proposed
---

# Idea: Google Solar API as Fallback

## What
Use Google Solar API `buildingInsights` endpoint for buildings outside LiDAR HD coverage or as validation.

## Cost
$0.0025/request (buildingInsights), $0.075/request (dataLayers). 10K free/month.

## Resolution
0.25m in France (vs our 0.1m LiDAR HD). Roof segments: center + bbox only, no full polygons.

## Integration Plan
Use the free tier (10K/month) for validation: compare Google's roof segment count, total area, and solar yield against our detection. Use as fallback for buildings outside LiDAR HD coverage (~40% of France as of Feb 2026).

## Timeline
1 day for API integration, 1 week for systematic comparison across 10K buildings.
