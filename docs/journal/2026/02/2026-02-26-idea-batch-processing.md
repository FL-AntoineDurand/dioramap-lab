---
id: "2026-02-26-idea-batch-processing"
date: 2026-02-26
title: "Idea: Multi-Building Batch Processing"
tags: [idea, batch, national-scale, worker-threads, research]
category: idea
status: proposed
---

# Idea: Multi-Building Batch Processing

## What
Process entire neighborhoods (100-1000 buildings) in a single pipeline run, sharing common data (DSM, ortho tiles, BDTOPO metadata).

## Why
Current pipeline processes one building at a time. For national-scale processing (846K buildings), batch mode would reduce I/O overhead by 5-10x.

## How
Modify the runner to accept a building list. Add a tile management layer that fetches COPC/ortho/BDTOPO data per tile (1km²) and distributes to individual building pipelines. Worker thread pool for CPU parallelism.

## Difficulty
High. Requires rethinking the pipeline context (per-building vs per-tile scope).

## Timeline
1-2 weeks.
