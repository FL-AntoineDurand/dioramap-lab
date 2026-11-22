---
id: "2026-02-26-v2-rewrite-pipeline-only"
date: 2026-02-26
title: "V2 Rewrite: Pipeline-Only Architecture"
tags: [v2-rewrite, pipeline, architecture, cleanup]
category: development
status: concluded
milestone: "V2 rewrite complete"
---

# V2 Rewrite: Pipeline-Only Architecture

- V2 rewrite completed — unified artifact pipeline, 24 steps, 6 pipelines, UI explorer
- Created `lidar-algos-bench-v2/` as clean rewrite focused on pipeline architecture
- Dropped all 27 monolithic algorithm files (kept only shared utilities)
- **24 pipeline steps** in `lib/pipeline/steps/` (vs 19 in V1)
- **6 registered pipelines:** `v2-champion`, `v3-combined`, `v4-normal-histogram`, `v2-co-register`, `v2-dsm-coreg`, `v5-multimodal-base`
- Total: ~19,000 lines of JS (down from ~30,000+ in V1)
