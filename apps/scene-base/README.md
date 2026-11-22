# @dioramap/scene-base

Shared Three.js scene infrastructure consumed by **lab-ui**
(`v3/apps/lab-ui/`) and the **viewer SDK** (`v3/apps/viewer-sdk/`).

What lives here:

- **`src/scene/`** — the Three.js scene / renderer / cameras / EDL post-process
  / sun control / scene-state / `BaseRenderContext`. The `Viewer` class is the
  central object; both apps mount one per page.
- **`src/viz/`** — composite-visualizers (multi-artifact `.viz.js` modules
  that paint the production scene). Lab-ui discovers them via Vite glob;
  the SDK imports a closed allow-list.
- **`src/utilities/`** — small helpers shared between viz modules
  (GLB loader hand-off, leaf sprite generator).

## Canonical references

- **Visualizer vocabulary + dispatch order** —
  [`v3/docs/VISUALIZERS.md`](../../docs/VISUALIZERS.md). Defines the three
  visualizer subtypes (builtin, step, composite), the five-surface
  `RenderContext` API, and where each subtype lives.
- **SDK consumption rules** —
  [`v3/docs/plan/PLAN-VIEWER-SDK-PORT.md`](../../docs/plan/PLAN-VIEWER-SDK-PORT.md)
  (closed allow-list §5.2, frozen subscription set §5.4, curated
  visibility toggles §5.5, bundle-size invariant §5.8).
- **Lab-ui consumption rules** —
  [`v3/docs/plan/PLAN-LAB-UI-PORT.md`](../../docs/plan/PLAN-LAB-UI-PORT.md)
  (composite-views panel §10, step-visualizer dispatch §11).

## Tree-shaking invariant

`package.json` declares `"sideEffects": false` and a per-file `exports`
map. The SDK imports composers via subpaths (e.g.
`@dioramap/scene-base/viz/lidar-positions`) so unused viz modules
never enter its bundle. The viewer-sdk's
`scripts/check-bundle-size.js` enforces this: it scans the production
bundle for distinctive labels of composers the SDK excludes
(terrain.viz / deshadowed-ortho.viz / ortho-surround-jpeg.viz) and
fails CI on any leak.

## Public surface

The package root re-exports the most-used classes for convenience:

```js
import {
  Viewer,                // src/scene/create-scene.js
  MarineCompass,         // src/scene/compass-gizmo.js
  createEDLPass,         // src/scene/postprocess/edl.js
  BaseRenderContext,     // src/scene/render-context.js
  SceneState, createSceneState,  // src/scene/scene-state.js
  SunControl,            // src/scene/sun.js
  generateLeafTexture,   // src/utilities/leaf-sprite.js
  loadGLBModel,          // src/utilities/gltf-loader.js
} from '@dioramap/scene-base';
```

Subpaths are exhaustively listed in `package.json` `exports`.
