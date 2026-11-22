---
id: "2026-02-26-idea-bdtopo-metadata-priors"
date: 2026-02-26
title: "Idea: BDTOPO Metadata Priors for Algorithm Selection"
tags: [idea, bdtopo, metadata, branching, silo]
category: idea
status: proposed
---

# Idea: BDTOPO Metadata Priors for Algorithm Selection

## What
Use the 29 BDTOPO attributes (nature, usage, materiaux_de_la_toiture, hauteur, nombre_d_etages) to select algorithm parameters or skip entire pipeline branches.

## Why
A silo needs cylindrical detection, a concrete flat roof should bypass RANSAC entirely, a 1-story light construction expects 1-2 planes max. Currently the pipeline treats all buildings identically.

## How
`fetch-metadata` step already retrieves the data. Add decision logic in the runner or a new `select-profile.js` step. Decision tree:
- `nature = 'Silo'` → cylindrical detection path (not yet built)
- `materiaux_de_la_toiture = '4' (beton)` → force flat path (histogram seed)
- `usage_1 = 'Agricole' AND hauteur > 8m` → looser thresholds, expect fewer planes
- `nombre_d_etages = 1` → lower maxReseedIter (simpler roof)
- `construction_legere = true` → 1-2 planes max, skip obstacle detection

## Expected Impact
+2-5 pts on specialized building types. Main value is robustness on real buildings, not synthetic score.

## Difficulty
Easy for parameter adjustment, medium for new algorithm paths (cylindrical).
