---
id: "2026-02-25-pipeline-architecture"
date: 2026-02-25
title: "Pipeline Architecture: Composable Steps with Chain-Hash Caching"
tags: [pipeline, architecture, caching, composable-steps, refactor]
category: design
status: concluded
milestone: "Composable pipeline architecture"
---

# Pipeline Architecture: Composable Steps with Chain-Hash Caching

## Motivation

Identified problem: 27 monolithic algorithm files with ~80% code duplication. Goal: decompose into composable steps, define algorithms as pipeline configs.

## Implementation

- Created `lib/pipeline/`: context, runner, cache, pipelines, min-heap (5 core files)
- Extracted 19 step functions from monolithic algorithms into `lib/pipeline/steps/`
- Artifact-based data flow: `PipelineContext` as shared `Map<string, Artifact>`
- Step-level caching with chain-hash keys (upstream change invalidates downstream)
- Cache speedup: ~20x on repeat runs (119ms warm vs 2365ms cold)

## Verification

- **pipeline_v2: matched champion_v2 exactly (240/240 pass, 0 delta)**
- **pipeline_v3: matched v3_combined exactly (10/10 synth10 pass)**

## Bug Fix

`until` condition must be checked after EACH step in repeat block.

Created `MASTER-PLAN.md` -- self-contained V4 roadmap for pipeline migration.
