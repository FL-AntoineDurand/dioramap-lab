# Viewer SDK + Scene-Base Port Plan

*Phase C2 of [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md) §12.3. Covers the two packages that together ship the production embedded viewer — `v3/apps/viewer-sdk/` and its shared rendering dependency `v3/apps/scene-base/` — and how `v3/apps/lab-ui/` consumes the same scene-base for developer inspection.*

*Scope: port `lidar-3d-environment-viewer/frontend/` to v3. Make the SDK a paper-thin production extract that imports all rendering from `@dioramap/scene-base` and all WS plumbing from `@dioramap/protocol`. Lab-ui can reproduce the SDK's scene for development inspection via the same scene-base.*

---

## 1. Why this doc exists — the framing

The earlier draft of the SDK port was written as if the SDK were a self-contained app that happens to share a WS client with lab-ui. The corrected framing is much stronger:

> **The SDK is a production-only extract of lab-ui.**

Every rendering primitive, every piece of 3D scene state logic, every bit of Three.js infrastructure lives in a shared package. The SDK imports a closed, versioned set of those primitives. Lab-ui imports the same primitives and optionally discovers new ones at build time for dev iteration. When a renderer improves in scene-base, lab-ui sees it next build; when the SDK ships, it sees the same improvement automatically.

This plan therefore covers two packages at once, because they are tightly coupled and the SDK plan is meaningless without the scene-base plan that unblocks it:

1. **`@dioramap/scene-base`** — the new shared package containing all Three.js scene infrastructure, every renderer, and the SceneState surface (sun + visibility tree).
2. **`v3/apps/viewer-sdk/`** — the production embed, reduced to its true owned surface: public API, WS orchestration, a small controls-panel DOM, the GLB model placement system, and the loading bar.

## 2. Scope

### 2.1 Source tree — `lidar-3d-environment-viewer/frontend/`

Fixed-target scope. Other directories in the legacy repo (`src/`, `backend/`, `location-picker/`, `seller-demo/`) are out of scope.

```
lidar-3d-environment-viewer/frontend/src/   4265 LOC
├── sdk.js               (208)   → viewer-sdk (public API surface)
├── main.js              (82)    → viewer-sdk (standalone dev page entry)
├── viewer.js            (772)   → viewer-sdk (shrinks to ~250 LOC orchestrator)
├── client.js            (101)   → deleted (use @dioramap/protocol)
├── protocol-messages.js (127)   → deleted (use @dioramap/protocol)
├── scene.js             (320)   → @dioramap/scene-base
├── point-cloud/*        (677)   → @dioramap/scene-base/viz/
├── terrain/builder.js   (223)   → @dioramap/scene-base/viz/
├── textures/leaf-sprite.js (86) → @dioramap/scene-base/utilities/
├── objects/gltf-loader.js  (56) → @dioramap/scene-base/utilities/
├── postprocess/edl.js   (132)   → @dioramap/scene-base/scene/postprocess/
├── ui/compass-gizmo.js  (134)   → deleted (lab-ui's version wins, moves to scene-base)
├── ui/controls-panel.js (137)   → viewer-sdk (DOM only, ~90 LOC after port)
├── debug/compass.js     (131)   → deleted (debug-only)
├── coords/*             (126)   → viewer-sdk
├── models/*             (660)   → viewer-sdk (SDK-only feature)
└── loading/*            (264)   → viewer-sdk
```

Net viewer-sdk source after the port: **~1500 LOC** (from 4265). The drop is redistribution into scene-base, not deletion.

### 2.2 Non-goals

- Porting `lidar-3d-environment-viewer/backend/`, `src/`, `location-picker/`, `seller-demo/`.
- Owning any rendering code, scene-state logic, or WS protocol code in viewer-sdk.
- New scene features (camera modes, VR, mobile touch optimizations).
- Public API redesign — `DioramapViewer.init(...)` signature is frozen.
- A framework migration (React, Vue, etc.).
- Any debug, dev, or inspection surface in the SDK — those live in lab-ui.
- A pipeline variant smaller than `viewer-full`.
- Photo-meta seeding for `find-best-sun-time` (brute-force search already scores 0.41 on Notre-Dame, "excellent" per the JS bench).

## 3. Package responsibilities

Three packages, three clean responsibilities:

```
v3/apps/
├── protocol/            (@dioramap/protocol)        — WS client, artifact decoder
├── scene-base/          (@dioramap/scene-base)      — Three.js scene + composite-visualizers + SceneState
├── location-picker/     (@dioramap/location-picker) — already exists
├── lab-ui/              (consumer)
└── viewer-sdk/          (consumer)
```

| Package | Owns | How SDK uses | How lab-ui uses |
|---|---|---|---|
| `@dioramap/protocol` | `WSClient`, `decodeArtifact`, `AK` constants, streaming chunk pairing | Imports `WSClient`; opens one connection per mount | Same — already shipped with streaming support |
| `@dioramap/scene-base` | Scene infra, per-artifact viz, multi-artifact composers, **SceneState surface** (sun + visibility tree), utilities | Imports composer modules **explicitly** from a closed allow-list; binds a DOM controls panel to `sceneState.sun` + a curated subset of `sceneState.visibility` | Uses Vite-glob discovery over `viz/*`; binds its 3D-scene-parameters panel to `sceneState.sun` + the **full** `sceneState.visibility` tree |
| `viewer-sdk` (this plan) | Public `DioramapViewer.init(...)` API, WS orchestration, controls-panel DOM, GLB models system, loading bar, public library bundle | — | — |

## 4. `@dioramap/scene-base`

The single source of truth for everything Three.js.

### 4.1 Layout

```
v3/apps/scene-base/
├── package.json   (name: "@dioramap/scene-base", private: true)
├── src/
│   ├── index.js                          (re-exports)
│   ├── scene/
│   │   ├── create-scene.js               (renderer + camera + camera-controls + sky + fog + lights + EDL wiring)
│   │   ├── compass-gizmo.js              (lab-ui's version)
│   │   ├── postprocess/edl.js            (Eye-Dome Lighting pass)
│   │   ├── sun.js                        (client-side SunCalc → DirectionalLight setter)
│   │   ├── scene-state.js                (SceneState: sun state + visibility tree + events)
│   │   └── render-context.js             (ctx; bridges composer calls into scene + SceneState + optional host hooks)
│   ├── utilities/
│   │   ├── leaf-sprite.js                (procedural canopy leaf texture)
│   │   └── gltf-loader.js                (Three's GLTFLoader wrapper)
│   └── viz/
│       ├── index.js                      (Vite glob → vizCatalog, lab-ui only)
│       │
│       ├── parcel-lines.viz.js           (single — line segments)
│       ├── deshadowed-ortho.viz.js       (single — ground texture plane)
│       ├── ortho-surround-jpeg.viz.js    (single — surround backdrop)
│       ├── elevation-surround.viz.js     (single — surround terrain)
│       ├── lidar-positions.viz.js        (single, streaming — fast preview points)
│       │
│       ├── terrain.viz.js                (multi: lidar-ground-grid + deshadowed-ortho-rgb-rasters + scene-origin)
│       ├── point-cloud-discs.viz.js      (multi: positions + normals + classifications + ortho + scene-origin + spacing)
│       ├── canopy.viz.js                 (multi: positions + normals + canopy-flags + ortho + leaf-sprite)
│       /* full-scene.viz.js cancelled — see §7 S6 */
└── README.md
```

### 4.2 Composite-visualizer module contract

The full contract (`meta`, `render`, optional streaming triplet, `RenderContext`'s five surfaces, lifecycle) lives in [VISUALIZERS.md §3–§5](../VISUALIZERS.md). Two SDK-relevant illustrations follow.

**Single-leaf composite-visualizer:**

```js
// @dioramap/scene-base/viz/canopy.viz.js

export const meta = {
  name: 'Canopy (leaf-textured discs)',   // human label
  requires: ['lidar-positions', 'lidar-normals', 'canopy-flags',
             'deshadowed-ortho-rgb-rasters', 'scene-origin'],
  pipelines: '*',                          // or a specific list
  default: true,                           // lab-ui: pre-checked in composite-views panel
};

// One-shot render: fires when all `requires` artifacts are present.
// `decoded` is { [artifactName]: decoded-typed-data }.
export function render(ctx, decoded) {
  const canopyMesh = buildCanopyMesh(decoded);
  // ctx.render3D declares a single leaf layer at the tree root.
  ctx.render3D([{
    kind: 'instanced-mesh',
    object: canopyMesh,
    layer: { id: 'canopy', label: 'Trees (canopy)', defaultVisible: true },
  }]);
}

// Optional progressive-streaming triplet (PLAN-VIEWER-PORT §7.3 contract).
export function beginStream(ctx, info) { /* ... */ }
export function appendChunk(ctx, decoded, info) { /* per-chunk */ }
export function finalizeStream(ctx, info) { /* on final=true */ }

// Cleanup on layer removal / scene unmount.
export function dispose(ctx) { /* free Three.js resources */ }
```

**Tree-emissive composite-visualizer** (boundary-3d-style) — one `render` call adds many leaves into the visibility tree by passing `path`:

```js
export function render(ctx, decoded) {
  for (const layer of decoded['boundary-3d'].layers) {
    ctx.render3D([{
      kind: layer.primitive,                     // 'lines' | 'cylinders' | ...
      object: buildThreeGroup(layer.items),
      layer: {
        id:    layer.id,
        label: layer.label,
        path:  [layer.category, layer.subcategory, layer.group].filter(Boolean),
        defaultVisible: layer.defaultVisible,
      },
    }]);
  }
}
```

#### What `ctx.render3D` actually does (per host)

| Effect | Lab-ui | SDK |
|---|---|---|
| Add `spec.object` to the Three.js scene graph | ✔ | ✔ |
| Declare `{id, label, path, defaultVisible}` in `sceneState.visibility` | ✔ | ✔ |
| Mark the artifact-inspector row with a `3D` surface badge | ✔ | — (no inspector) |

In the SDK the four non-3D surfaces (`renderSVG`/`renderChart`/`renderJSON`/`renderHTML`) are no-ops. See [VISUALIZERS.md §4](../VISUALIZERS.md) for the full per-surface table.

### 4.3 SceneState surface (`scene/scene-state.js`)

**SceneState is 3D-scene-scoped.** It holds everything about the 3D scene's current settings and visible-groups state. It does NOT own artifact navigation, inspector rows, SVG/Chart/JSON/HTML rendering, or anything outside the 3D scene — those belong to lab-ui's `OverlayPanel` (§6).

Both the SDK's user-facing controls panel and lab-ui's "3D scene parameters" panel need the same underlying state + effect logic. SceneState ships it once; each consumer ships its own UI chrome that binds to it.

#### 4.3.1 Top-level API

```js
import { createSceneState } from '@dioramap/scene-base/scene/scene-state';

const sceneState = createSceneState(sceneCtx, { lat, lon });
// → { sun, visibility, addEventListener, dispatchEvent, ... }
```

#### 4.3.2 `sceneState.sun`

Unchanged from the earlier proposal, just under the new namespace:

```js
sceneState.sun.setFromSunDirection({ sun_dx, sun_dy, sun_dz });   // one-shot at mount (from artifact)
sceneState.sun.setFromDate({ year, month, day, hour, minute });   // per slider movement
sceneState.sun.setEnabled(true);
sceneState.sun.state;   // { date, dir, enabled }

sceneState.addEventListener('sun-changed', (ev) => { /* ev.detail = sun.state */ });
```

SunCalc lives inside `sun.js`. Neither consumer calls SunCalc directly.

#### 4.3.3 `sceneState.visibility` — the layer tree

The v2 `boundary-3d` artifact proved that one renderer can emit **dozens of 3D-scene groups** organized as a 4-level tree (category → subcategory → group → leaf). The model has to support that from day one; simpler composers use a 1-node tree (leaf at root).

**Declare / attach**:

```js
// Single leaf at root (simple composers):
sceneState.visibility.declare({
  id: 'canopy',
  label: 'Trees (canopy)',
  defaultVisible: true,
});
sceneState.visibility.attach('canopy', canopyMesh);

// Leaf under a path (tree-emissive composers):
sceneState.visibility.declare({
  id: 'rg-2-plane-geometry-edges',
  label: 'Edges (47)',
  path: ['Roof Graph', '2-plane-geometry', 'plane-4'],
  defaultVisible: false,
});
sceneState.visibility.attach('rg-2-plane-geometry-edges', threeGroup);
```

Composers typically go through `ctx.render3D(...)` rather than calling `declare` + `attach` directly, but the underlying API is here.

**Queries**:

```js
sceneState.visibility.tree();           // → full hierarchy for UI render
sceneState.visibility.flatList();       // → every leaf (compatible with the SDK's simple list UI)
sceneState.visibility.node(idOrPath);   // → { ...node, visibleDerived: 'all'|'none'|'some' }
```

**Actions (cascading)**:

```js
sceneState.visibility.setVisible(idOrPath, bool);
// - on a leaf: flips the leaf's state, traverses attached Three.js objects, fires 'visibility-changed'
// - on a group node: cascades to every descendant leaf, fires ONE 'subtree-changed' with the affected leaves

sceneState.visibility.hideAll();
sceneState.visibility.showAll();
sceneState.visibility.setDefaults();   // every leaf → its defaultVisible
```

**Events**:

```js
sceneState.addEventListener('visibility-registered', (ev) => { /* ev.detail = { node } */ });
sceneState.addEventListener('visibility-changed',    (ev) => { /* ev.detail = { nodeId, path, visible } */ });
sceneState.addEventListener('subtree-changed',       (ev) => { /* ev.detail = { pathRoot, leavesAffected } */ });
sceneState.addEventListener('subtree-cleared',       (ev) => { /* on composer dispose */ });
```

**Internal tree shape** (sketch, not public API):

```js
// sceneState.visibility._root
{
  children: Map {
    'Roof Graph' → {
      children: Map {
        '2-plane-geometry' → {
          children: Map {
            'plane-4' → {
              children: Map {
                'rg-2-plane-geometry-edges' → { id, label, objects: [group], visible, defaultVisible }
              }
            }
          }
        }
      }
    }
    'canopy'  → { id: 'canopy',  label, objects: [mesh], visible, defaultVisible }   // root-level leaf
    'parcels' → { id: 'parcels', label, objects: [lines], visible, defaultVisible }  // root-level leaf
  }
}
```

#### 4.3.4 Tri-state visibility on group nodes

A group node's derived visibility follows the v2 UX:
- All descendant leaves `visible: true`    → group shows **checked**
- All descendant leaves `visible: false`   → group shows **unchecked**
- Mixed                                    → group shows **indeterminate**

`sceneState.visibility.node(groupPath).visibleDerived` returns one of `'all' | 'none' | 'some'`. UIs render the tri-state checkbox directly from that.

### 4.4 Discovery via Vite glob — **lab-ui only**

`scene-base/src/viz/index.js` ships a Vite-glob entry exporting `vizCatalog` + `filterByCatalog(availableArtifacts)`. Lab-ui imports `vizCatalog` and filters to the pipeline's available artifacts; entries surface in the composite-views panel (PLAN-LAB-UI-PORT L10). Per [VISUALIZERS.md §1](../VISUALIZERS.md), the SDK never imports this entry — it would defeat tree-shaking — and uses the closed allow-list in §5.2 instead.

### 4.5 The mega-composer (`full-scene.viz.js`) — **dropped**

Originally planned as a single composite that would bundle every SDK composite for a lab-ui "Show production scene" toggle. Cancelled at S6 once L10 shipped — the composite-views panel already toggles individual composites and the SDK set auto-renders via `default: true`. See §7 S6 for the full rationale.

### 4.6 The Concept-B exception (real algorithmic packing promoted to a v3 step)

Some artifacts a multi-artifact composer consumes are wasteful to ship whole when only a fraction is needed. Where this is true, a v3 algorithmic step earns its place (e.g. `extract-canopy-points` reads positions + canopy-flags, writes `canopy-positions`). Pure packing-for-rendering stays client-side as multi-artifact viz.

Decision rule: **promote to a v3 step only when the projection has algorithmic meaning beyond rendering convenience.** Not shipped at S1; re-evaluated if cold-mount latency or wire bytes become a problem.

### 4.7 Where visualizers live — composite-visualizers vs the rest

**Canonical reference: [VISUALIZERS.md](../VISUALIZERS.md)** — vocabulary, the three subtypes (builtin / step / composite), the per-subtype lifecycle, and the lab-ui ↔ SDK UI mapping all live there.

This plan only owns the **composite-visualizer** subtype (`apps/scene-base/src/viz/*.viz.js`) — everything the SDK imports. Step-visualizers and builtin-visualizers are lab-ui's surface; the SDK never imports from `v3/steps/<step>/viz/` or `apps/lab-ui/src/renderers/builtin/`.

**Rule of thumb (from VISUALIZERS.md §2)**: if the SDK imports it → composite-visualizer (here). Otherwise → step-visualizer (next to its step) or builtin-visualizer (next to lab-ui's kind dispatcher).

Concrete examples for this plan's scope:
- `canopy.viz.js`, `terrain.viz.js`, `point-cloud-discs.viz.js` (multi-artifact, SDK uses them) → **scene-base**
- `parcel-lines.viz.js`, `lidar-positions.viz.js` (single-artifact, SDK uses them) → **scene-base**
- `boundary-3d.viz.js`, `ndsm.viz.js`, debug-tier views → **step-visualizers, NOT scene-base** — they don't ship in the SDK.

## 5. `v3/apps/viewer-sdk/`

The production embed. Small, versioned, closed.

### 5.1 Module layout

```
v3/apps/viewer-sdk/
├── package.json   (deps: "@dioramap/protocol": "file:../protocol",
│                         "@dioramap/scene-base": "file:../scene-base",
│                         "three": "^0.169.0")
├── vite.config.js                (library build → dist/)
├── index.html                    (standalone dev page)
├── public/
│   └── models/                   (GLB samples for the placement system)
├── src/
│   ├── sdk.js                    (DioramapViewer.init — public API, ~200 LOC)
│   ├── main.js                   (standalone dev page wrapper, ~80 LOC)
│   ├── viewer.js                 (WS orchestration: opens WSClient, sends RUN,
│   │                              calls composers, manages lifecycle. ~250 LOC)
│   ├── controls-panel.js         (DOM + layout ONLY; sun slider + curated
│   │                              visibility-toggle allow-list + models-controls
│   │                              hooks. Binds to sceneState.sun and a filtered
│   │                              subset of sceneState.visibility. ~90 LOC)
│   ├── loading-bar.js            (loading bar UI + progress-tracker, ~265 LOC)
│   ├── coords/                   (lambert93 + scene-origin helpers, ~125 LOC)
│   └── models/                   (GLB placement: model-manager + interaction +
│                                  ghost-placement; ~660 LOC; SDK-only feature)
├── CHANGELOG.md                  (SDK semver per UNIFIED §4.10)
└── README.md
```

### 5.2 Closed composer allow-list

The SDK imports composers explicitly — **a fixed, versioned allow-list**:

```js
// viewer-sdk/src/viewer.js
import * as terrain       from '@dioramap/scene-base/viz/terrain.viz';
import * as discs         from '@dioramap/scene-base/viz/point-cloud-discs.viz';
import * as canopy        from '@dioramap/scene-base/viz/canopy.viz';
import * as parcels       from '@dioramap/scene-base/viz/parcel-lines.viz';
import * as ortho         from '@dioramap/scene-base/viz/deshadowed-ortho.viz';
import * as preview       from '@dioramap/scene-base/viz/lidar-positions.viz';
import * as surroundOrtho from '@dioramap/scene-base/viz/ortho-surround-jpeg.viz';
import * as surroundElev  from '@dioramap/scene-base/viz/elevation-surround.viz';
```

| | Lab-ui | SDK |
|---|---|---|
| Composer discovery | Vite glob → `vizCatalog` | Explicit `import` list |
| Adding a new composer | Appears automatically | Requires SDK code change + rebuild + CHANGELOG + release |
| Intent | Dev cockpit, exploratory | Versioned production artifact |

### 5.3 Orchestration shape

```js
import { WSClient, AK, decodeArtifact } from '@dioramap/protocol';
import { createScene } from '@dioramap/scene-base/scene/create-scene';
import { createSceneState } from '@dioramap/scene-base/scene/scene-state';
import { createRenderContext } from '@dioramap/scene-base/scene/render-context';
import { mountControlsPanel } from './controls-panel';   // SDK-local DOM
// ...explicit composer imports per §5.2...

const ORCHESTRATION = [
  { mod: terrain,        id: 'terrain'       },
  { mod: discs,          id: 'discs'         },
  { mod: canopy,         id: 'canopy'        },
  { mod: parcels,        id: 'parcels'       },
  { mod: ortho,          id: 'ground-ortho'  },
  { mod: preview,        id: 'preview'       },  // streaming
  { mod: surroundOrtho,  id: 'surround-rgb'  },
  { mod: surroundElev,   id: 'surround-elev' },
];

export async function mountViewer({ container, backendUrl, lat, lon, ... }) {
  const sceneCtx = createScene(container);
  const sceneState = createSceneState(sceneCtx, { lat, lon });
  mountControlsPanel(container, sceneState);   // SDK's sun-+-allow-listed-toggles DOM
  const ws = new WSClient(backendUrl);

  const buffer = new Map();        // artifact-name → decoded data

  ws.addEventListener('artifact', ({ detail }) => { /* decode + buffer + triggerReady() */ });
  ws.addEventListener('stream-chunk', ({ detail }) => { /* route to progressive composer */ });
  ws.addEventListener('session', () => ws.run('viewer-full', { lat, lon, radius_m: 75 }, SUBSCRIBE_LIST));
  ws.connect();

  return {
    setSunTime: (t) => sceneState.sun.setFromDate(t),  // delegates; client-side
    addModel: ...,                                     // delegates to models/
    dispose: () => { ws.cancel(); ws.close(); sceneCtx.dispose(); },
  };
}
```

`triggerReady()` checks each `ORCHESTRATION` entry: when all `mod.meta.requires` land in the buffer, it calls `mod.render(ctx, gather(decoded))` with an SDK-flavoured `ctx` (3D-only, non-3D surface methods are no-ops). Progressive composers route `stream-chunk` events through their `beginStream/appendChunk/finalizeStream` triplet.

### 5.4 Subscribe list

Hardcoded; matches the artifacts the allow-listed composers need.

```
canonical-location, scene-origin, sun-direction,
lidar-positions, lidar-normals, lidar-classifications,
canopy-flags, grass-flags,
lidar-ground-grid,
deshadowed-ortho-rgb-rasters,
ortho-surround-jpeg, elevation-surround-f32,
parcel-lines
```

13 artifacts. Pipeline = `viewer-full`. Server-side subscription gating (B10) prunes everything else.

### 5.5 Controls panel — sun + **curated visibility allow-list** + models

The SDK user is placing GLB sheds and garden features. They need a few contextual toggles (cadastre outlines, trees visibility, surround backdrop), not a full scene-decomposition tree.

The SDK ships an **explicit allow-list** of visibility node IDs to surface as user toggles:

```js
// viewer-sdk/src/controls-panel.js — the SDK's curated set
const EXPOSED_VISIBILITY_TOGGLES = [
  { nodeId: 'parcels',       label: 'Cadastre outlines' },
  { nodeId: 'canopy',        label: 'Trees' },
  { nodeId: 'surround-rgb',  label: 'Surround tile' },
  // ... curated per SDK release
];
```

Layers outside this list **are still declared** in `sceneState.visibility` (composers always call `ctx.render3D`, which always declares). They just aren't rendered as user-facing checkboxes in the SDK DOM. Their initial visibility follows `defaultVisible` and stays as declared unless user toggles the ones that ARE exposed.

This gives the SDK:
- A tight, curated UX ("Cadastre outlines" not "Category / Subcategory / Group / Edges (47)")
- A versioned product decision per release (adding a toggle = doc'd minor version bump)
- Zero coupling to whatever tree shape a composer declares internally

```js
// viewer-sdk/src/controls-panel.js — ~90 LOC, DOM only

export function mountControlsPanel(container, sceneState) {
  const panel = buildPanelDOM(container);

  // Sun section
  const sunUI = buildSunSliderDOM(panel);
  sunUI.slider.addEventListener('input', (e) =>
    sceneState.sun.setFromDate(parseDate(e.target.value)));
  sceneState.addEventListener('sun-changed', (ev) => sunUI.reflectState(ev.detail));

  // Curated visibility toggles
  const rowEls = new Map();
  for (const t of EXPOSED_VISIBILITY_TOGGLES) {
    const cb = addCheckboxRow(panel, t.label);
    cb.addEventListener('change', () => sceneState.visibility.setVisible(t.nodeId, cb.checked));
    rowEls.set(t.nodeId, cb);
  }
  sceneState.addEventListener('visibility-changed', (ev) => {
    const cb = rowEls.get(ev.detail.nodeId);
    if (cb) cb.checked = ev.detail.visible;
  });

  // Models section is built separately by models/model-manager.js.
}
```

### 5.6 Sun slider — 100% client-side

- SunCalc lives in `@dioramap/scene-base/scene/sun.js`
- Slider calls `sceneState.sun.setFromDate({...})` → SunCalc → Three.js `DirectionalLight` rotates
- **Zero WS traffic**. No re-RUN, no backend involvement.
- `sun-direction` artifact from `find-best-sun-time` only initializes the slider position so the light matches what the deshadow was baked against. After that, slider moves freely.

### 5.7 Public API + stability

`DioramapViewer.init(...)` signature preserved so agri-map.fr flips one URL and stops.

```js
import DioramapViewer from '/dioramap-viewer.js';
const viewer = await DioramapViewer.init({
  container: '#viewer',
  apiKey: 'pk_live_xxxxx',
  lat: 48.576,
  lon: -2.568,
  backendUrl: 'wss://app.dioramap.com/ws',
  radius: 75,
  features: {},
  onReady, onModelSelected, onModelMoved, onModelDeselected, onError,

  // Optional render-quality feedback widget (added in 0.7.0 — see
  // v3/docs/plan/PLAN-RENDER-FEEDBACK.md). When `enabled` and
  // `embedderUserHash` is a 64-char sha256 hex, the SDK auto-mounts a
  // rating pill inside the container 60s after run-complete (override
  // via `autoPromptDelayMs`). Submission goes to dioramap CP's public
  // POST /api/feedback/render. Disabled silently if `embedderUserHash`
  // is missing/malformed — feedback is non-essential, never throws.
  feedback: {
    enabled: true,
    embedderUserHash: '<sha256(user.id + pepper)>',
    autoPromptDelayMs: 60_000,
    backendUrl: 'https://app.dioramap.com', // default; matches WS host
    onSubmitted: ({ rating, hasComment }) => {}, // optional embedder UI gesture
  },
});
```

**Removed from the API**: `devTools` (no replacement — use lab-ui).

**Default backend correction (v0.7.0)**: was `wss://api.dioramap.com/ws` — `api.` was a stale subdomain that doesn't exist in our infra. Embedders who pass `backendUrl` explicitly (everyone today) are unaffected; the fix matters for the standalone seller-demo case.

**External CSS + theming surface (v0.8.0)**: the SDK no longer injects `<style>` tags at runtime. Embedders MUST add `<link rel="stylesheet" href=".../dioramap-viewer.css">` alongside the JS bundle. In return, `'unsafe-inline'` is no longer required in their `style-src` / `script-src` CSP. Theming is now declared via `--dioramap-*` CSS custom properties (17 vars on `:root`); the picker mirrors with `--dioramap-picker-*`. Class names unified to a `.dioramap-*` prefix (`.vw-compass-*` → `.dioramap-compass-*`). Public class list + variable table documented in `landing/docs/api/viewer-sdk/#styling`. See §5.10 for the architecture and `0.8.0` CHANGELOG for the migration list.

SDK semver tracked in `apps/viewer-sdk/CHANGELOG.md`, independent of artifact versions per UNIFIED §4.10.

### 5.8 Bundle-size invariant — tree-shaking contract

Scene-base will grow as new composers and inspector viz land. The SDK bundle must **only** contain what it directly imports — growing scene-base must not inflate the SDK output.

This depends on two conventions:

1. **`@dioramap/scene-base/package.json` must declare `"sideEffects": false`.** Without this, Rollup/Vite conservatively keeps all reached modules, defeating tree-shaking. If any module in scene-base DOES have side effects (e.g. a CSS import), whitelist it: `"sideEffects": ["./src/some-side-effect.js"]`.

2. **The SDK must NEVER import `@dioramap/scene-base/viz/index.js`.** That file is the Vite glob entry for lab-ui's discovery — it eagerly imports every `viz/*.viz.js`. Importing it would drag the entire viz directory (and all its transitive Three.js-heavy deps) into the SDK bundle. The SDK only imports composers by direct path: `import * as terrain from '@dioramap/scene-base/viz/terrain.viz'`. Treat `viz/index.js` as lab-ui's private entry point.

Verification happens at V8: build the SDK bundle, assert the dist size matches only the allow-listed composer deps. A regression here means either `sideEffects` is missing or someone imported `viz/index.js`.

### 5.9 Debug / dev surface — deleted wholesale

| Deleted | Why |
|---|---|
| `debug/compass.js` | Superseded by scene-base's compass (lab-ui's version) |
| `MSG.DEBUG_RASTER` handler | SDK is hardcoded `public` tier |
| `JSON_MSG.SUBSCRIBE_DEBUG`, `REQUEST_DEBUG`, `DEBUG_CATALOG` | Same |
| `mountViewer` `devTools` param | Removed from API |
| `COREG_OFFSET`, `TILE_INFO`, per-step timing | Debug-flavored; never rendered in embed |
| `_scene` / `_camera` / `_renderer` internal leaks | Audited at V6; expose only what `models/` needs |
| Full visibility tree in controls panel | §5.5 — curated allow-list only |
| Console.log noise behind `if (debug)` | Gates + logs both removed |

### 5.10 CSP-friendly bundle + theming (v0.8.0)

The SDK is shipped CSP-strict: zero `<style>` injection, zero `setAttribute('style', …)`, zero `el.style.cssText = …`, zero `eval` / `new Function` / runtime `<script>` injection. Embedders can ship `script-src 'self' https://static.dioramap.com; style-src 'self' https://static.dioramap.com;` with no `'unsafe-inline'`. Per-frame property assignments (`el.style.transform = …`) are kept where dynamic toggling is needed — those are CSP-safe per CSP3 (style property, not attribute).

**Two assets per SDK now**: a JS bundle and a CSS bundle. Both are emitted by `vite build` (`build.cssCodeSplit: false`) into `dist/`. `dev-up.sh:link_sdk_bundles` symlinks both into the control-plane's static dir; the public CDN does the same on deploy.

**Theming**: rules reference `--dioramap-*` CSS custom properties declared on `:root` in the bundled stylesheet. Embedders override by loading their own `<link>` AFTER ours. Variable namespaces:

- `--dioramap-*` (viewer): surface bg/border/shadow, text primary/secondary/muted, accent + hover + disabled, danger + success, radius (3 sizes), font-ui + font-mono. **17 vars total.**
- `--dioramap-picker-*` (location-picker): confirm-button bg/hover/disabled, surface bg + shadows, divider, hover bg, hint banner colours, banner colours, radius, font. **13 vars total.**

Picker uses its own namespace so the two SDKs can coexist on the same page without colliding on `--dioramap-accent` etc. Both surfaces are stable within a major version; renames go through CHANGELOG + a major bump.

**Class-name surface**: documented in the API docs (`landing/docs/api/viewer-sdk/#styling`, `landing/docs/api/location-picker/#styling`). Single-class selectors only; no `!important` anywhere — so an embedder targeting the same selector with same specificity wins by virtue of declaration order.

**Source layout** (per package):

- `src/styles.css` — single bundle, imported once from the entry (`sdk.js`).
- Per-component JS files: zero CSS strings, only `el.className = 'dioramap-…'` assignments.

**Compass HUD class rename**: `.vw-compass-*` → `.dioramap-compass-*` (lab-ui's `layout/style.css` was updated in lockstep). Any embedder who was targeting the legacy prefix directly (undocumented) needs to rename — flagged in the 0.8.0 CHANGELOG.

## 6. Lab-ui's consumption of scene-base

Lab-ui has two independent UIs that touch the 3D scene; they are orthogonal concerns.

### 6.1 `OverlayPanel` — artifact inspector (existing, unchanged)

`apps/lab-ui/src/artifact-inspector/overlay-panel.js` is lab-ui's artifact navigation surface. One row per **loaded artifact**, with icons for each surface the renderer contributes (3D, SVG, Chart, JSON, HTML). Clicking a row focuses it + scrolls to its render block; × removes it.

This is **inspector-scoped**, not scene-scoped. It exists only in lab-ui; the SDK does not have one.

The overlay-panel is not being refactored or moved. It stays exactly as it is, because it is orthogonal to scene-base's SceneState. The only interaction: when a renderer calls `ctx.render3D(...)`, lab-ui's ctx implementation appends a `3d` surface marker to the artifact's row (one call, two effects — see §4.2).

One artifact row in the overlay-panel can correspond to **N** nodes in `sceneState.visibility` — a tree-emissive composer like boundary-3d produces **one inspector row** but **147 visibility leaves**. Both UIs show the right thing for their scope.

### 6.2 `3DSceneStatePanel` — new component, ports v2's UX

A new lab-ui component that binds to `sceneState`:
- **Sun section**: time slider with date fields — same widget the SDK uses, but with more room and dev info (azimuth/altitude readouts).
- **Visibility tree**: renders `sceneState.visibility.tree()` generically — directly ports v2's boundary-3d panel UX (`ui/public/app.js:3790-4035`):
  - Collapsible sections for each non-leaf level
  - Tri-state master checkbox per non-leaf (reads `visibleDerived`)
  - Leaf checkboxes
  - Text filter (substring match across full `category + subcategory + group + label` path)
  - **Deselect all** button (calls `sceneState.visibility.hideAll()`)
  - **Show all** / **Reset defaults** buttons

Because the widget renders `sceneState.visibility.tree()` generically, **any** composer that declares nodes with `path` gets this UI for free. The boundary-3d case just happens to produce the most nodes.

### 6.3 Lab-ui's composite-views panel

Tracked as PLAN-LAB-UI-PORT **L10**. Behavior + UI mapping in [VISUALIZERS.md §6.4](../VISUALIZERS.md): toggle list keyed off `vizCatalog` filtered by the current pipeline's catalog; pre-checked when `meta.default === true`; on → subscribe + dispatch; off → `ctx.dispose()`.

### 6.4 Mega-composite entry point — **dropped**

The S6 "Show production scene" mega-composite was cancelled (see §7 S6). Lab-ui devs reproduce the SDK's scene by toggling individual composites in the L10 panel; the SDK set carries `meta.default: true` so it auto-renders on pipeline load.

## 7. Port milestones (interleaved)

Each ships as a unit. `P*` = `@dioramap/protocol` package milestone; `S*` = `@dioramap/scene-base` milestone; `V*` = viewer-sdk milestone. `L*` cross-references in PLAN-LAB-UI-PORT.

**P1 — `@dioramap/protocol` package extraction.** ✅ done.
Created `v3/apps/protocol/` with `package.json` (`name: "@dioramap/protocol"`, `private: true`, `"sideEffects": false`, source-imported via Vite — no build step). Moved `ws-client.js` (152 LOC, STREAM_CHUNK + dual-delivery aware) and `artifact-decoder.js` (190 LOC, kind-dispatched) from lab-ui. Added `src/index.js` re-exporter. Updated `apps/lab-ui/package.json` with `"@dioramap/protocol": "file:../protocol"` and rewired four import sites in lab-ui (`app.js`, `renderers/loader.js`, `renderers/builtin/metadata-json.js`, `artifact-inspector/inspector.js`) to import from `@dioramap/protocol`. Validation: `npm run build` green (363 modules transformed cleanly); dev server starts in 133 ms; no behaviour change.

**S1 — Scene-base scaffold + scene infrastructure.** ✅ done.
Created `v3/apps/scene-base/` with `package.json` (`name: "@dioramap/scene-base"`, `sideEffects: false`, `three: ^0.169.0` — matched to lab-ui's pin). Moved from lab-ui: `viewer/viewer.js` → `scene/create-scene.js` (exports `Viewer` class); `viewer/compass.js` → `scene/compass-gizmo.js` (exports `MarineCompass`). Augmented `create-scene.js` with sky-gradient texture + 3-light rig (HemisphereLight + AmbientLight + DirectionalLight, the last exposed as `this.sun` for future slider rotation) ported from legacy SDK's `scene.js`. Copied legacy `postprocess/edl.js` to `scene/postprocess/edl.js` (exported but not yet wired into the render pipeline — opt-in for future EffectComposer-based consumers). Shipped a minimal `scene/render-context.js` with `BaseRenderContext` (S2 will fill SceneState integration + non-3D surface bridges). Lab-ui's `package.json` adds `"@dioramap/scene-base": "file:../scene-base"`; `app.js` imports `Viewer` and `MarineCompass` from `@dioramap/scene-base`. Lab-ui's `vite.config.js` adds `resolve.dedupe: ['three']` so the production bundle ships one canonical Three.js instance (required for `instanceof` checks across the module boundary). Validation: `npm run build` green (363 modules transformed; bundle 1633 kB / 456 kB gzipped, +0.6 kB vs post-P1 — essentially flat); `npx vite dev` starts in 140 ms; no resolution errors.

**S2 — SceneState surface (sun + visibility tree).** ✅ done.
Shipped `scene/scene-state.js` (`SceneState` + `createSceneState` factory) with both `sun` and `visibility` subsurfaces, and `scene/sun.js` (`SunControl` — SunCalc-based, `setFromSunDirection` + `setFromDate` + `setEnabled`). Visibility tree model: `declare`, `attach`, `tree`, `flatList`, `node`, `setVisible(idOrPath)` (cascades on groups), `hideAll`, `showAll`, `setDefaults`, `clearSubtree` (composer dispose), tri-state `visibleDerived` (`'all' | 'none' | 'some'`). Events on the SceneState `EventTarget`: `sun-changed`, `visibility-registered`, `visibility-changed`, `subtree-changed` (one per cascade with `affectedLeaves`), `subtree-cleared` (with `removedLeafIds`). `BaseRenderContext.render3D` extended to auto-declare layers via SceneState. SunCalc added as `scene-base` dep. Subpath exports added: `@dioramap/scene-base/scene/scene-state`, `/scene/sun`. Validation: 33/33 unit-test assertions green via `npm test` (covering all four mandated scenarios: leaf setVisible, group cascade, tri-state derivation, leaf/subtree removal — plus event-payload shape and sun-direction sanity); lab-ui build green and bundle-size flat at 1633 kB (SceneState is opt-in — actual lab-ui integration lands at L8 / consumers via S4 composers).

**↳ Lab-ui milestone L8** (consumes scene-base) sits here in the dependency graph; tracked in [`PLAN-LAB-UI-PORT.md`](PLAN-LAB-UI-PORT.md). Depends on **P1 + S1 + S2**. Listed in the lab-ui plan because the work happens inside lab-ui; cross-referenced here so it isn't forgotten when S2 unblocks. (Note: P1 is independently a precondition — L8 imports from `@dioramap/protocol` AND `@dioramap/scene-base`.)

**V2 — Viewer-SDK scaffold + protocol wiring + sun-only controls-panel.** ✅ done.
Created `v3/apps/viewer-sdk/` with `package.json` (deps on `@dioramap/protocol`, `@dioramap/scene-base`, `three`), Vite library build (`dist/dioramap-viewer.js`, ES + UMD `DioramapViewer`, three bundled in), standalone dev page on port 5183, frozen `DioramapViewer.init({...})` public surface (PLAN §5.7) — `init`, `setSunTime`, `setSunEnabled`, `dispose` working today; all other methods (`addModel`/visibility setters/etc.) stubbed with warn-once until later milestones. `viewer.js` opens one `WSClient`, mounts `Viewer` + `SceneState` from scene-base, sends `RUN viewer-full` with the §5.4 frozen subscription set on `SESSION_STARTED`, decodes incoming artifacts into a buffer (composer routing is V3 — `ORCHESTRATION = []` for V2). `controls-panel.js` ships sun-only DOM with the legacy 3-slider UX (Month / Day / Time slot 0..143) bound to `sceneState.sun.setFromDate`; `EXPOSED_VISIBILITY_TOGGLES = []` with placeholder "(no layers configured)". Validation: `npm install` clean (13 packages, two devs vulns from upstream Vite chain — no action), `npm run build` green (517 kB / 132 kB gzipped, no warnings), Vite dev server on 5183 resolves all bare specifiers (`@dioramap/protocol`, `@dioramap/scene-base/scene/scene-state`) and serves the dev page. CHANGELOG.md cut at `0.0.1`. Live WS-handshake validation against a running backend deferred to whoever runs the dev server next — code path is straightforward (`WSClient.connect` → `SESSION_STARTED` → `RUN`).

**S3 — Pure utilities.** ✅ done.
Ported `textures/leaf-sprite.js` (procedural canopy mosaic — 128px OffscreenCanvas, deterministic LCG) and `objects/gltf-loader.js` (Three GLTFLoader wrapper returning `{scene, dimensions, bbox}` + recursive `castShadow/receiveShadow`) from the legacy SDK into `v3/apps/scene-base/src/utilities/`. Added subpath exports `@dioramap/scene-base/utilities/leaf-sprite` and `/utilities/gltf-loader` (per §4.1) plus root re-exports. Validation: smoke node import resolves both subpaths and via root index; lab-ui + viewer-sdk builds remain green; scene-base unit tests still 33/33.

**S4 — Shared-scene single-artifact viz modules.** ✅ done.
Shipped five viz modules in `v3/apps/scene-base/src/viz/`, each exporting `meta` (`name`, `requires`, `pipelines`, `default`) + `render(ctx, decoded)` + (where applicable) `beginStream`/`appendChunk`/`finalizeStream`. All call `ctx.render3D([{kind, object: <pre-built THREE.Object3D>, layer: {id, label, defaultVisible}}])` per §4.2 — the composer pre-built-object pattern, not declarative specs. To support that, extended `Viewer.addLayer` (`scene/create-scene.js`) with a top-of-switch branch: when `spec.object` is a Three.js Object3D, scene-add it directly and store in `viewer.layers` so `sceneState.visibility.attach` lookups work uniformly across the SDK ctx and lab-ui ctx. Modules: (a) `parcel-lines.viz.js` — F32 segments → THREE.LineSegments under leaf 'parcels' (yellow, depthWrite:false); (b) `deshadowed-ortho.viz.js` — RGB raster → CanvasTexture → flat plane at z=-0.5 m under 'ground-ortho'; (c) `ortho-surround-jpeg.viz.js` — JPEG → bitmap texture → flat plane at z=-2 m under 'surround-rgb' (`default:false` since S5 terrain.viz supersedes); (d) `elevation-surround.viz.js` — F32 raster → vertex-displaced PlaneGeometry with elevation-coloured vertex colours under 'surround-elev' (`default:false`); (e) `lidar-positions.viz.js` — both one-shot and growable streaming paths (DynamicDrawUsage + 1.6× geometric capacity grow), centroid-derived scene-origin adoption, leaf 'lidar-positions-preview'. Shipped `viz/index.js` with Vite-glob discovery (`import.meta.glob('./*.viz.js', {eager:true})`) exporting `vizCatalog` + `filterByCatalog(availableArtifacts)` for lab-ui's composite-views panel; subpath exports added in `package.json` for both the index and each module. Validation: smoke-imported each module via raw Node (all five expose meta+render+correct requires; lidar-positions has the streaming triplet); scene-base unit tests still 33/33; lab-ui build green (1653 kB, unchanged — vizCatalog not yet wired into app.js, that's L9); viewer-sdk build green (515 kB, unchanged — confirms the §5.8 tree-shaking invariant: SDK doesn't transitively pull in the viz directory). Live lab-ui composite-views integration validates at L9 when app.js consumes `vizCatalog`.

**V3 — SDK first three composer layers + first curated toggles.** ✅ done.
Plan adjustment: V3 was originally listed as `terrain` + `lidar-positions` + `deshadowed-ortho`, but `terrain.viz` is an S5 multi-artifact composer (not yet shipped). V3 instead wires the three single-artifact composers from S4 that exist now: `lidar-positions` (streaming preview), `deshadowed-ortho`, `parcel-lines`. The `terrain` composer slots in at V4 once S5 lands. New module `src/orchestration.js` owns the closed-allow-list dispatch: tracks `_kindByName` from CATALOG_RESPONSE, decodes ARTIFACT bytes via `decodeArtifact(kind, name, bytes)`, gathers `meta.requires` per composer and one-shot-renders when satisfied; routes STREAM_CHUNK events through the streaming triplet (`beginStream` on first chunk, `appendChunk` per chunk, `finalizeStream` on `final=true`) and suppresses the trailing dual-delivery ARTIFACT_HEADER for already-streamed artifacts. `viewer.js` now does the two-step handshake: SESSION_STARTED → CATALOG `viewer-full` → CATALOG_RESPONSE → seed kinds → RUN with the §5.4 frozen subscription set. ORCHESTRATION list explicit-imports the three viz modules from `@dioramap/scene-base/viz/*` per §5.2. Dispose path tears down composer ctxs (`BaseRenderContext.dispose`) which in turn removes their viewer.layers + sceneState.visibility leaves. `EXPOSED_VISIBILITY_TOGGLES` populated with three curated entries — `lidar-positions-preview`, `ground-ortho`, `parcels` — matching the leaf IDs the composers register. Validation: vite build green (526 kB / 135 kB gzipped, +12 kB / +3 kB gzipped vs V2 — three viz modules + orchestration); HMR-served dev page (5183) imports the new modules cleanly via `/@fs/...` resolution; live backend smoke needs running dioramap-processor (composer dispatch, sub-second first chunk, toggle wiring all coded but not yet exercised end-to-end against a real session). `_renderedViaStream`-style suppression keeps ORCHESTRATION sane on cached-re-run cases that resend a full ARTIFACT_HEADER after streaming.

**S5 — Shared-scene multi-artifact + tree-emissive composers.** ✅ done.
Shipped three multi-artifact composite-visualizers in `v3/apps/scene-base/src/viz/` (Z-up native — no axis swap, in contrast to the legacy SDK's Y-up source). (a) `terrain.viz.js` reads `lidar-ground-grid` + `deshadowed-ortho-rgb-rasters` + `canonical-location` + `scene-origin` and renders a vertex-displaced PlaneGeometry textured with the deshadowed RGB. (b) `point-cloud-discs.viz.js` reads `lidar-positions` + `lidar-normals` + `lidar-spacing` + `lidar-classifications` + `canopy-flags` + `deshadowed-ortho-rgb-rasters` + `canonical-location` + `scene-origin`, filters out canopy-flagged points, builds an InstancedMesh of CircleGeometry(1, 6) hex discs scaled by spacing, oriented to per-point normals, with per-vertex ortho sampling + radial alpha fade injected via `onBeforeCompile`; cast/receive shadows with a 25%-scale solid-hex depth material. (c) `canopy.viz.js` reads the same per-point reads + the leaf-sprite utility from S3, builds a 2-instance-per-point InstancedMesh of leaf-textured hex billboards with random-tilt depth, alpha-discard via leaf texture in the fragment shader (NOT exposed to the depth pass — depth uses solid hex). All three add a `meta.supersedes: ['lidar-positions-preview']` field on `discs` so toggling discs ON in either client auto-hides the early streaming preview. Wired into `package.json` exports + Vite-glob index. The toy boundary-3d tree-emissive fixture wasn't shipped — partially exercised by the lab-ui raster ground-plane group (`path: ['Rasters']` + tri-state cascade + filter), full exercise deferred to the boundary-3d v3 port (real consumer).
The polish round (#168) tuned the SDK to production: removed `local terrain` + `ground-ortho` from ORCHESTRATION (discs already paint per-vertex ortho), collapsed `EXPOSED_VISIBILITY_TOGGLES` to just `parcels`, added a `devHelpers` Viewer option (axes/grid only when true — lab-ui passes true, SDK leaves false), 30 fps cap (`maxFps` constructor option), enabled shadow map + configured a 600 m orthographic shadow camera on the directional sun, wired EDL via EffectComposer (32-bit depth texture matching legacy SDK to kill the moire — original 16-bit had quantization artefacts on the regular hex disc grid; fade EDL between 150 m and 300 m view distance so the surround doesn't get noisy halos), enabled subtle fog (near=500, far=1200), and the surround composite was upgraded to combine `elevation-surround-f32` + `ortho-surround-jpeg` into a single textured heightmap (was two separate planes).
The surround mesh's no-data and rendering edge cases all got fixed: BIL data is row-0=north (so vertex y=0 north samples raster row 0 — no flip); created a per-cell validity flag with two filters (sentinel range + median+MAD outlier rejection); REBUILD the index buffer to skip every triangle touching an invalid vertex (clean cutoff vs the prior fragment-discard smear which dived to median z); inner-radius discard for fragments inside the lidar disc footprint so surround doesn't overpaint the discs; CCW triangle winding so `computeVertexNormals` puts the front face up; sized the mesh to ±600 m of canonical center (matches the C step's `radius_m` default — was wrong padding-from-disc-edge logic that crammed 600 m of data into 275 m of mesh).

**↳ Lab-ui milestone L9** (`3DSceneStatePanel`) sits here in the dependency graph; tracked in [`PLAN-LAB-UI-PORT.md`](PLAN-LAB-UI-PORT.md). Depends on L8 + S2; benefits from S5's tree fixture.

**V4 — SDK remaining composers.** ✅ done.
SDK ORCHESTRATION extended with the S5 composites: `preview` (streaming) + `parcels` + `discs` + `canopy` + `surround`. `terrain` and `ground-ortho` intentionally excluded (discs already paint per-vertex ortho, so a separate ground plane would just z-fight + add a debug-feel grey patch underneath). Discs auto-supersedes preview via the new `meta.supersedes` mechanism — both the SDK orchestration and lab-ui's composite-views dispatcher honour it on first successful render. SUBSCRIBE_LIST extended with `lidar-spacing` (the discs need it). `EXPOSED_VISIBILITY_TOGGLES` collapsed to a single curated entry (`parcels` — Cadastre outlines); the rest of the production scene is non-toggleable (canopy/discs/surround are part of the embed, not user-configurable). Validation: SDK at 5183 paints production-quality scene matching the legacy embed visually; reload behaves correctly; no z-fighting; shadows visible on the disc cloud + canopy onto terrain.

Side-fixes shipped in the same arc:
- **C step `fetch-elevation` + `fetch-elevation-surround` v1 → v2**: wrap the raw IGN BIL F32 bytes in the standard 12-byte raster_format header before writing — was emitting bare F32 bytes against a declared `AK_RASTER_F32` kind, so the JS decoder mis-parsed cols/rows as junk integers. Bumped consumer step versions too (`compute-scene-origin` v1→v2, `build-parcel-lines` v2→v3) to skip past the new header.
- **WS server frame-pair race** (`server.c:cb_on_artifact_ready` + `cb_on_stream_chunk`): made (text-header, binary-payload) emission atomic via `session_enqueue_pair` — concurrent on_artifact_ready callbacks from the runner's thread pool were interleaving on the session queue (header_A, header_B, binary_A, binary_B), and the WSClient's pair-by-adjacency strategy mis-paired them. Surfaced as 90008-byte denormal-F64 garbage for `lidar-positions` in lab-ui's 36-artifact load (SDK's 13-artifact load was sparse enough to dodge it).
- **Orphan ctest harnesses**: all three `serve_*.py` test harnesses now use `start_new_session=True` + `os.killpg(SIGTERM, fallback SIGKILL)` so abrupt parent death takes the processor child down with it. Cleaned up three orphans from the host (PIDs 1907 / 54385 / 54796).
- **Lab-ui raster ground planes always-on**: lab-ui's `renderers/builtin/raster.js` was calling `viewer.addLayer` directly — bypassing `sceneState.visibility.attach`, so the AK_RASTER_F32 ground planes were rendered but had no checkbox. Routed through `ctx.render3D` with a stable `layer.id = raster::<artifact>` and `path: ['Rasters']`, so all raster planes now collapse under a single tri-state `Rasters` group node in the gear-popover Layers tree. Updated lab-ui's `RenderContext.render3D` to honour `spec.layer.{id, label, path, defaultVisible}` per the §4.2 contract.
- **Lab-ui visibility tree alignment + colour**: leaf rows now have an empty triangle-width spacer so leaf names align with sibling group names (groups previously appeared indented because the disclosure triangle pushed their checkbox right). Group labels coloured with `--accent` (blue) + bold, distinct from the bright-white leaf labels.

**V5 — SDK sun-direction slider init.** ✅ done (commit `2025…` / shipped in `@dioramap/viewer-sdk@0.1.0`). Subscribed to `sun-direction` (output of `find-best-sun-time`); on artifact arrival the SDK calls `sceneState.sun.setFromDate({...})` which seeds the Month/Day/Time slider at the photo-capture time AND recomputes direction via SunCalc (matches `(sun_dx, sun_dy, sun_dz)` to numerical drift). ~20 LOC tap in `viewer.js`'s ws.artifact handler. Validated: slider initial position moves from the noon default to whatever the C step picked; cached re-runs hit the same path; user can still slide freely.

**V6 — SDK models system.** ✅ done (shipped in `@dioramap/viewer-sdk@0.1.0`). Ported `lidar-3d-environment-viewer/frontend/src/models/` (~660 LOC) to `apps/viewer-sdk/src/models/` and adapted to v3's Z-up frame: `model-manager.js` (GLTFLoader + Map<uuid, ModelInstance> + top-down ground-snap raycaster that excludes models / dev helpers / TransformControls drag-plane / bbox helper), `model-interaction.js` (cyan Box3Helper + TransformControls + CSS2D action-buttons {move / rotate / delete}; per-frame ground-follow on X/Y drags, manual height on pure-Z drag), `ghost-placement.js` (translucent GLB follows cursor; click confirms via addModel; Esc cancels), `index.js` (entry point that wraps manager + interaction + ghost into the SDK's frozen surface). Public methods now wired: `addModel`, `startGhostPlace`, `cancelGhost`, `removeModel`, `getModels`, `getModel`, `selectModel`, `deselectModel`, `getSelectedModel`, `setGizmoMode`, `setModelTransform`. Three sample GLBs copied to `apps/viewer-sdk/public/models/`. Dev page got a small lower-left "Models (V6 dev test)" panel with one button per GLB and a placed-list with click-to-select + delete + live xyz that updates during drag. Y-up→Z-up rotation is `gltfScene.rotation.x = +PI/2` (the `-PI/2` sign flips models upside-down and produces the "model floats high above ground" symptom). The bigger seller-side end-to-end demo with API-token integration is a separate later workstream.

**S6 — Mega-composer + full-scene parity.** ❌ **cancelled** 2026-04-26.
Original idea: ship `full-scene.viz.js` as a single-toggle composite that bundles every SDK composite into one entry, so a lab-ui dev could click one checkbox and see exactly the production scene. Cancelled because L10's composite-views panel already covers the use case at zero extra cost — the SDK composites (`preview` / `parcels` / `discs` / `canopy` / `surround`) all carry `meta.default: true`, so they auto-render on pipeline load. Lab-ui's gear-popover Layers tree → Hide-all + cherry-pick gets the same effect when needed. A keyboard shortcut for "preset: SDK scene" can be added later as ~10 LOC if it ever becomes a hot path; doesn't justify a whole composite.

**V7 — SDK loading bar + progress.** ✅ done (shipped in `@dioramap/viewer-sdk@0.1.0`). Centred dark card with a green progress bar at `apps/viewer-sdk/src/loading-bar.js`, mounted inside the SDK's container. Driven by `RUN_STARTED` (resets, `total = step_plan.length`, shows) / `STEP_COMPLETE` (`completed++`, refresh) / `RUN_COMPLETE` (100 %, fade, hide). Cap at 99 % until run-complete. Label stays "Loading…" — internal pipeline step names aren't surfaced to end users (the SDK is the production embed; lab-ui has its own timeline panel for that).

**V8 — SDK polish + bundle-size check + CHANGELOG v0.1.0 cut.** ✅ done (shipped in `@dioramap/viewer-sdk@0.1.0`). New `npm run check:bundle-size` (combined as `npm run check`) — guards raw / gzip thresholds AND scans the bundle for distinctive labels of composers the SDK is supposed to NOT include (terrain.viz, deshadowed-ortho.viz, ortho-surround-jpeg.viz). Catches accidental imports of `@dioramap/scene-base/viz` (the index) or transitive leaks per §5.8. Current bundle: 699 kB raw / 185 kB gzipped (under 1.1 MB / 220 kB budget). The +180 kB jump from 0.0.3 (519 kB UMD) is the GLTFLoader + TransformControls + CSS2DRenderer pulled in by V6. Open dispose-audit note: scene-base's `Viewer` doesn't yet expose a `dispose()` for its internal rAF — deferred to S7 (scene-base cleanup); the SDK's own resources (rAFs, listeners, WSClient, panels, models, loading-bar, compass) are properly cleaned up in `mountViewer().dispose()`.

**S7 — Scene-base documentation + cleanup.** ✅ done.

- `Viewer.dispose()` shipped at `apps/scene-base/src/scene/create-scene.js`: cancels the internal rAF (via a stored `_rafId` + a `_disposed` guard inside `_frame`), disconnects the resize observer, runs `clearAll()` to remove every layer, disposes the EffectComposer's pass materials + the shared `_composerTarget`, disposes OrbitControls + sky texture, then `renderer.dispose()` + `forceContextLoss()` + canvas removal. Idempotent. The viewer-sdk's `mountViewer().dispose()` was simplified to call `sceneCtx.dispose()` instead of touching the renderer directly. This closes the V8 "Open dispose-audit notes" entry.
- READMEs added at four levels: package root `apps/scene-base/README.md` (canonical refs + tree-shaking invariant + public surface map), per-directory at `src/scene/`, `src/viz/`, `src/utilities/`. Each is a thin pointer that defers to `v3/docs/VISUALIZERS.md` (the umbrella vocabulary doc) for contracts and only documents what is unique to the directory (file table + SDK/lab-ui split for `viz/`). Renderer-authoring guidance lives in `viz/README.md`'s "Adding a new composite" section + the `Module shape` block; SceneState-binding lives in `scene/README.md`'s `Lifecycle expectations` section. No separate guide files — keeping the doc surface tight per L11's policy of links-not-restates.
- `apps/scene-base/CHANGELOG.md` cut at 0.1.0 (the package was already at version 0.1.0 in package.json from earlier work — S7 just ratifies it with the first formal CHANGELOG entry). Pre-0.1.0 changes are catalogued in `apps/viewer-sdk/CHANGELOG.md` (S1–V8) + `PLAN-LAB-UI-PORT.md` (L1–L11).
- "Remove the moved files from lab-ui's source" — already clean. Sweep of `apps/lab-ui/src/` for `*.viz.js` or `scene/*` paths returned nothing; the lab-ui import paths point at `@dioramap/scene-base/...` everywhere.

Validation: scene-base tests 33/33, viewer-sdk `npm run check` green (700 kB raw / 185 kB gzip — bundle-size + composer-isolation invariants both pass), lab-ui build green.

**V9 — Production cutover.** ❌ pending.
Swap production embed URL to v3 bundle. Decommission `lidar-3d-environment-viewer/backend/`. Bundle hosting + auth + seller-demo end-to-end (A15 + A16) cleared the prerequisites; the actual cutover (DNS + decommission) hasn't been performed.

## 8. Open issues

1. **`ceiling-flags` — RESOLVED 2026-04-25: dropped permanently.** In bench-v2's `geometry/ceiling-flags.js`, a "ceiling point" is a LiDAR point at the top of a solid surface (roof ridge, upper canopy layer). The legacy SDK's `column-renderer.js` uses these flags to drop a vertical hexagonal "skirt" from each ceiling point down to the tile's minimum altitude — a cosmetic fill that hides the hollow underside of overhangs. **Decision**: dropped from the v3 SDK entirely. The column-skirts renderer (`point-cloud/column-renderer.js` in legacy) does not get ported. No `detect-ceilings` step is planned. If a future use case demands it, the renderer can be added back as a step-collocated viz with its own detection step.

2. **`lidar-spacing` — RESOLVED 2026-04-23: option (a) shipped.** The legacy disc renderer scales each per-point hexagon by the point's **local spacing** — distance to its k-th nearest neighbour. Implemented as a second F32 per-point write on `compute-normals` (`v3/steps/normals/compute-normals.c`): spacing[i] = sqrt(neighbors[k/2].dist_sq), matching the legacy SDK's `Math.floor(k/2)` median k-NN distance definition. `compute-normals.version` bumped 1 → 2 (writes-set change → identity-hash invalidation). `n_writes = 2`; both writes are `STEP_VIS_PUBLIC` so SDK and lab-ui composers can subscribe. Edge case: when N < min_neighbors the early-exit path defaults spacing to 1.0 m. Tests added: `test_compute_spacing_uniform_grid`, `test_compute_spacing_dense_vs_sparse`, `test_compute_spacing_few_points_fallback` (all green; 3234/3234 checks total). The S5 disc composer now has the artifact it needs.

3. **Three.js version lock.** Scene-base pins `^0.169.0` (lab-ui's pin). Both consumers follow. If a legacy renderer's shader doesn't compile, fix in scene-base, not in viewer-sdk.

4. **`RenderContext` and SceneState API churn.** §4.2/4.3 were informal until S5 exercised composite-visualizers under real load (multi-artifact discs/canopy/surround). The contract held — only a `meta.supersedes` field added during V4 polish. Considered closed.

5. **Sun-direction artifact accuracy.** Unseeded `find-best-sun-time` scores 0.41 on Notre-Dame. Measure on more real mounts during V5; if accuracy holds, the legacy photo-meta concept stays dead.

6. **SDK's curated toggle set.** `EXPOSED_VISIBILITY_TOGGLES` is a product decision that will evolve. Per-release CHANGELOG entries required when it changes. Not fixed by this plan — established case-by-case during V3 + V4.

## 9. Risks

- **Cold-mount latency.** `viewer-full` cold run is ~180 s on Notre-Dame (IGN COPC fetch, serial). STREAM_CHUNK delivers first point-cloud chunk ~1 s after `laz-extract-disc` starts. Server-side caching makes repeat mounts sub-second.

- **Scene-base S1 regression in lab-ui.** Moves `viewer/` and `compass.js` out of lab-ui. Mitigation: full lab-ui ctest + smoke before S1 lands.

- **Tree-API validation under real load.** The v2 boundary-3d step produces 147 leaves. The SceneState tree API must not degrade on a 100+ leaf tree with tri-state master checkboxes. Prototype the toy fixture at S5 specifically to stress this.

- **Legacy `frontend/` fork drift.** The legacy tree may still see edits during the port. Fork at a fixed revision at V2; cherry-pick fixes forward by hand.

- **`models/` integration friction.** The placement system needs scene hooks that scene-base may have refactored. Expect small adjustments at V6.

- **Three.js version drift.** Prototype the hardest renderer (`point-cloud-discs.viz.js`) first at S5 to catch shader compat surprises.

- **Scene-base bundle leakage into the SDK.** As scene-base grows with inspector viz + new composers, tree-shaking must keep the SDK bundle minimal. Two failure modes (§5.8): (a) scene-base's `package.json` missing `"sideEffects": false` → Rollup keeps everything reached; (b) SDK accidentally imports `viz/index.js` (the lab-ui-only Vite glob entry) → drags the entire viz directory in. Mitigation: CI bundle-size check at V8.

## 10. Open questions — all resolved

| # | Question | Resolution |
|---|---|---|
| Q1 | Seed `find-best-sun-time` with photo capture metadata? | **No.** Brute-force; 0.41 score is excellent. Photo-meta concept dropped. |
| Q2 | Sun slider — server re-RUN or client-side? | **Client-side only.** SunCalc → DirectionalLight via `sceneState.sun`. `sun-direction` artifact initialises slider position. |
| Q3 | Avoid copy/paste divergence with lab-ui? | **`@dioramap/protocol` + `@dioramap/scene-base`** sibling packages, `file:` deps. Same pattern as `@dioramap/location-picker`. |
| Q4 | Which pipeline? | **Hardcoded `viewer-full`.** No variant. |
| Q5 | How does lab-ui share rendering with the SDK? | **Both consume `@dioramap/scene-base`.** SDK orchestrates from a closed allow-list (§5.2); lab-ui discovers via Vite glob (§4.4). |
| Q6 | Does the SDK expose visibility toggles? | **Yes — a curated allow-list.** `EXPOSED_VISIBILITY_TOGGLES` in §5.5. Not every declared layer; not zero. Product-decided per release. |
| Q7 | Does a new viz in `scene-base/viz/` propagate to the SDK automatically? | **No.** Closed allow-list. Requires SDK code change + CHANGELOG + new release. Lab-ui picks it up via Vite glob automatically. |
| Q8 | How do lab-ui's existing `OverlayPanel` and the new SceneState coexist? | **Orthogonal concerns.** `OverlayPanel` is inspector-scoped (artifact navigation, any surface). `SceneState` is 3D-scene-scoped (sun + visibility tree, 3D only). They intersect only at `ctx.render3D`, whose dual effect is an implementation detail of the ctx. `OverlayPanel` stays unchanged. |
| Q9 | How do tree-emissive composers (boundary-3d shape) fit? | **One composer → one `OverlayPanel` row, N `sceneState.visibility` leaves.** Composer calls `ctx.render3D` with a `layer.path` to place each leaf in the tree. Lab-ui's `3DSceneStatePanel` renders the tree generically (ported from v2's UX) with filter + tri-state + deselect-all. |

## 11. What this plan does NOT commit to

- Owning any rendering code in viewer-sdk. **All renderers live in `@dioramap/scene-base`.**
- Owning any WS protocol code. **All WS plumbing lives in `@dioramap/protocol`.**
- Owning any scene-state logic. **Sun math, SunCalc, light transforms, visibility tree, cascade + tri-state logic all live in `@dioramap/scene-base/scene/scene-state.js`.** Each consumer ships its own DOM bound to that surface.
- Moving, refactoring, or renaming `apps/lab-ui/src/artifact-inspector/overlay-panel.js`. It stays as-is — it is orthogonal to SceneState and solves a different problem (artifact inspector navigation, any-surface).
- SceneState owning artifact navigation, inspector rows, SVG/Chart/JSON/HTML rendering, or any surface besides the 3D scene.
- Exposing the full visibility tree in the SDK — only the curated `EXPOSED_VISIBILITY_TOGGLES` allow-list is user-facing.
- A discovery mechanism for visualizers in the SDK — the SDK imports explicitly.
- New scene features, camera modes, VR, mobile optimizations.
- Public API redesign — `DioramapViewer.init(...)` signature is frozen.
- Porting `lidar-3d-environment-viewer/backend/`, `src/`, `location-picker/`, `seller-demo/`.
- A framework migration.
- Any debug, dev, or inspection surface in the SDK.
- A pipeline variant smaller than `viewer-full`.
- Photo-meta seeding for `find-best-sun-time`.
- Replacing lab-ui's existing per-artifact built-ins (`renderers/builtin/*`) — those stay as fallbacks.
- Reactivity / dynamic re-render on artifact updates within the same run.
- Server-side viz or SceneState registry.
- Per-module renderer versioning (composers are versioned by the package's semver).
