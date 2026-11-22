---
id: "2026-02-26-idea-ortho-ridge-confirmation"
date: 2026-02-26
title: "Idea: Ortho Edge Detection for Ridge Confirmation"
tags: [idea, ortho, ridge, sobel, canny, confirmation]
category: idea
status: proposed
---

# Idea: Ortho Edge Detection for Ridge Confirmation

## What
Run Sobel/Canny on the orthophoto cropped to building footprint. Project LiDAR-detected ridge lines onto the image. Measure alignment between image edges and LiDAR ridges.

## Why
Ridges visible in orthophoto can confirm or reject LiDAR-detected ridges. Could reduce false ridges on noisy data and detect missed ridges (strong image edge with no LiDAR ridge).

## How
The edge detection (`compute-ortho-artifacts`) already produces a Sobel edge map. Add a new step `ridge-confirm.js` that: (1) rasterizes each LiDAR ridge line onto the ortho grid, (2) computes chamfer distance between ridge pixels and nearest ortho edge pixel, (3) outputs a confidence score per ridge. Low-confidence ridges can be flagged for merge or removal.

## Expected Impact
+1-3 pts on `cross_plan` (reduce false ridges at valley intersections) and `dormer_house` (confirm real dormer ridges).

## Difficulty
Medium. Edge detection exists; the scoring/projection logic is new.
