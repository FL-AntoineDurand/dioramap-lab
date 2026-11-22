---
id: "2026-03-04-per-point-metadata-pipeline"
date: 2026-03-04
title: "Per-Point Metadata Plumbing Through Pipeline"
tags: [pipeline, per-point-metadata, extract-denoise, wiring, artifacts]
category: development
status: concluded
refs: ["2026-03-04-flight-strip-simulation", "2026-03-03-artifact-type-system-ui-overhaul"]
---

# Per-Point Metadata Plumbing Through Pipeline

## Problem

Real IGN LiDAR HD data carries per-point metadata (ScanAngle, PointSourceId,
ReturnNumber, NumberOfReturns) that is critical for strip-aware processing. The pipeline
had no way to receive, forward, or consume this metadata.

## Changes

### Runner: 6 new input artifacts

In `runner.js`, both `runPipeline()` and `runPipelineWithContext()` now register
optional per-point metadata as input artifacts:

```js
if (options.scanAngles)       ctx.addArtifact('scan-angles', 'array', options.scanAngles);
if (options.pointSourceIds)   ctx.addArtifact('point-source-ids', 'array', options.pointSourceIds);
if (options.returnNumbers)    ctx.addArtifact('return-numbers', 'array', options.returnNumbers);
if (options.numberOfReturns)  ctx.addArtifact('number-of-returns', 'array', options.numberOfReturns);
if (options.bdtopoMetadata)   ctx.addArtifact('bdtopo-metadata', 'metadata', options.bdtopoMetadata);
if (options.lambert93Origin)  ctx.addArtifact('lambert93-origin', 'metadata', options.lambert93Origin);
```

No signature change — everything flows through the existing `options` bag.

### Wiring: extract-denoise reads per-point arrays

`wiring.js` updated `extractDenoiseInputs()` to wire the 4 per-point arrays from input:

```js
'scan-angles': IN, 'point-source-ids': IN,
'return-numbers': IN, 'number-of-returns': IN,
```

### extract-denoise v3 → v4: metadata forwarding

After denoising filters points (removing noise class 7, vegetation class 3-5), the
surviving indices are tracked via `denoisedToOrig`. The step now uses this mapping
to forward per-point metadata:

```js
for (const artName of ['scan-angles', 'point-source-ids', 'return-numbers', 'number-of-returns']) {
  const src = ctx.hasArtifact(artName) ? ctx.getArtifact(artName).data : null;
  if (src) {
    const filtered = new src.constructor(denoisedToOrig.length);
    for (let i = 0; i < denoisedToOrig.length; i++) filtered[i] = src[denoisedToOrig[i]];
    ctx.addArtifact('building-' + artName, 'array', filtered);
  }
}
```

Output artifacts are prefixed `building-` (e.g., `building-scan-angles`) following the
convention for post-denoise data (`building-positions`, `building-classifications`).

## Design decision: conditional writes

The `building-scan-angles` etc. artifacts are **not** declared in the step's formal
`writes` list. They are conditionally written only when the corresponding input exists.
Declaring them as formal writes would cause the step validation test to fail for
non-flight-strip test cases where these artifacts are absent.

This is a passthrough pattern — the step doesn't consume the metadata, it just
reindexes and forwards it for downstream steps that may need it.

## Validation

Verified with a flight-strip-enabled test case:
- Input: 20461 points with scan-angles, point-source-ids, etc.
- After denoise: 13311 building points
- `building-scan-angles` correctly has 13311 entries matching `building-positions`
- Quick benchmark: 7/7, avg 95.4 — identical to baseline (additive change, no regression)
