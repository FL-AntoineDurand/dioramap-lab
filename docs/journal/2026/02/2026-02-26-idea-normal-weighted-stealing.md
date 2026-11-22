---
id: "2026-02-26-idea-normal-weighted-stealing"
date: 2026-02-26
title: "Idea: Normal-Weighted Stealing (Track 2 Revisited)"
tags: [idea, normals, stealing, free-improvement, retry]
category: idea
status: proposed
---

# Idea: Normal-Weighted Stealing (Track 2 Revisited)

## Original Attempt
`normals_enhanced` — `stealScore = planeDist * (1 + 0.3 * normalAngle / 90)`.

## Why It Failed
+0.2 pts but +5% runtime (computing per-point normals was the cost).

## Why Retry
In V4/V5/V6 pipelines, normals are already computed by `compute-normals` step. The incremental cost of normal-weighted stealing is ZERO runtime. The +0.2 improvement becomes free.

## How to Retry
Modify `point-stealing.js` to optionally use precomputed normals when `context.artifacts.normals` exists. Add `normalWeight: 0.3` parameter.

## Expected Impact
+0.2 pts across the board. Free improvement in any pipeline that includes `compute-normals`.

## Difficulty
Easy (modify one step, normals already in context).
