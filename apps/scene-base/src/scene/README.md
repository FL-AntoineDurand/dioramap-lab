# scene-base / scene

Three.js scene infrastructure: renderer, cameras, controls, post-process,
fog, sun, visibility tree, render context.

| File | Class / API | What it owns |
|---|---|---|
| `create-scene.js` | `Viewer` | The central scene object: `THREE.Scene`, `WebGLRenderer`, perspective + ortho cameras, `OrbitControls`, sky background, fog, three-light rig (hemi + ambient + sun directional), shadow map, optional EDL post-process via `EffectComposer`, declarative `addLayer`/`removeLayer` for primitive specs (`points`, `ground-raster`) and pre-built `THREE.Object3D`. Internal rAF render loop with `maxFps` cap. `dispose()` cancels the rAF, disconnects the resize observer, and tears down GPU resources. |
| `compass-gizmo.js` | `MarineCompass` | DOM overlay (SVG marine-compass dial) that reads the active camera each frame and renders the bearing + tilt. Injects its own stylesheet once. |
| `postprocess/edl.js` | `createEDLPass()` | Eye-Dome Lighting fragment shader as a Three.js `ShaderPass`. Reads the composer's depth texture, computes 8-neighbour log-depth deltas, darkens pixels in front of farther neighbours. Honours an `alpha < 0.002` per-pixel bypass marker (used by the dome ground discs). |
| `render-context.js` | `BaseRenderContext` | The `RenderContext` API surface visualizers call into (`render3D`, `renderSVG`, `renderChart`, `renderJSON`, `renderHTML`, `onCleanup`, `dispose`). Bridges visualizer output to the host's `Viewer` + `SceneState`. See [VISUALIZERS.md §3](../../../../docs/VISUALIZERS.md) for the full contract. |
| `scene-state.js` | `SceneState`, `createSceneState` | Two coupled surfaces: `sun` (a `SunControl`, see `sun.js`) and `visibility` (a hierarchical leaf tree; tri-state cascades; events for declarations + visibility changes). The lab-ui's gear-popover Layers panel and the SDK's `EXPOSED_VISIBILITY_TOGGLES` both read from this. |
| `sun.js` | `SunControl` | Drives the scene's `THREE.DirectionalLight`. Two input paths: `setFromDate({...})` (computes direction client-side via SunCalc — used by the slider) and `setFromSunDirection({sun_dx, sun_dy, sun_dz})` (one-shot init from the v3 `sun-direction` artifact). Both converge on `_applyDirection` which positions the light along the unit direction toward the sun. |

## Lifecycle expectations

Mount once per page; tear down via `Viewer.dispose()` on unmount. The
SDK's `mountViewer` and lab-ui's `app.js` both call `dispose()` in their
own teardown paths.

`SceneState.visibility.attach` is the single entry point for visibility
leaves. Every `RenderContext.render3D({layer:{id,...}})` call ends up here.
Removing a layer via `clearSubtree(id)` cascades the visibility tree.

## Coordinate convention

All scene content is **Z-up Lambert-93 metres**, recentered onto a shared
`sceneOrigin` chosen by whichever layer arrives first (typically the
point cloud's centroid). Subsequent layers read `getSceneOrigin()` and
offset their L93 coordinates so everything lives in the same local frame
with WebGL-F32-safe magnitudes.
