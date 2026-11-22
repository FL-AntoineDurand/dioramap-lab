---
id: "2026-03-05-idea-reclassify-unassigned-points"
date: 2026-03-05
title: "Idea: Re-classify Unassigned Points Fitting Detected Planes"
tags: [classification, extract-denoise, plane-fitting, recall]
category: idea
status: proposed
refs: ["2026-03-05-psid-metadata-deep-analysis"]
pipeline: v6a-base
---

## Context

IGN's Myria3D classifier has ~93.7% recall for building points. The ~6% missed points are spatially correlated at roof boundaries, under vegetation, and at complex structures. Some are tagged as class 1 (unclassified), class 2 (ground), or class 5 (high vegetation) despite being actual roof points.

After plane fitting, we know the roof geometry. Points that weren't classified as building but fit a detected plane with low residual are likely misclassified roof points.

## Proposed Approach

Add a post-fitting step (after `refit-planes`) that:

1. Scans all non-building points within the footprint (class 1, 2, 5)
2. For each, compute point-to-plane distance against all detected planes
3. If distance < `distThreshold` AND point has spatial neighbors on the plane → adopt it
4. Re-run competitive-grow with the expanded point set

This is essentially "if it fits a roof plane, treat it as roof" — using the pipeline's own output as evidence to override the classifier.

## Expected Benefit

- Recovers 0.5-3% of roof points at boundaries
- Improves hull completeness (fewer gaps at eaves)
- Particularly valuable for complex buildings where Myria3D struggles

## Risks

- False positives: ground points near low eaves could be adopted into roof planes
- Need a conservative distance threshold (e.g., 2×σ_z) to avoid over-inclusion
- Should only adopt if spatial connectivity to existing plane is confirmed (k-NN check)

## Effort

Medium — new pipeline step (~100 lines), needs careful threshold tuning and benchmark validation.
