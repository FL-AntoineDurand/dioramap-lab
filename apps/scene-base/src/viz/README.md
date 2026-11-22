# scene-base / viz

Composite-visualizer modules — multi-artifact `.viz.js` files that paint
the production scene shared between **lab-ui** and the **viewer SDK**.

For the umbrella vocabulary (composite vs builtin vs step) see
[`v3/docs/VISUALIZERS.md`](../../../../docs/VISUALIZERS.md). This
directory is **only** the composite-visualizer subtype.

## Module shape

Every `*.viz.js` exports:

```js
export const meta = {
  name: 'Human-readable label',
  requires: ['artifact-1', 'artifact-2', ...],   // artifact names this composer needs
  pipelines: '*' | ['pipeline-id', ...],         // restrict by pipeline id
  default: true | false,                          // auto-render on pipeline load?
  supersedes: ['leaf-id-A', ...],                 // hide these visibility leaves once we render
};

export function render(ctx, decoded) { ... }    // one-shot path

// Optional streaming triplet — for artifacts the runner emits as chunks:
export function beginStream(ctx, info) { ... }
export function appendChunk(ctx, decoded, info) { ... }
export function finalizeStream(ctx, info) { ... }
```

`ctx` is a `BaseRenderContext` (see `../scene/render-context.js`).
`decoded` is `{[artifactName]: decodedPayload}` covering every
`meta.requires` entry.

## Current modules

| Module | Reads | Renders |
|---|---|---|
| `lidar-positions.viz.js` | `lidar-positions` | Streaming preview point cloud (blue dots). Capacity-grown `BufferAttribute` so the cloud assembles smoothly during the run. Layer leaf: `lidar-positions-preview`. |
| `parcel-lines.viz.js` | `parcel-lines`, `scene-origin` | Cadastre outlines as `THREE.LineSegments` (yellow, depthWrite=false). Layer: `parcels`. |
| `point-cloud-discs.viz.js` | `lidar-positions`, `lidar-normals`, `lidar-spacing`, `lidar-classifications`, `canopy-flags`, `grass-flags`, `deshadowed-ortho-rgb-rasters`, `canonical-location`, `scene-origin` | Two oriented-disc InstancedMesh outputs: a parabolic-dome variant for ground (with hash noise + flat normal + EDL bypass + grass tint + manual `getShadowMask()` modulation), and a flat-hex variant for everything non-ground. Per-vertex ortho sampling, radial alpha fade, 25%-scale solid-hex shadow caster. `meta.supersedes = ['lidar-positions-preview']`. Layers: `discs-ground`, `discs-other` under `Point cloud` parent. |
| `canopy.viz.js` | `lidar-positions`, `canopy-flags`, `scene-origin` | Leaf-textured billboards (2 instances per canopy point with random tilt; alpha-discard via custom shader). Layer: `canopy` under `Point cloud` parent. |
| `elevation-surround.viz.js` | `elevation-surround-f32`, `ortho-surround-jpeg`, `canonical-location`, `scene-origin` | Vertex-displaced `PlaneGeometry` covering ±600 m around canonical centre, textured with the WMS aerial JPEG and colour-graded (S-curve contrast / saturation +40 % / vegetation boost / levels remap [0.08, 0.92]→[0,1]). Index-buffer culls every triangle touching a no-data vertex. Inner-radius hole at the lidar disc footprint. Layer: `surround`. |
| `terrain.viz.js` | `lidar-ground-grid`, `deshadowed-ortho-rgb-rasters`, `canonical-location`, `scene-origin` | Heightmap mesh from the lidar ground grid, textured with the deshadowed RGB. **Excluded from the SDK** (the disc cloud already paints the ortho per-vertex; a separate ground plane would z-fight). Lab-ui keeps it for debug comparison. Layer: `terrain`. |
| `deshadowed-ortho.viz.js` | `deshadowed-ortho-rgb-rasters`, `canonical-location`, `scene-origin` | Flat ortho plane at `z=-0.5 m`. **Excluded from the SDK** — same reasoning as `terrain.viz`. Layer: `ground-ortho`. |
| `ortho-surround-jpeg.viz.js` | `ortho-surround-jpeg`, `canonical-location`, `scene-origin` | Flat JPEG plane at `z=-2 m`. Layer: `surround-rgb`. **Excluded from the SDK** (the displaced `elevation-surround` mesh supersedes it). |

`index.js` re-exports a `vizCatalog` (Vite-glob discovery) + a
`filterByCatalog(availableArtifacts)` helper used by lab-ui's
composite-views panel. The SDK does NOT import `index.js` — it imports
only the specific composers it needs by subpath.

## Adding a new composite

1. Create `<name>.viz.js` here. Implement `meta` + `render` (+ optional
   streaming triplet).
2. Add a subpath entry to `package.json` `exports`.
3. If the SDK should ship it: add an explicit `import` in
   `apps/viewer-sdk/src/viewer.js` `ORCHESTRATION` and bump the SDK's
   CHANGELOG. Lab-ui auto-discovers it via the Vite glob.

## SDK / lab-ui split

| Composer | SDK ships? | Lab-ui shows? |
|---|---|---|
| `lidar-positions` (preview) | ✓ | ✓ |
| `parcel-lines` | ✓ | ✓ |
| `point-cloud-discs` | ✓ | ✓ |
| `canopy` | ✓ | ✓ |
| `elevation-surround` | ✓ | ✓ |
| `terrain` | ✗ | ✓ (debug compare) |
| `deshadowed-ortho` | ✗ | ✓ (debug compare) |
| `ortho-surround-jpeg` | ✗ | ✓ (debug compare) |

`scripts/check-bundle-size.js` in viewer-sdk asserts the SDK exclusion
holds at build time.
