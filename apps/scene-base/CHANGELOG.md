# @dioramap/scene-base changelog

Versioning: package semver. Composer-shape changes (new `meta` field,
new RenderContext surface, new visibility-tree event) require a
documented minor bump.

## Unreleased

- **Stray diagnostic logs removed** (`scene/create-scene.js`). Two `console.log` calls — `[scene-base] init camera …` (camera-framing computation dump from the v0.6.1 fix) and `[scene-base] dimensions dump …` (WebGLRenderer mount diagnostic) — were both gated by a comment saying "After this is verified across viewports, drop or gate behind a debug flag." Verified end-to-end on the agri-map.fr embed; dropped. No other behavior change. Will ship with the next viewer-sdk release that re-bundles scene-base.

## 0.1.0 — 2026-04-27 (S7 milestone — first packaged cut)

This is the first tagged version of `@dioramap/scene-base`. The package
has been growing alongside lab-ui (L1–L11) and the viewer SDK (S1–V8);
S7 collects the work into a properly documented, dispose-clean release.

Public surface frozen at this point:
- `Viewer` (`scene/create-scene.js`) with `dispose()` (new) — cancels
  the internal rAF, disconnects the resize observer, tears down the
  EffectComposer + render target + sky texture + WebGLRenderer +
  OrbitControls + canvas element. Idempotent.
- `MarineCompass`, `createEDLPass`, `BaseRenderContext`, `SceneState`,
  `createSceneState`, `SunControl`, `generateLeafTexture`,
  `loadGLBModel` — re-exported from the package root and via per-file
  subpaths declared in `package.json` `exports`.
- Composite-visualizer subtype (`src/viz/`): 8 modules — `lidar-positions`,
  `parcel-lines`, `point-cloud-discs`, `canopy`, `elevation-surround`,
  `terrain`, `deshadowed-ortho`, `ortho-surround-jpeg`. Each carries
  `meta.{name, requires, pipelines, default, supersedes?}` + `render`
  (+ optional streaming triplet).

Documentation added in S7:
- Root `README.md` (canonical references, tree-shaking invariant,
  public surface map).
- Per-directory READMEs at `src/scene/`, `src/viz/`, `src/utilities/`.
- The umbrella visualizer doc continues to live at
  `v3/docs/VISUALIZERS.md` — READMEs link to it instead of restating it.

Tree-shaking invariants enforced externally:
- `package.json` declares `"sideEffects": false` and exposes per-file
  subpaths under `exports`.
- The viewer-sdk's `scripts/check-bundle-size.js` (added in V8) scans
  the production bundle for distinctive labels of composers the SDK
  excludes (terrain.viz / deshadowed-ortho.viz / ortho-surround-jpeg.viz)
  and fails CI on any leak.

Outstanding scene-base work (not blocking 0.1.0):
- Viewer's `scene` field is publicly mutated by callers that do
  `scene.add(...)` outside the layer system. We tolerate that today
  (canopy / disc viz modules + the V6 models system rely on it). A
  future `attach`/`detach` API on Viewer could close the gap, but it
  isn't load-bearing.

## Pre-0.1.0 (untagged)

Shipped as part of consumer milestones; see
`apps/viewer-sdk/CHANGELOG.md` (S1–V8) and
`v3/docs/plan/PLAN-LAB-UI-PORT.md` (L1–L11) for chronology.
