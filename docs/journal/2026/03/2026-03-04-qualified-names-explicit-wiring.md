---
id: "2026-03-04-qualified-names-explicit-wiring"
date: 2026-03-04
title: "Qualified Artifact Names + Explicit Wiring Architecture"
tags: [pipeline, refactor, architecture, wiring, immutability]
category: development
status: concluded
refs: ["2026-03-03-immutable-artifact-refactor"]
---

## Problem

Phase 1 (immutable artifacts) required renaming artifacts to avoid collisions
(`planes-extracted`, `planes-ridge-clipped`, `owner-post-grow`, etc.). This
created a naming mess — steps used inconsistent names, the artifact graph had
no clear data flow edges, and the `Map<bareName, Artifact>` only retained the
last writer.

## Solution

**Qualified names + explicit wiring** — every artifact is stored as
`{stepLabel}.{bareName}` (e.g., `hull-extraction#0.planes`), and ALL data flow
is defined by a wiring table in the pipeline definition.

### Key design decisions

1. **Steps use bare names**: `ctx.getArtifact('planes')`, `ctx.addArtifact('owner', ...)`
2. **Context qualifies writes**: `competitive-grow#0.owner`, `hull-extraction#0.planes`
3. **Reads resolve through wiring ONLY** — no fallback, no lastWriter
4. **Step labels**: `{step-name}#{0-indexed}` always, `input` for pipeline inputs
5. **Transient (`_`-prefixed) artifacts**: bare names, no wiring needed

### Wiring format

Compact mapping: `{ consumerStep: { artifactBareName: sourceStepLabel | [alternatives] } }`

```js
wiring: {
  'ridge-clip#0':  { planes: 'hull-extraction#0' },
  'ransac-seed#0': { owner: 'competitive-grow#0' },
  'hull-extraction#0': { owner: 'point-stealing#0', 'plane-equations': 'refit-planes#0' },
}
```

Array alternatives `[source1, source2]` support branch wiring — first match wins.

### Wiring helpers (`lib/pipeline/wiring.js`)

Composable fragments: `v6Globals()`, `v6NormalInputs()`, `v6OwnerChain()`, etc.
Pipelines compose shared fragments:
```js
const V6A_WIRING = mergeWiring(v6Globals(), v6NormalInputs(), v6OwnerChain(), ...);
```

18 pipelines share ~90% of wiring through 10+ composable functions.

### Pipeline outputs

Explicit qualified names — no resolution:
```js
outputs: {
  planes: 'shape-fitting#0',
  edges: 'resolve-overlaps#0',
  'planes-lod2': 'lod2-simplify#0',
}
```

## Changes

### Infrastructure (4 files)

- **`context.js`**: Major rewrite — `addArtifact` qualifies names, `getArtifact`/`hasArtifact`
  resolve through wiring only. Added `getQualified()` for direct lookup and
  `findByBareName()` for UI server ad-hoc lookups.
- **`runner.js`**: Step labels as `step-name#0`, occurrence tracking, input artifacts
  stored as `input.{name}`, output extraction via `getQualified()`.
- **`cache.js`**: Save/load uses step labels for qualified name construction.
- **`wiring.js`**: New file — wiring helpers + per-pipeline wiring definitions.

### Step files (26 files)

- Reverted all Phase 1 artifact names to generic bare names
- Removed snapshot duplicates: `owner-post-grow`, `owner-post-steal`, `plane-equations-refitted`
- Fixed 3 mutation bugs: `asymmetric-steal` (owner/plane-equations), `competitive-grow` (ridge-candidates), `point-stealing` (ridge-candidates)
- Fixed 5 early-return-without-write bugs: steps must write ALL declared outputs even on early return

### UI (3 files)

- **`server.js`**: API serves wiring + outputs. Artifact endpoint supports qualified names via `getQualified()` + `findByBareName()` fallback.
- **`artifact-graph-view.js`**: Graph edges built from wiring table instead of `lastWriter` tracking. Displays bare names on ports, qualified names in tooltips.
- **`app.js`**: Panel displays `bareName`, fetches with qualified `name`. Fixed `activatePlaneArtifact` style detection for qualified keys.

### Tests (2 files)

- `test/steps.test.js`: Contexts created with wiring, artifacts checked by qualified names.
- `test/lod2-simplify.test.js`: Same pattern.

## What this eliminated

All Phase 1 names: `planes-extracted`, `planes-ridge-clipped`, `planes-corners-closed`,
`planes-footprint-clipped`, `planes-obstacles-filtered`, `planes-overlaps-resolved`,
`planes-fitted`, `edges-computed`, `edges-snapped`, `edges-snapped-all`,
`edges-corners-closed`, `edges-overlaps-resolved`, `owner-post-grow`,
`owner-post-steal`, `plane-equations-refitted`.

## Verification

- 31/31 unit tests pass
- 35/35 benchmark cases pass (V2, V3, V6a, V6d, V6e: avg 93-96)
- UI artifact fetching works with qualified names
- Wiring served via `/api/pipelines` (23 entries for V6a)
