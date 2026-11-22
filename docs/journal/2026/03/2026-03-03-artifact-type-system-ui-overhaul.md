---
id: "2026-03-03-artifact-type-system-ui-overhaul"
date: 2026-03-03
title: "Artifact Type System Enrichment and UI Overhaul"
tags: [ui, pipeline, architecture, artifacts]
category: development
status: concluded
refs: ["2026-03-03-immutable-artifact-refactor", "2026-03-03-multi-source-edge-detection"]
---

## Artifact Type System

### New artifact types

Added two semantic types to `PipelineContext`:
- `'planes'` — Array of plane result objects (hull, normal, d, area, etc.)
- `'edges'` — Array of edge segment objects ({ from, to, type?, planeIndices? })

Previously both were stored as `'metadata'`, making it impossible for the UI to
distinguish plane artifacts from other metadata. Now the UI uses `art.type === 'planes'`
for type-aware rendering (toggle 3D meshes) and `art.type === 'edges'` for edge lines.

### `colorMode` rendering hint

Added `colorMode` option to `addArtifact()` for array-type artifacts:
- `'categorical'` — owner/segment arrays (random color per value)
- `'class-code'` — LAS classification codes (standard LAS colors)
- `'bitmask'` — ridge-candidates (popcount-based heat coloring)
- `'binary'` — 0/1 masks (dim/bright)

Propagated through cache serialization (`cache.js`) and artifact listing.

### Steps updated

All steps that produce planes or edges now use the correct type:
- `hull-extraction`: `addArtifact('planes', 'planes', ...)`
- `lod2-simplify`: `addArtifact('planes-lod2', 'planes', ...)`
- `close-corner-gaps`, `ridge-clip`, `shape-fitting`, etc.: `'planes'` type
- `sharp-edges`, `ridge-clip`, `close-corner-gaps`: `'edges'` type
- `cascade-shared`: `colorMode: 'categorical'` for segments, `'bitmask'` for ridge-candidates
- `competitive-grow`, `point-stealing`: `colorMode: 'categorical'` for owner
- `runner.js`: `ground-truth` type changed from `'metadata'` to `'planes'`

## UI Overhaul

### Axis gizmo

Added a 3D orientation gizmo in the bottom-left corner of the viewer canvas.
Renders X (red), Y (green), Z (blue) axes with labels. Synchronizes rotation
with the main camera via `gizmoCamera.quaternion.copy(camera.quaternion)`.
Hidden when graph view is active.

- New canvas: `<canvas id="axis-gizmo">` in `index.html`
- CSS: `.axis-gizmo` positioned absolute bottom-left
- Three.js: separate scene/camera/renderer for gizmo

### Artifact panel sections

Split the artifact panel into labeled sections with headers:
- **General Controls** — point cloud, unassigned, non-building toggles
- **Artifacts** — per-step artifact rows

### Removed auto-activation

Planes and GT planes no longer auto-activate on load. User must explicitly
click to toggle them on. This avoids visual clutter on complex buildings and
makes the initial 3D view cleaner (point cloud only).

### Edge artifact rendering

Clicking an `edges`-type artifact now toggles 3D edge line visibility.
Edge icon is `&#9585;` (diagonal slash) to distinguish from plane triangle `&#9650;`.

### Misc

- Unassigned/non-building default to hidden (`false`)
- Brighter unassigned/non-building colors (`#556677`, `#443344`)
- Primary button text color changed to white
- Legend position moved up to accommodate gizmo
- Plane artifact detection uses `art.type === 'planes'` instead of name matching
- Removed hardcoded `PLANE_ARTIFACT_NAMES` set

## Files Changed

- `lib/pipeline/context.js` — new types + colorMode option
- `lib/pipeline/cache.js` — colorMode serialization
- `lib/pipeline/runner.js` — ground-truth as 'planes', classifications colorMode
- `lib/pipeline/steps/cascade-shared.js` — colorMode annotations
- `lib/pipeline/steps/competitive-grow.js` — colorMode for owner
- `lib/pipeline/steps/histogram-seed.js` — colorMode for owner
- `ui/public/app.js` — gizmo, sections, type-aware rendering, no auto-activate
- `ui/public/index.html` — gizmo canvas
- `ui/public/style.css` — gizmo CSS, section headers, button colors
