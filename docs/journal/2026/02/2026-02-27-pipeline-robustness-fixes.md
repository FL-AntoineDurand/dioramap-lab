---
id: "2026-02-27-pipeline-robustness-fixes"
date: 2026-02-27
title: "Pipeline Robustness Fixes: Null Guards and Early-Exit Crashes"
tags: [robustness, null-guard, crash, extract-denoise, runner]
category: bugfix
status: concluded
refs: ["2026-02-27-bad-footprints-silent-corruption"]
---

# Pipeline Robustness Fixes: Null Guards and Early-Exit Crashes

## extract-denoise early-exit crash

When denoising left <30 points, `extract-denoise` returned without writing `owner`, `_kdTree`, `_rng` artifacts → `ransac-seed` crashed with null dereference. Fix: write ALL artifacts unconditionally before the <30 check.

## Runner pre-check for `until` condition

Added `let loopDone = entry.until ? !!ctx[entry.until] : false;` so repeat blocks skip entirely when the until flag is already set before entry.

## compute-normals null guard

Added early exit when totalPts < 3 or kdTree is null.

## normal-cascade-segment null guard

Added early exit with properly shaped empty artifacts when too few points.
