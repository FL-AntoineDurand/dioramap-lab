---
id: "2026-02-26-idea-mbr-shape-fitting"
date: 2026-02-26
title: "Idea: MBR Shape Fitting for Boundary Regularization"
tags: [idea, mbr, shape-fitting, rotating-calipers, retry]
category: idea
status: abandoned
---

# Idea: MBR Shape Fitting for Boundary Regularization

## Original Attempt
Track B1 (`shape_b1_mbr`) — replace convex hulls with minimum bounding rectangles using rotating calipers.

## Why It Failed
-0.3 on 10-case. Over-regularizes non-rectangular planes (hip roof triangles, L-shaped fragments).

## Why Retry
Apply MBR only to planes categorized as `roof` with 4+ edges (rectangular planes). Skip MBR for triangular hip planes, obstacle planes, and irregular shapes. The categorization from V3/V5 provides the type information that was missing.

## How to Retry
Modify `shape-fitting.js` to accept a `preferMBR` flag. Only apply MBR when `plane.category == 'roof' && hull.vertices.length >= 6`. Fall back to ridge-constrained shape (B4) otherwise.

## Expected Impact
+0.5-1 pt on rectangular gable/flat roofs. Risk of regression on complex shapes if the filter is too loose.

## Difficulty
Easy (extend existing step).
