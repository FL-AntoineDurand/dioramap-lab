# scene-base / utilities

Small shared helpers used by viz modules and the apps that consume them.

| File | Export | Purpose |
|---|---|---|
| `leaf-sprite.js` | `generateLeafTexture(size, count)` | Procedurally renders a deterministic-RNG-seeded canvas of `count` leaf-shaped ellipses with HSL variation, returns a `THREE.CanvasTexture`. Used by `canopy.viz.js` to texture the leaf billboards without shipping a PNG asset. |
| `gltf-loader.js` | `loadGLBModel(url, opts)` | Thin promise wrapper around `GLTFLoader.load`. Used by stand-alone callers that don't go through the SDK's models system. (The full models system at `apps/viewer-sdk/src/models/` does its own loader-handling because it needs Y-up→Z-up rotation, auto-scale, ground-snap, and shadow flags.) |

These are pure modules — no Three.js scene state. Add new utilities here
if they're broadly reusable across viz modules; if a helper is specific
to one composer, keep it inline in that composer instead.
