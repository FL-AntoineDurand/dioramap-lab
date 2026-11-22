---
id: "2026-03-03-step-dump-replay-tooling"
date: 2026-03-03
title: "Step Dump/Replay Scripts for Fast Pipeline Debugging"
tags: [tooling, debugging, pipeline]
category: development
status: concluded
refs: ["2026-03-03-multi-source-edge-detection"]
---

## Problem

Debugging a single pipeline step required re-running the entire pipeline from
scratch (20+ steps, several seconds). During the wall-roof-detect and
ridge-edge-fit development, we needed to iterate dozens of times on a single
step's logic — each full pipeline re-run added unnecessary latency.

## Solution

Two scripts that snapshot and replay individual steps:

### `scripts/dump-step-input.js`

Runs the pipeline up to (but not including) a target step, then serializes
all artifacts in the PipelineContext to a JSON snapshot.

```bash
node scripts/dump-step-input.js <test-case> <step-name> [pipeline]
# → /tmp/step-snapshot-<step>.json
```

Handles typed arrays (Float32Array, Uint8Array, Int8Array, etc.) via
`{__typed: 'Float32Array', data: [...]}` serialization. Also serializes
k-d tree data, metadata objects, and branch conditions.

### `scripts/replay-step.js`

Loads a snapshot, reconstructs the PipelineContext, runs the target step,
and reports results.

```bash
node scripts/replay-step.js <step-name> [--param key=value ...]
```

Reports: execution time, log messages, written artifacts with counts, edge
details (length, elongation, source), bitmask statistics.

## Usage Pattern

1. `dump-step-input.js proc-church-large-standard wall-roof-detect` — snapshot once (~3s)
2. Edit `wall-roof-detect.js` — change algorithm
3. `replay-step.js wall-roof-detect` — test instantly (~50ms)
4. Repeat 2–3 until satisfied
5. Run full pipeline to verify integration

## Impact

Reduced step debugging iteration time from ~5s (full pipeline) to ~50ms
(replay from snapshot). Enabled the rapid v1→v8 evolution of wall-roof-detect
in a single session.
