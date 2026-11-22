# Visualizers

*The single source of truth for "the JS module that turns artifact bytes into something the user sees" — what we call them, where they live, how each client loads them, and where their outputs render in the UI.*

This doc replaces the scattered nomenclature ("renderer", "viz module",
"composer", "scene composer", "builtin", "primitive renderer", "generic
renderer", "shared scene composer") that grew across UNIFIED-ARCHITECTURE,
PLAN-LAB-UI-PORT, and PLAN-VIEWER-SDK-PORT. Other docs should refer here
rather than re-defining concepts locally.

---

## 1. Vocabulary

A **visualizer** is a JavaScript module that consumes one or more decoded
artifacts and emits user-visible output through a fixed surface API
(`ctx.render3D`, `ctx.renderSVG`, `ctx.renderChart`, `ctx.renderJSON`,
`ctx.renderHTML`). Visualizer source files end in `.viz.js` and live in
one of three locations:

| Subtype | On-disk location | Loaded by | Audience |
|---|---|---|---|
| **builtin-visualizer** | `apps/lab-ui/src/renderers/builtin/<kind>.js` | lab-ui — static import, kind-dispatched | lab-ui only |
| **step-visualizer** | `v3/steps/<step>/viz/<artifact>.viz.js` | lab-ui — dynamic-import on artifact arrival | lab-ui only |
| **composite-visualizer** | `apps/scene-base/src/viz/<name>.viz.js` | lab-ui via Vite glob (`vizCatalog`); SDK via explicit closed allow-list | lab-ui **and** SDK |

These three subtypes share the same module contract (§3) and surface API
(§4). They differ only in **discovery / load mechanism**, **lifecycle**
(when they fire — §5), and **which UI surfaces** their outputs reach
(§6, §7).

The umbrella term is **visualizer**. Subtype names are
`builtin-visualizer`, `step-visualizer`, `composite-visualizer`. Avoid
"renderer" (collides with `THREE.WebGLRenderer`), "composer" alone (a
composite-visualizer that reads multiple artifacts is still a
composite-visualizer — "composer" is just an English word for what one
does, not a category).

---

## 2. Why three subtypes?

The split is driven by **who imports the module** and **what guarantees
each client needs**.

- The **SDK** is a versioned production artifact. Adding a visualizer to
  it must be an explicit code change with a CHANGELOG entry. Hence the
  closed allow-list: composite-visualizers in `scene-base/viz/` are the
  only modules the SDK ever imports.
- **Lab-ui** is a developer cockpit. It needs every visualizer
  available at all times: every kind-dispatched fallback, every
  step-collocated custom view, and every shared composite. Hence two
  additional load mechanisms layered on top.
- **Step-visualizers** are ergonomically owned by one step. Putting the
  module next to the C source keeps version drift low and the
  ownership obvious. They never need to ship in the SDK because they
  are inspector views, not scene content.

The rule is therefore:

> **If the SDK imports it → composite-visualizer (in `scene-base/viz/`).
> Otherwise → step-visualizer (in `steps/<step>/viz/`) or builtin-visualizer
> (in `lab-ui/renderers/builtin/`) depending on whether it customises one
> specific artifact or covers a whole artifact kind.**

---

## 3. Module contract (identical across all three subtypes)

```js
// the .viz.js shape — same for builtin / step / composite
export const meta = {
  name: 'human label',           // shown in panels (composite-visualizer only — others derive label from artifact name)
  requires: ['artifact-a', ...],  // composite-visualizer only — list of artifact names; lab-ui filters by this
  pipelines: '*' | ['p1', 'p2'],  // composite-visualizer only — narrow which pipelines see this entry
  default: true | false,          // composite-visualizer only — pre-check the toggle in lab-ui's composite-views panel
};

// One-shot render. `decoded` shape:
//   - builtin / step:    decoded = the single artifact's decoded payload
//   - composite:         decoded = { [artifactName]: payload, ... }
export function render(ctx, decoded) { ... }

// Optional progressive-streaming triplet. Used by composite-visualizers
// over streaming artifacts (e.g. lidar-positions). Builtins + step-visualizers
// can implement them too if they handle a streaming artifact.
export function beginStream(ctx, info) { ... }
export function appendChunk(ctx, decoded, info) { ... }
export function finalizeStream(ctx, info) { ... }

// Optional cleanup hook (or use ctx.onCleanup(fn) inside render).
export function dispose(ctx) { ... }
```

Builtins and step-visualizers don't need a `meta` block — their
discovery is by-artifact-kind or by-artifact-name respectively, and
they always run when their artifact arrives. The fields above only
apply to composite-visualizers, which need to declare what they read
because lab-ui has no other way to know.

---

## 4. Output surfaces (`ctx.render*`)

A visualizer never directly touches the DOM, the Three.js scene, or
`sceneState`. It calls one or more of these methods on the
`RenderContext` it receives:

| Method | What it does |
|---|---|
| `ctx.render3D(specs)` | Adds Three.js objects to the scene **and** declares a leaf in `sceneState.visibility`. Each spec carries `{kind, object, layer:{id,label,path?,defaultVisible?}}`. The optional `path:[…]` puts the leaf inside an interior group node — that's how a single visualizer call can populate a multi-level tree (a **tree-emissive** call, sized for boundary-3d's 147-leaf shape). |
| `ctx.renderSVG({name, gElement})` | Appends one SVG block to the artifact inspector entry. Repeatable. |
| `ctx.renderChart(builderFn)` | Appends a Chart.js block to the artifact inspector entry. The builder gets a `<div>` to populate. |
| `ctx.renderJSON(value)` | Appends a pretty-printed JSON block. |
| `ctx.renderHTML(elementOrString)` | Appends free-form HTML. |
| `ctx.onCleanup(fn)` | Registers a teardown callback (called on visualizer dispose / pipeline re-run). |

**3D output is one surface among five.** A visualizer can emit any
combination — a step-visualizer that renders a histogram + a JSON
summary + zero 3D layers is perfectly valid. The artifact inspector
displays the non-3D surfaces; the 3D viewer renders the 3D surface;
they're independent stacks.

In the SDK, **only `render3D` runs**. The other four are no-ops because
the SDK has no inspector. A composite-visualizer that contributes both
3D and a JSON block will paint the 3D and silently drop the JSON when
imported by the SDK. This keeps visualizer code identical across hosts.

---

## 5. Lifecycle — when does a visualizer fire?

| Subtype | Trigger | Notes |
|---|---|---|
| **builtin** | Automatically when its kind-dispatched artifact arrives | One per artifact-kind. Falls through to a `null` placeholder if the kind is unknown. |
| **step** | Automatically when its named artifact arrives — overrides the builtin for that artifact | Resolution order: step-visualizer (if a `.viz.js` exists at `v3/steps/<producer>/viz/<artifact>.viz.js`) → fall back to the builtin for the artifact's kind. |
| **composite** | In **lab-ui**: only when its toggle is on in the composite-views panel. `meta.default: true` entries are toggled on at session start. In the **SDK**: always on, for every entry in the closed `ORCHESTRATION` allow-list. | A composite-visualizer with all `meta.requires` already in the buffer fires immediately when toggled on. Streaming composites use the triplet (begin/append/finalize) so the first chunk paints in <1 s. |

Streaming composites have one extra rule: when the trailing
`ARTIFACT_HEADER` arrives after a `final=true` chunk (dual delivery
per the WS protocol), it's suppressed — the visualizer already
consumed everything via chunks.

---

## 6. UI surfaces — lab-ui

Lab-ui has four UI surfaces that visualizer outputs reach:

```
┌──────────────────────────────────────────┐  ┌─ right column ─────────────┐
│                                           │  │  composite-views panel     │
│        3D viewer pane                     │  │  (L10 — planned)           │
│                                           │  │  ☑ Point cloud (preview)   │
│  • render3D output from any visualizer    │  │  ☑ Ground (deshadowed)     │  ← composite-visualizer toggles
│    paints into the Three.js scene here    │  │  ☐ Surround backdrop       │
│                                           │  │                            │
│  Gear ⚙ → 3D scene panel                  │  │  artifact inspector        │
│  • sun controls                           │  │  ▸ lidar-positions    3D   │
│  • visibility tree                        │  │  ▸ deshadowed-ortho   3D2D │  ← inspector entries from
│    (full sceneState.visibility,           │  │  ▸ canopy-flags       JSON │     builtin- and step-visualizer
│     collapsible group rows,               │  │  ▸ boundary-3d        3D[…]│     output via render{SVG,Chart,
│     tri-state checkboxes,                 │  │                            │     JSON,HTML} (and 3D badges
│     filter, deselect-all)                 │  │                            │     when render3D also fired)
└──────────────────────────────────────────┘  └────────────────────────────┘
```

### 6.1 3D viewer pane — for `ctx.render3D` output

Every visualizer subtype that calls `render3D` paints into the same
Three.js scene. There's no per-subtype gating: a builtin-visualizer's
points, a step-visualizer's mesh, and a composite-visualizer's textured
plane all coexist.

### 6.2 3D scene panel (gear popover) — for the visibility tree

`sceneState.visibility` is a hierarchical tree of visibility leaves.
Every `render3D` call (regardless of subtype) declares one leaf at
`{path, id}`. The 3D scene panel renders the **full** tree:

- Collapsible interior group rows (`{path: ['Roof Graph', 'Plane 4']}`
  builds the chain).
- Tri-state checkboxes on group nodes (`'all' | 'none' | 'some'`).
- Free-text filter (substring on the full path + label).
- Show-all / hide-all / reset-defaults actions.

This is the **layer tree control surface**. It's host-agnostic — any
visualizer that emits a 3D layer participates automatically; no
per-visualizer UI code.

(In the SDK the same `sceneState.visibility` exists but only a
**curated subset** of leaf IDs — `EXPOSED_VISIBILITY_TOGGLES` — surfaces
as user toggles. See §7.)

### 6.3 Artifact inspector — for non-3D surfaces

One row per loaded artifact. The row shows surface badges (`3D`, `SVG`,
`JSON`, `HTML`, `Chart`) that reflect which `ctx.render*` calls were
made for this artifact. Clicking a row scrolls to its render blocks.

For each artifact, lab-ui resolves a single visualizer module:

1. If `v3/steps/<producer>/viz/<artifact>.viz.js` exists → **step-visualizer**.
2. Else → **builtin-visualizer** keyed on the artifact's kind.

Either fires on artifact arrival. Output is the per-artifact inspector
row plus (optionally) 3D scene contributions and visibility-tree leaves.

A **composite-visualizer** can also write to this stack — when it has
`renderSVG`/`renderJSON`/etc. calls in its render body. The block lands
under a synthetic inspector row keyed by the visualizer's `meta.name`
(not by an artifact name).

### 6.4 Composite-views panel — for composite-visualizer toggles

A list-of-toggles panel listing every composite-visualizer whose
`meta.requires` are all satisfied by the current pipeline's catalog.
Pre-checked entries: those with `meta.default: true`. User toggling on
→ subscribes to the listed artifacts (if not already), instantiates a
RenderContext, and dispatches when ready. User toggling off →
ctx.dispose() removes the visualizer's contributions from the 3D scene
and the visibility tree.

This is the **only** UI surface where composite-visualizers appear as
controllable units. Their layer-tree leaves still surface in the 3D
scene panel like any other visualizer's leaves; their per-artifact
contributions still surface in the artifact inspector. The
composite-views panel is the *enable/disable* switch, not the output
target.

---

## 7. UI surfaces — SDK

The SDK has only the 3D viewer + one DOM panel.

| Surface | Renders |
|---|---|
| 3D viewer pane | `ctx.render3D` output of every composite-visualizer in the closed `ORCHESTRATION` allow-list. No builtin / step visualizers. |
| **SDK controls panel** (top-right corner of the embed) | Sun slider (Month / Day / Time) bound to `sceneState.sun`. Curated visibility toggles, one per entry in `EXPOSED_VISIBILITY_TOGGLES` — a hand-picked subset of leaf IDs the visualizers register. Adding/removing entries here is a documented per-release product decision (CHANGELOG bump). |

The full visibility tree is NOT exposed in the SDK. Toggles outside
`EXPOSED_VISIBILITY_TOGGLES` follow each leaf's `defaultVisible` and
stay as declared.

There is no artifact inspector, no composite-views panel. Non-`render3D`
surface methods on the RenderContext are no-ops (§4).

---

## 8. Summary table — at a glance

| | builtin-visualizer | step-visualizer | composite-visualizer |
|---|---|---|---|
| **On disk** | `apps/lab-ui/src/renderers/builtin/` | `v3/steps/<step>/viz/` | `apps/scene-base/src/viz/` |
| **Discovery in lab-ui** | static import, kind-dispatched | dynamic-import by artifact name | Vite glob → `vizCatalog`, filtered by `meta.requires` |
| **Discovery in SDK** | (never) | (never) | explicit closed allow-list |
| **Reads** | one artifact (kind-typed) | one artifact (named) | 1..N artifacts (named via `meta.requires`) |
| **Triggered by** | artifact arrival, automatic | artifact arrival, automatic; overrides builtin for that artifact | toggle in lab-ui's composite-views panel; always-on in SDK |
| **Output surfaces** | any of the 5 (`render3D` + 4 inspector surfaces) | any of the 5 | any of the 5 (only `render3D` is non-no-op in SDK) |
| **Lab-ui — paint into 3D viewer** | via `render3D` | via `render3D` | via `render3D` |
| **Lab-ui — visibility-tree leaves** | yes | yes | yes |
| **Lab-ui — appear in composite-views panel** | no | no | yes |
| **Lab-ui — appear in artifact inspector** | yes (per artifact) | yes (per artifact, replacing the builtin) | yes (under a synthetic row keyed by `meta.name`) |
| **SDK — runs at all** | no | no | yes (allow-list only) |
| **`meta` block required** | no | no | yes (`name`, `requires`, `pipelines`, `default`) |
| **Streaming triplet supported** | yes | yes | yes |

---

## 9. Concrete examples

### builtin-visualizer
- `apps/lab-ui/src/renderers/builtin/point-column.js` — silent
  JSON-stats-only fallback for `AK_POINT_COLUMN` artifacts that don't
  ship a step-visualizer (3D rendering and histograms are step-visualizer
  territory now — see below).
- `raster.js`, `image-jpeg.js`, `metadata-json.js`, `line-segments.js`
  — one module per kind.

### step-visualizer
- `v3/steps/copc/viz/lidar-classifications.viz.js`,
  `v3/steps/copc/viz/lidar-intensities.viz.js`,
  `v3/steps/copc/viz/lidar-num-returns.viz.js`,
  `v3/steps/normals/viz/lidar-spacing.viz.js` — Chart.js histograms +
  JSON stats for scalar point columns.
- `v3/steps/normals/viz/lidar-normals.viz.js` — 3D points coloured by
  `|nx|,|ny|,|nz|`, sourced from sibling `lidar-positions` via
  `ctx.getSiblingDecoded`. Hides the sibling's default cloud via
  `ctx.removeSibling3DLayer` so we don't paint two overlapping clouds.
- `v3/steps/copc/viz/lidar-positions.viz.js` — JSON stats + a
  streaming-progress overlay line. Deliberately emits **no** 3D layer:
  the `lidar-positions` 3D cloud is the composite-visualizer's job
  (see below). Avoiding the duplicate is the canonical example of how
  step-visualizer and composite-visualizer divide work for an artifact
  that has both: composite paints the production scene, step-viz adds
  inspector-only metadata/charts.
- `v3/steps/boundary-3d/viz/boundary-3d.viz.js` (planned) — will render
  the `boundary-3d` artifact as 147 visibility-tree leaves (4-level
  hierarchy: category / subcategory / group / leaf) plus a JSON
  inspector summary. No composite counterpart, so this one will paint
  3D directly.
- `v3/steps/buildings/viz/per-building-records.viz.js` — renders every
  cadastre-derived building footprint as a closed line ring + the
  inlier point cluster, color-coded per building (golden-angle hue
  spread). Reads `lidar-positions` as a sibling to dereference the
  per-record point indices. First example of the
  PLAN-THIRD-PARTY-RESEARCH track.

> **Lab-ui restart required when adding a new step-visualizer.**
> The renderers/loader.js Vite glob uses `import.meta.glob({ eager: true })`,
> which is evaluated once at dev-server startup. New `.viz.js` files
> appear in the loader registry only after a lab-ui restart — Vite's
> HMR does not re-evaluate eager globs. Workflow:
> `kill $(pgrep -f 'vite.*5181'); cd v3/apps/lab-ui; nohup npm run dev &`

### composite-visualizer
- `apps/scene-base/src/viz/lidar-positions.viz.js` — single-artifact,
  streaming, paints a 3D points layer.
- `apps/scene-base/src/viz/parcel-lines.viz.js` — single-artifact,
  paints THREE.LineSegments.
- `apps/scene-base/src/viz/terrain.viz.js` — multi-artifact:
  `lidar-ground-grid` + `deshadowed-ortho-rgb-rasters` +
  `canonical-location` + `scene-origin` → textured terrain mesh.
- `apps/scene-base/src/viz/point-cloud-discs.viz.js`,
  `apps/scene-base/src/viz/canopy.viz.js`,
  `apps/scene-base/src/viz/elevation-surround.viz.js` — the rest of
  the multi-artifact set shipped at S5.

---

## 9.1 Helpers (`apps/lab-ui/src/renderers/_helpers/`)

When several step-visualizers share the same rendering recipe, factor
the recipe into a helper and have each `.viz.js` be a thin (~10 LOC)
config wrapper. Helpers live alongside but **outside** the loader's
Vite glob (`steps/*/viz/*.viz.js`) so they're not mistaken for
visualizers themselves.

Two helpers in the registry today:

- **`scalar-gradient-cloud.js`** — F32 per-point scalar → 3D point
  cloud coloured by an HSL gradient (blue → red, matches bench-v2's
  `colorMode: 'gradient'`) + Chart.js histogram with hue-matched bars
  + min/mid/max legend + JSON stats. Handles both `AK_POINT_COLUMN`
  (flat F32) and `AK_ARRAY_INLINE` (per-building F32 records, with
  global indices coming from sibling `per-building-records`).
  Knobs: `valueRange`, `histogramScale`, `histogramBins`,
  `layerPath`, `layerLabel`, `layerId`, `pointSize`.

- **`per-point-rgb-cloud.js`** — Per-point RGB → 3D point cloud.
  Three input types via `inputType`:
  - `'uint8'`        — bytes ∈ [0,255] (e.g. v2's `eigen-ternary` and
                       any other pre-encoded RGB).
  - `'normalized'`   — F32 ∈ [0,1] used directly as colour.
  - `'abs-vec3-f32'` — F32 vector; colour = clamp(|x|, |y|, |z|, 0..1)
                       (the standard normal-map convention; used by
                       `lidar-normals` and `tile-normals`).

Per-building parsing is shared in
`_helpers/per-building-records.js` — `parseBuildingHeader(u8)`
returns `{schema, n_verts, n_pts, globalIndices}` for the same
schema that every step writing `per-building-records` follows.

Visibility-tree grouping is by convention: all 13
`v7-tile-signals` outputs use `path: ['Tile Signals']` so they
appear under one collapsible group in the 3D scene panel. The
group label lives in `v3/steps/v7/viz/_layer-path.js` (single
source of truth for renames).

Concrete usage — every v7-tile-signals viz is the same shape:

```js
// v3/steps/v7/viz/planarity.viz.js
import { renderScalarGradientCloud } from
  '../../../apps/lab-ui/src/renderers/_helpers/scalar-gradient-cloud.js';
import { TILE_SIGNALS_PATH } from './_layer-path.js';
export function render(ctx, data) {
  renderScalarGradientCloud(ctx, data, {
    layerPath: TILE_SIGNALS_PATH,
    layerLabel: 'planarity',
    layerId: 'tile-signals/planarity',
    valueRange: [0, 1],
  });
}
```

---

## 10. Cross-references (after this doc lands)

- UNIFIED-ARCHITECTURE.md §11 — sketch only, link to §1–§3 here.
- PLAN-LAB-UI-PORT.md §7.6 — replace the inline definition with a
  pointer here. L10 (composite-views panel) and L11 (step-visualizer
  dynamic-import) both reference §6.4 / §5 here for behaviour.
- PLAN-VIEWER-SDK-PORT.md §4.7 — keep the "where does it live" rule,
  drop the duplicated lifecycle/output prose; link here.
- New module README.md files in each of the three locations should
  open with a one-line link back to this doc.
