# Lab UI Port Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md) §6, §7.2, §10.4, §11 (`v3/apps/lab-ui/`) and to [PLAN-VIEWER-PORT.md](PLAN-VIEWER-PORT.md). Scope: port the `lidar-algos-bench-v2/ui/` lab UI — the algorithm development surface — to a new ESM frontend that speaks the v3 WebSocket protocol (§8.1) directly. Algorithm steps themselves are covered by PLAN-VIEWER-PORT.md; this doc is only about the browser app.*

---

## 1. Why this doc exists

The lab UI is the development cockpit for every LiDAR algorithm we ship. It picks a pipeline, picks a test case, runs it, and lays out each step's artifacts over a DAG view so the author can inspect them. The v3 runtime shipped A7 (WS + sessions); v3 can already run pipelines and stream artifacts. The next question is what frontend consumes those artifacts.

This plan answers:

1. **What features of the current lab UI survive the port** — and which get dropped.
2. **How artifacts render** — a per-artifact renderer-module architecture replaces the monolithic kind dispatch that made new inline artifacts expensive to wire up.
3. **What §8.1 messages the UI needs** — so gaps in the protocol become visible before coding.
4. **What module boundaries make this maintainable** — so a new `.viz.js` drops in without editing a monolith.

Out of scope: the viewer SDK (different audience, different surface), the bench CLI / benchmark runners (`lib/benchmark*.js`), and the bench SQLite database. Metric computation is algorithm-layer; in v3 every score is an artifact emitted by a scoring step and rendered by its own `.viz.js` module.

## 2. What the lab UI is (one paragraph)

Served by a Fastify process at `localhost:4200` (`ui/server.js`), the UI is a single-page vanilla-JS + Three.js app. Left column: pipeline selector + filterable test-case list. Center: a 3D point-cloud viewer with plane meshes, edges, ground-projected image overlays, axis/compass gizmo, perspective/ortho toggle, and a Blender-style node-editor view of the artifact DAG. Clicking an artifact in the DAG shows its payload — point cloud overlays into the 3D scene, or a panel overlay for 2D / chart / JSON / HTML payloads. Right column: step timeline and a collapsible log viewer. Status bar: time, cache stats, map picker. The map picker opens an IGN ortho basemap for live-fetching a real building by clicking a location.

## 3. External inputs

| Source | Protocol | Used for | Notes |
|---|---|---|---|
| Bench backend `/api/pipelines`, `/api/test-cases`, `/api/steps` | HTTP GET JSON | Populate pipeline list, test-case list, per-step params/docs | `ui/server.js:56-206` |
| Bench backend `/api/preview/:tc` | HTTP GET | Load point cloud + footprint before running a pipeline | `ui/server.js:231-300` |
| Bench backend `/api/run`, `/api/run-live`, `/api/artifact` | HTTP POST JSON | Execute a pipeline on a cached TC or live IGN building; fetch one artifact's rendered payload | `ui/server.js:340-982` |
| IGN ortho WMTS, `api-adresse.data.gouv.fr` | HTTPS GET | Basemap + geocoding inside the map picker (today's `map-picker.js`) | `map-picker.js:21-82` |

In v3, everything in the first three rows collapses into a single WebSocket to `dioramap-processor` (§8.1). The lab UI does not ship its own map picker in v3 — it embeds the viewer SDK's map picker component (see §4.6).

## 4. Feature inventory — functional

Each row is a capability the UI exposes today. Entry-point citations are to the legacy tree, not to v3. Features marked **dropped** do not port.

### 4.1 Pipeline running

| Feature | Entry point | Ports? |
|---|---|---|
| Select pipeline from a filterable list | `app.js:434-489` | Ports as a flat list with a filter input — no DAG picker. |
| Select pipeline from URL param | `app.js:461-473` | Ports. |
| Select test case | `app.js:713-814` | Ports as a filterable list. Category/sibling tree logic is kept; synthetic-case special rules drop. |
| Run pipeline on TC | `app.js:956-1005` | Ports. Becomes a RUN + subscriptions over WS. |
| Run pipeline on live IGN building | `app.js:6711-6764` | Ports via map-picker (§4.6). |
| No-cache toggle | `index.html:48`, `app.js:974` | Ports; becomes a RUN flag. |
| Cmd/Ctrl+Enter to run | `app.js:6652-6656` | Ports. |

### 4.2 Test-case management

| Feature | Entry point | Ports? |
|---|---|---|
| Preview point cloud before run | `app.js:833-853` | Ports. |
| Toggle GT validation flag | `app.js:855-919` | **Dropped.** Synthetic test cases do not port, so no GT validation surface is needed. |
| Save live building as new TC | `app.js:6771-6810` | Ports, gated on §11.Q2 (fixture catalog). |
| Save boundary test case | `ui/server.js:1043-1068` | **Dropped.** The dedicated boundary benchmark is abandoned. |

### 4.3 Artifact inspection

The old UI dispatches artifact rendering from a monolith: `applyArtifactToViewer()` + a growing chain of name-based `if`s spread across `app.js` and `artifact-graph-view.js`. Every new 2D artifact required edits in two places — confirmed pain (memory: `feedback_inline_2d_artifact_wiring.md`).

The v3 model inverts this. **Each artifact type's output is produced by a visualizer.** When an artifact arrives, the lab UI dispatches to its visualizer (per the resolution order in [VISUALIZERS.md §5](../VISUALIZERS.md): step-visualizer if present → builtin-visualizer keyed on artifact kind). Composite-visualizers in `@dioramap/scene-base/viz/` reach lab-ui via the composite-views panel (L10). The visualizer translates the raw artifact (SoA, raster, JSON, whatever kind the pipeline emits over WS) into output via a fixed `RenderContext` API. It never touches Three.js, the SVG viewport, or the panel DOM directly.

**Discovery.** All three visualizer subtypes (builtin / step / composite), their on-disk locations, load mechanism, and lifecycle are defined in [VISUALIZERS.md](../VISUALIZERS.md). Most artifacts ship no custom module at all — the kind-dispatched builtin-visualizer covers the common case (points / rasters / JPEGs / arrays / opaque JSON).

**Five output surfaces** (`ctx.render3D` / `renderSVG` / `renderChart` / `renderJSON` / `renderHTML`) — defined in [VISUALIZERS.md §4](../VISUALIZERS.md). A visualizer may call more than one — e.g. a segmentation visualizer can push points into the 3D scene and a chart into the artifact inspector simultaneously.

**Multiple artifacts loaded at once.** The old UI's toggled-artifact model is preserved and made first-class in the layout (see §5). Loading another artifact does not unload the previous one; SVG / chart / JSON / HTML renders stack as a layer list in the panel overlay, and 3D layers accumulate in the scene.

**Cleanup.** The module's render function returns a `Cleanup` callback. The UI invokes it when the user removes that artifact layer. `rebuildViewerOverlays()`-style re-applies survive into v3 for the 3D surface.

| Feature | Entry point | Ports? |
|---|---|---|
| Per-artifact fetch + render | `app.js:3181-3270` | Rewritten around the renderer-module architecture above. |
| Toggleable artifacts (multiple active) | `app.js:3181-3345` | Ports. First-class in v3 (see §5). |
| Step-timeline highlighting | `app.js:1283-1370` | Ports. |
| Inline chart renderers | `histogram-chart.js:70-931`, `app.js:5028-5649` | Each becomes a `.viz.js` module living next to its producing step. No inline chart code in `app.js` equivalent. |
| Step description + params table | `app.js:1553-1598` | Ports; becomes a collapsible section in the DAG view node. |
| List-view artifact panel | `app.js:1451-1701` | **Dropped.** Spatial (DAG) mode is the only mode. |
| Graph-view (DAG node editor) | `artifact-graph-view.js:1123-1252` | Ports. Remains the primary artifact-inspection surface. |

### 4.4 3D viewer

| Feature | Entry point | Ports? |
|---|---|---|
| Core viewer bootstrap | `app.js:1968-2193` | Ports. Three.js scene, OrbitControls, ortho/perspective, lights. |
| Perspective ↔ orthographic toggle | `app.js:1936-1967` | Ports. |
| Point click inspector | `app.js:2005-2054` | Ports. The inspector is a generalization of today's per-point tooltip: it shows whatever `properties` dict the 3D-layer primitive declared. HTML overlay floats next to the pick. |
| Axis + compass gizmo | `app.js:2056-2135` | Ports with one change: full-degree marine-compass graduation. Every degree ticked, every 10° labeled, like a ship's compass rose. Replaces the current 4-cardinal rose. |
| Highlight specific points | `app.js:1380-1421` | Ports. Exposed to visualizers as `ctx.render3D` properties, not a global function. |
| Escape-to-clear artifacts | `app.js:6657-6663` | Ports. |

All other 3D rendering — point clouds, planes, edges, markers, poly-surfaces, ground-plane image overlays, per-artifact legends — is expressed through the `ctx.render3D` primitive layers described in §4.3. The viewer owns scene bootstrap, camera, gizmo, click inspector, and the primitive compiler (layer-JSON → Three.js meshes). Renderer modules never import Three.js.

**Scene visibility surface is minimal at L1–L7.** Only a point-size slider is exposed. The fine-grained visibility matrix from the old UI (per-plane, GT/non-GT, wall/roof, bitmask filters…) is replaced by per-layer toggles auto-registered through `ctx.render3D`. **L8–L9 expand this** by binding lab-ui to `@dioramap/scene-base`'s SceneState surface (sun + visibility tree + tri-state cascade) and shipping `3DSceneStatePanel` — at that point lab-ui exposes the full visibility tree with filter / deselect-all / collapsible groups, generic across composers. See PLAN-VIEWER-SDK-PORT §4.3 (SceneState API) and §4.7 (viz module location rule) for the shared-package contract.

**Cross-tab BroadcastChannel camera sync: dropped.** Not needed for v3.

### 4.5 Step timeline + log

The old "Metrics + results" right column shrinks to two items. Scores become artifacts.

| Feature | Entry point | Ports? |
|---|---|---|
| Composite score bar, summary stats, metrics breakdown | `app.js:1027-1280` | **Dropped as UI chrome.** Scores ship as artifacts emitted by the scoring step, rendered by an HTML `.viz.js` module (§4.3). The "right column of metrics" surface is gone. |
| Step timeline bars | `app.js:1283-1341` | Ports. Per-step execution duration with `CACHED` / cache-miss badges and iter labels. This is the only surviving persistent right-column element. |
| Log viewer | `app.js:1903-1927` | Ports as an ephemeral stream. Today it collapses the `data.log[]` array from the run response; in v3 it consumes existing WS events (`STEP_STARTED`, `STEP_COMPLETE`, `ERROR`) and log-event frames, displayed as a rolling text panel with a line-count header. No dedicated persistent "log artifact" panel. |

### 4.6 Map picker + live buildings

The v3 lab UI does **not** ship its own map picker. The viewer SDK's map-picker component (`v3/apps/location-picker/`) is embedded. A single implementation, reused.

| Feature | Entry point | Ports? |
|---|---|---|
| Map picker (IGN basemap, click-to-pick, geocoder) | `map-picker.js:17-98` | **Replaced** by embedding the viewer SDK's picker. No reuse of current lab UI picker code; no reimplementation. |
| Live-run flow (click a location → RUN) | `app.js:6722-6764` | Ports as a thin orchestrator that consumes the SDK picker's pick event and fires a RUN. |
| Save live TC | `app.js:6771-6810` | Ports (gated on §11.Q2). |
| Footprint overlay on map | `map-picker.js:104-142` | **Dropped.** |

### 4.7 Genealogy / history

The old UI has a standalone full-page genealogy DAG plus an embedded DAG used as the pipeline picker. In v3 the DAG picker is replaced by a filterable list; the standalone genealogy page does not port.

| Feature | Entry point | Ports? |
|---|---|---|
| Standalone genealogy SVG page | `genealogy.html`, `genealogy.js` | **Dropped.** |
| Embedded genealogy-as-pipeline-picker | `app.js:434-556` | **Dropped.** Pipeline selection is a flat, filterable list. |
| Pipeline / run history | — | A new thin `history-list/` module exposes past runs and pipeline selections as a filterable list with a text-input filter. No DAG, no connection edges, no detail panel. |

### 4.8 Layout + UX state

| Feature | Entry point | Ports? |
|---|---|---|
| Resizable column grid | `app.js:256-380` | Ports. Column count shrinks to three (left selector / center artifact-DAG + overlays / right timeline + log). |
| URL state persistence | `app.js:392-400` | Ports. |
| Viewer ResizeObserver | `app.js:2181-2189` | Ports. |
| Spinner overlay | `app.js:241-248` | Ports. |

### 4.9 Removed features (summary)

Consolidated for reference. Nothing below ports to v3.

- Cross-tab BroadcastChannel camera sync (`app.js:118-124, 2195-2217`).
- Genealogy DAG pipeline picker and standalone page (`genealogy-graph.js`, `genealogy.html`, `genealogy.js`, `ui/data/genealogy.js`). Replaced by a filterable list.
- All dropout-survey tooling (`ui/server.js:1117-1153`, any UI it fed). Research topic abandoned.
- Synthetic test-case loaders and all GT-validation surfaces. Synthetic cases do not port; GT-dependent scoring, validation badges, and save-boundary-TC go with them.
- The boundary-bench dedicated benchmark (save-boundary-test-case, boundary-algo in-browser playground).
- List-view artifact panel — spatial DAG mode is the only artifact inspection surface.
- Metrics / scores / results right column. Scores are artifacts now.
- Footprint overlay on the map picker.
- `dahouet-analysis.html` and other debug/one-off pages.

## 5. Feature inventory — UI surface

Things that make the UI feel the way it feels. Port-as-is unless noted.

### 5.1 Layout primitives

Three-column grid declared via CSS variables. Grid order: **left selector | center artifact-DAG view + overlays | right timeline/log**. The center column holds the artifact DAG view as the default surface; the 3D scene and the overlay panel (SVG / chart / JSON / HTML renders) are layered over it on demand. Column widths are localStorage-persisted.

Status bar is a fixed 36 px row across the full grid width.

### 5.2 Artifact layer list

The single biggest layout change from the old UI. The overlay panel above the DAG view shows a **layer list column**: one row per loaded artifact, with

- a type icon (one of the five base renderer types — see §5.3),
- the artifact name,
- a remove button.

Multiple artifacts can be loaded simultaneously. SVG / chart / JSON / HTML renders stack inside the overlay panel in the order the user loaded them; 3D layers accumulate in the scene. Clicking an icon in the layer list focuses that layer's render surface.

### 5.3 Artifact type icons

Five icons, one per base renderer surface. Replaces the old unicode-scatter vocabulary (▲ plane, ◻ solar, ☇ inline chart, etc.).

| Surface | Icon |
|---|---|
| 3D layers | a box-outline glyph |
| SVG | an `<svg>`-tag glyph |
| Chart | a bar-chart glyph |
| JSON | a braces glyph |
| HTML | an `<html>`-tag glyph |

Final shapes are a design task at L3; the constraint is one recognizable icon per surface, not per artifact kind.

### 5.4 Dark theme + color tokens

CSS variables in `style.css:5-31`, GitHub-dark lineage. Port as-is.

### 5.5 Panel semantics

- **Left panel** — selection only (pipeline + test case, both as flat filterable lists).
- **Center panel** — artifact DAG view is the default. The 3D scene is a modal layer; the overlay panel (artifact layer list + stacked renders) is another modal layer. Blender-style modelessness between them.
- **Right panel** — read-only. Re-rendered per run. Only the step timeline and the log viewer live here.

### 5.6 Keyboard shortcuts

- `Cmd/Ctrl+Enter` = run
- `Esc` = clear all overlays, leave 3D view

Port as-is.

### 5.7 Badge vocabulary

- `CACHED` on step timeline = cache hit
- `DISABLED` = step skipped
- `iter N` = step was part of a repeat-block

Port.

### 5.8 Spinner + loading

One global spinner with a text label. Shown by `showSpinner(text)`. No per-artifact spinners; individual artifact layer rows dim until their visualizer resolves.

### 5.9 Status-bar affordances

`statusPipeline`, `statusTestcase`, `statusTime`, `statusCache`, plus the Map button (opens the embedded viewer SDK picker). Save-TC appears when the context is a live run.

## 6. Current architecture snapshot

Used only for "how much code is displaced." The legacy UI runs unchanged on port 4200 throughout the port.

### 6.1 File sizes (legacy UI)

```
 6819  ui/public/app.js              ← monolithic; all top-level features
 2196  ui/public/style.css
 1349  ui/public/artifact-graph-view.js  ← node-editor view
 1167  ui/server.js                  ← Fastify backend
  931  ui/public/histogram-chart.js
  635  ui/public/genealogy-graph.js  ← (not ported)
  571  ui/data/genealogy.js          ← (not ported)
  153  ui/public/map-picker.js       ← (not ported; SDK picker reused)
  130  ui/public/index.html
──────
 ~13.9k total kept-in-scope (genealogy + standalone picker subtract out)
```

### 6.2 Smells that drove the rewrite

- **`app.js` is one file.** 6819 LOC with ~120 top-level functions and mutable module-global state. Grep-navigable but not editable by two people at once and not testable in isolation. Largest port cost.
- **Artifact-kind dispatch is spread across `applyArtifactToViewer()` and ad-hoc name matching** (`app.js:3234-3270`, `3236-3245`, `1628`, `1845`). Name-based dispatch for inline 2D artifacts requires edits in both `app.js` and `artifact-graph-view.js`. Replaced by the renderer-module architecture in §4.3.
- **`ui/server.js` is half-RPC, half-runner.** `/api/run` holds the entire runPipeline invocation + metric computation + viewer-data assembly inline. v3 moves all of this into `dioramap-processor`. The server disappears once L1 ships.
- **Per-artifact HTTP fetches re-run the whole pipeline.** `/api/artifact` calls `runPipelineWithContext()` and re-extracts one artifact (`server.js:894-982`). Works because everything's cached. v3's `SUBSCRIBE` primitive replaces this cleanly.

### 6.3 Where state lives (legacy)

Pipeline catalog, current selection, current run result, 3D scene objects, active viewer overlays, column widths — all are module globals in `app.js`. A clean port means each concern owns its state behind a module boundary.

### 6.4 Transport

HTTP JSON against Fastify. No WebSocket, no streaming. Every run is one round-trip; every artifact is one POST. v3 is WS-only.

## 7. Per-module specification (v3 rewrite)

Each subsection names a target module under `v3/apps/lab-ui/src/`, its responsibility, and how it maps to the legacy code.

### 7.1 `protocol/` — WS client

> **Post-L8 update.** This module was extracted out of lab-ui to the shared package `@dioramap/protocol` (commit `33acfb3`, P1 milestone — see L8 row in §10 status table).  Lab-ui now consumes it as a `file:` dep alongside the viewer SDK; both draw from one source of truth.  The design below describes the responsibility shape; the code lives in `v3/apps/protocol/`, not `v3/apps/lab-ui/src/protocol/`.

**Responsibility.** Single WebSocket to `dioramap-processor`. Sends CATALOG / RUN / SUBSCRIBE / CANCEL / RECONNECT. Handles SESSION_STARTED, RUN_STARTED, STEP_STARTED, ARTIFACT_HEADER + binary frame, STREAM_CHUNK, STEP_COMPLETE, RUN_COMPLETE, SUBSCRIBE_ACK, ERROR. Reconstructs an `Artifact` object per `ARTIFACT_HEADER` by draining the subsequent binary frame; dispatches to subscribers by artifact name.

**Spec.** Event-bus surface: `run-started`, `step-started`, `artifact`, `step-complete`, `run-complete`, `subscribe-ack`, `error`, `log-event`. Owns the session token for RECONNECT within the grace window.

**Port source.** Written from scratch. Nothing in `app.js` matches the WS model.

**LOC budget.** ~400.

### 7.2 `pipeline-catalog/` — pipeline + test-case + step metadata

**Responsibility.** Fetch and cache the catalog (via CATALOG), per-pipeline artifact list (CATALOG with `pipeline` arg), test-case fixtures (see §11.Q2), step metadata. Exposes synchronous read API + `onChange`.

**Spec.** Three maps: `pipelines`, `testCases`, `steps`. Flat lists with text-filter inputs — no categorized tree, no sibling groups, no DAG picker. `step-manifest` reads `meta.json` per step (step name + version + param defaults + doc).

**Port source.** Small amount of categorization logic ports; most is new.

**LOC budget.** ~200.

### 7.3 `history-list/` — filterable runs + pipelines

**Responsibility.** Show past pipeline runs and the catalog of pipelines in a single filterable list with a text input. No DAG, no graph, no connection edges, no detail panel. Clicking an entry loads it.

**Port source.** New. Replaces the ~2000 LOC of `genealogy-graph.js` + `genealogy.js` + `ui/data/genealogy.js`.

**LOC budget.** ~200.

### 7.4 `viewer/` — Three.js 3D scene

> **Post-L8 update.** The Three.js scene + `Viewer` class was extracted out of lab-ui to `@dioramap/scene-base/scene/` (commit `0f2c95a`, S1 milestone).  Only `viewer/viewer-toolbar.js` (the gear-popover scene-state controls) stays local to lab-ui; the viewer body is shared with the SDK.  The design below describes the responsibility shape; the code lives in `v3/apps/scene-base/src/scene/`.

**Responsibility.** Scene bootstrap, cameras, OrbitControls, gizmo, projection toggle, click inspector, point-size slider. Compiles declarative 3D-layer JSON (the `ctx.render3D` payload from visualizers) into Three.js meshes; tracks per-layer visibility; dispatches click events through the primitive back to an HTML inspector overlay driven by each primitive's declared `properties` dict.

**Spec.** `Viewer` class with `addLayer(id, layerSpec) → Cleanup`, `removeLayer(id)`, `setPointSize(px)`, plus the camera/gizmo/inspector internals. No per-kind rendering functions — every visual is expressed as primitives (point / plane / edges / cylinders / spheres / poly-surface / ground-raster).

**Compass.** Full-degree marine graduation (see §4.4).

**Port source.** Ports scene bootstrap (`app.js:1968-2193`), perspective/ortho toggle (`app.js:1936-1967`), click inspector (`app.js:2005-2054`), gizmo (`app.js:2056-2135`) — the rest of the viewer-adjacent code in `app.js` (the per-kind render functions) is displaced by the `renderers/` architecture.

**LOC budget.** ~1200.

### 7.5 `artifact-inspector/` — DAG view + overlay layer-list

**Responsibility.** The center column. Renders the artifact DAG (Blender-style node editor); on artifact click, subscribes through `protocol/`, then hands the resulting bytes to the artifact's visualizer (resolved per §7.6). Manages the overlay panel — the layer list of currently-loaded artifacts and the stacked SVG / chart / JSON / HTML render containers.

**Sub-modules:**

- `dag-view.js` — ports `artifact-graph-view.js` almost verbatim (1349 LOC, self-contained, dagre-based).
- `overlay-panel.js` — the per-artifact layer list and the render stack.
- `params-display.js` — collapsible per-step param table inside each DAG node (replaces `app.js:1553-1598`).

**Port source.** DAG view ports as-is; overlay panel is new; per-kind renderer dispatch is replaced by the registry in §7.6.

**LOC budget.** ~1400.

### 7.6 `renderers/` — visualizer dispatch + builtins

> **Terminology note.** The on-disk directory name `renderers/` is grandfathered from the original design (pre-VISUALIZERS.md doctrine).  Per [VISUALIZERS.md](../VISUALIZERS.md) §1, the canonical noun is **"visualizer"** ("renderer" is reserved for `THREE.WebGLRenderer`).  Renaming the directory + import paths is a separate ticket.

**Canonical reference: [VISUALIZERS.md](../VISUALIZERS.md)** — the umbrella vocabulary (visualizer / builtin-visualizer / step-visualizer / composite-visualizer), the five-surface `RenderContext` API, the per-subtype lifecycle, and how each surface maps to a lab-ui panel are all defined there.

**Lab-ui responsibility.** Lab-ui owns the **builtin-visualizer** stack (`renderers/builtin/<kind>.js`) and the dispatcher that resolves which visualizer fires per artifact:

1. step-visualizer (`v3/steps/<producer>/viz/<artifact>.viz.js`) if present — L11.
2. else builtin-visualizer keyed on artifact kind.

Composite-visualizers ship in `@dioramap/scene-base/viz/` and reach lab-ui via the composite-views panel (L10). Lab-ui never owns a composite-visualizer.

Builtins shipped in `renderers/builtin/`: `point-column.js`, `raster.js`, `image-jpeg.js`, `line-segments.js`, `metadata-json.js`. One per kind in `AK.*`; the dispatcher falls back to a `null`-placeholder for unknown kinds.

**LOC budget.** ~600 (loader + API + builtins). L10/L11 add the composite + step paths on top.

### 7.7 `map-picker/` — viewer-SDK wrapper

**Responsibility.** Embed the viewer SDK's picker component; surface its pick event to the app as `{lat, lon}`; fire a RUN with that payload through `protocol/`.

**Spec.** Thin wrapper. No IGN basemap config here — the SDK picker owns it. No footprint overlay, no geocoder config; whatever the SDK ships is what appears.

**Port source.** The legacy `map-picker.js` is discarded. The live-run orchestrator (`app.js:6711-6810`) ports.

**LOC budget.** ~150.

### 7.8 `params-editor/` (new)

**Responsibility.** Edit step-level param overrides before Run. Reads from the step manifest; writes overrides into the RUN message's `params`. Today the legacy UI only displays params read-only (`app.js:1560-1597`).

**LOC budget.** ~250.

### 7.9 `layout/` — grid, resize, URL, keybinds, spinner, status bar

**Responsibility.** §5 surface. Column resize, `applyColumnWidths`, URL sync, keybindings, spinner, status bar.

**Port source.** Ports from `app.js:211-401, 6585-6665`.

**LOC budget.** ~400.

### 7.10 `app.js` (entry)

**Responsibility.** Wire the above together. `init()` fetches catalog, mounts viewer + artifact-inspector + right column, registers event listeners. Nothing else lives here.

**LOC budget.** ~200.

## 8. New module layout

> **Post-L8 update.** The tree below reflects the **original Phase C design**.  After L8 (commits `33acfb3` + `0f2c95a`), `protocol/` and the body of `viewer/` (`viewer.js` + `compass.js`) were extracted out to `v3/apps/protocol/` and `v3/apps/scene-base/src/scene/` respectively — both shared with the SDK.  Lab-ui consumes them as `file:` deps; only `viewer/viewer-toolbar.js` (gear popover) stays local.  The directories below marked `[extracted to @dioramap/<pkg> at L8]` are no longer present in the lab-ui tree.

```
v3/apps/lab-ui/
├── index.html
├── vite.config.js                 (per UNIFIED §11)
├── src/
│   ├── app.js                     (composition root, ~200 LOC)
│   ├── protocol/                  [extracted to @dioramap/protocol at L8/P1]
│   │   ├── ws-client.js
│   │   └── artifact-decoder.js    (§5 binary layout reader)
│   ├── pipeline-catalog/
│   │   ├── catalog.js
│   │   ├── test-cases.js
│   │   └── step-manifest.js
│   ├── history-list/
│   │   └── list.js
│   ├── viewer/                    [body extracted to @dioramap/scene-base at L8/S1; only viewer-toolbar.js remains local]
│   │   ├── viewer.js              (scene + layer compiler)
│   │   ├── gizmo.js               (marine compass)
│   │   ├── click-inspector.js
│   │   └── point-size.js
│   ├── artifact-inspector/
│   │   ├── dag-view.js            (ports artifact-graph-view.js)
│   │   ├── overlay-panel.js
│   │   └── params-display.js
│   ├── renderers/
│   │   ├── loader.js              (dynamic import by kind / name)
│   │   ├── context.js             (5-surface API)
│   │   └── builtin/
│   │       ├── point-column.js
│   │       ├── raster.js
│   │       ├── image-jpeg.js
│   │       ├── line-segments.js
│   │       └── metadata-json.js
│   ├── map-picker/
│   │   └── wrapper.js             (embeds viewer SDK picker)
│   ├── params-editor/
│   │   └── editor.js
│   ├── timeline/
│   │   └── step-timeline.js
│   ├── log/
│   │   └── log-panel.js           (consumes WS events)
│   └── layout/
│       ├── grid.js
│       ├── url-state.js
│       ├── keybinds.js
│       ├── spinner.js
│       └── status-bar.js
└── fixtures/                      (per UNIFIED §11)
```

### Import direction (no circular deps)

```
layout ── app.js ──┬── protocol
                   ├── pipeline-catalog
                   ├── history-list
                   ├── viewer
                   ├── artifact-inspector ── viewer
                   │                      ├── renderers
                   │                      └── protocol
                   ├── renderers ── viewer   (for ctx.render3D)
                   ├── timeline ── protocol
                   ├── log ── protocol
                   ├── map-picker ── pipeline-catalog + protocol
                   └── params-editor ── pipeline-catalog
```

`viewer/` depends on nothing else in the app. `renderers/` uses `viewer/` exclusively through the `ctx.render3D` primitive-layer call. `artifact-inspector/` uses `viewer/` (to host 3D overlays), `renderers/` (to dispatch), and `protocol/` (to subscribe). Nothing imports `artifact-inspector/`.

### LOC budget summary

| Module | Budget | Source today |
|---|---|---|
| protocol | 400 | 0 (new) |
| pipeline-catalog | 200 | ~150 in `app.js` + `server.js` |
| history-list | 200 | (replaces ~2000 of genealogy — net drop) |
| viewer | 1200 | ~1400 in `app.js` (less, because per-kind functions moved out) |
| artifact-inspector | 1400 | ~1500 in `artifact-graph-view.js` + `app.js` |
| renderers (loader + built-ins) | 600 | (replaces ~2000 of in-`app.js` kind dispatch + `histogram-chart.js`) |
| map-picker | 150 | ~230 in legacy (discarded; SDK picker reused) |
| params-editor | 250 | 0 (new) |
| timeline | 150 | ~100 in `app.js` |
| log | 100 | ~30 in `app.js` |
| layout | 400 | ~400 in `app.js` |
| app.js | 200 | (composition root) |
| **Total** | **~5250** | **~13.9k today (in-scope)** |

Net shrink: ~62%. Dropped features (genealogy, list-view, metrics panel, map picker, dead pages, GT plumbing) account for the bulk.

## 9. Build + tech choices

**Keep:**

- **Plain ESM modules, no framework.** Vanilla JS stays vanilla.
- **Three.js** for the 3D viewer.
- **Chart.js** for charts. Assumed globally available to visualizers per §4.3.
- **dagre** for DAG layout in `artifact-inspector/dag-view.js`.

**Add:**

- **Vite** as build + dev server, per UNIFIED §11.

**Drop:**

- **Fastify.** No node server in v3; `dioramap-processor` serves static files.
- **CDN importmap for Three / Chart.** Vite vendors these.
- **MapLibre GL** (as a lab-UI dependency). It comes in transitively through the viewer SDK's map picker, not as a direct lab-UI import.

**Decided:**

- **No TypeScript.** Plain ESM + JSDoc where it earns its keep. JSDoc is enough for the WS client and the renderer-module contract.

## 10. Port order milestones

L1–L7 shipped during Phase B alongside PLAN-VIEWER-PORT B1–B10 (the old bench UI ran on port 4200 until L7 made it redundant). L6 (history list) shipped 2026-04-28 as a three-part stack: SQLite journal in the C runner, LIST_RUNS / REHYDRATE_RUN WS handlers, full-page history overlay in lab-ui. L8–L9 belong to Phase C and depend on PLAN-VIEWER-SDK-PORT shipping the `@dioramap/scene-base` package — they are tracked here because the work happens inside lab-ui, but their unblocking comes from the SDK port plan.

**L1 — WS client + CATALOG + filterable pipeline/TC lists.** ✅ done (commit `87de800`). Stand up `protocol/`, `pipeline-catalog/`, minimal `app.js`. Render pipeline list + test-case list with filter inputs. No viewer, no artifacts, no run. Validation: user can browse v3's pipelines + TCs.

**L2 — Three.js viewer + first renderer modules.** ✅ done (commit `87de800`). `viewer/` module shipped (scene, camera, gizmo with marine-compass graduation, click inspector, point-size slider). `renderers/` loader + built-in fallbacks for point-column, raster, image-JPEG, line-segments, metadata-JSON. Ship `.viz.js` for the initial set of v3 artifacts: `canonical-location` (HTML), `scene-origin` (JSON), `ortho-rgb-jpeg` (raster overlay), `lidar-positions` (3D point cloud). Validation: runs the v3 smoke pipeline and every artifact it emits renders.

**L3 — Artifact DAG view + overlay layer list + multi-artifact stacking.** ✅ done (commit `87de800`). `artifact-inspector/` module shipped. Dagre DAG view ports. Overlay panel stacks multiple renders; 3D layers accumulate in the scene. Per-artifact type icons (§5.3) land here. Validation: user loads three artifacts simultaneously and all three renders coexist.

**L4 — Step timeline + log panel.** ✅ done (commit `dd3ee83`). Right column. Timeline consumes `STEP_STARTED` / `STEP_COMPLETE` WS events with cache-hit/miss badges; log panel streams `log-event` frames.

**L5 — Params editor.** ✅ done (commit `dd3ee83`). Add per-step param override UI. RUN messages carry overrides.

**L6 — History list (run journal + lab-ui panel).** ✅ done (commits `694da41`, `648a73d`, `4f4940d`).

Three-part stack delivered as designed below:

1. **L6.1** — SQLite journal at `<cache>/runs.sqlite` in WAL mode (`core/src/runner/run_journal.{c,h}`). One row per completed run; PK is an auto-increment `journal_id` (the runner's `run_id` resets per server-restart and was unsafe as the durable handle). Wired into `cb_on_run_started` / `cb_on_artifact_ready` / `cb_on_run_complete` via a per-session accumulator on `session_t`. Retention pruned at startup (>30d OR >10k rows).

2. **L6.2** — Two new WS messages in `core/src/server/server.c`. `LIST_RUNS { limit?, before?, pipeline?, input_hash?, success? }` → `LIST_RUNS_RESPONSE { runs[], next_before }`. `REHYDRATE_RUN { journal_id }` → emits one `ARTIFACT_HEADER` + binary frame per produced artifact still present in the store, then a `REHYDRATE_COMPLETE { n_emitted }` or `REHYDRATE_PARTIAL { n_emitted, missing[] }` summary. Synthetic `run_id = "rehy_<jid>"` on rehydrated headers so clients route through their normal artifact pipeline without colliding with active-runner ids.

3. **L6.3** — Full-page modal overlay in lab-ui (`apps/lab-ui/src/history-panel/{overlay.js, overlay.css}`). z-index 200 above everything. Topbar button + `H` key open it; Esc / backdrop / ✕ close. Filters (pipeline dropdown, input substring, success/fail, fresh/cached). Click a row → `WSClient.rehydrateRun(journal_id)` + close overlay + `setCenterMode('inspect')`; existing artifact-handler pipeline replays the cached bytes into the inspector. `WSClient.run()` accepts an optional `testCaseLabel` 5th arg; lab-ui synthesises one from `input.{lat,lon,radius_m}` so rows show something readable.

Validation outcomes were the spec's bullet list:
- Two distinct sessions issuing fresh runs → `journal_id` 1 and 2, no collision.
- Server restart with the journal kept → `LIST_RUNS` still returns both rows.
- Wipe the store but keep `runs.sqlite` → `REHYDRATE_PARTIAL` lists every artifact as missing, `n_emitted=0`.
- `REHYDRATE_RUN` with bogus `journal_id` → `UNKNOWN_RUN`; missing `journal_id` field → `BAD_MSG`.
- Cache-hit re-run records `n_executed=0, n_cache_hits=N` correctly.

Original design notes preserved below for reference.

**Goal.** A filterable panel in lab-ui showing recent pipeline runs (timestamp, pipeline, test case, cache-hit ratio, duration, success/fail). Clicking a row re-loads that run's artifacts into the inspector — without re-running anything — so the algorithm dev can A/B-compare run #7 ("yesterday's good output") against run #29 ("this morning's regression"). No DAG, no edges, no detail panel beyond the row itself; clicking just rehydrates the inspector from cache.

**Why it can't be built today (cache audit, 2026-04-27).** The v3 store is purely content-addressable: `<cache_root>/<aa>/<bb>/<full-hash>.bin` (`v3/core/include/v3/store.h:19`). The intent map is an append-only `identity_hash → content_hash` log with no timestamps, pipeline names, or test case identifiers (`v3/core/include/v3/intent_map.h:29`). The runner keeps `run_state_t` in memory and discards it on `on_run_complete` (`v3/core/src/runner/runner.c:28`). The server's `cb_on_run_complete` (`v3/core/src/server/server.c:790`) forwards the event to the WebSocket and forgets it. There's a structured-logging vocabulary (`EV_RUNNER_RUN_STARTED`, `EV_RUNNER_RUN_COMPLETE` in `v3/core/include/v3/log_events.h`) but it goes to stderr, not to a queryable journal. **Conclusion: L6 needs net-new persistence that the system doesn't have today.**

**Where the journal lives.** Processor-side, not control-plane. Decided 2026-04-27 after auditing `~/control-plane/` (Postgres + Fastify, existing `usage_events` table at `sql/migrations/004-usage.sql`, ingest at `src/internal/routes.js:100`). The control-plane DB is the right place for prod analytics + cross-customer billing + support reproduction across processor nodes — but L6 is a **dev-time** lab-ui panel talking to a local `dioramap-processor` instance, so a processor-local journal is the right granularity (no SaaS round-trip, works offline, captures full forensic detail the prod usage event doesn't carry). Prod analytics work is tracked separately as **A14 (control-plane run telemetry)** in `MAIN-IMPL-PLAN.md` — see "Stitching together" below. (A14 has since shipped per commit `06b35c2`.)

**Scope.**

1. **Processor-side journal (C).** Add a new translation unit `v3/core/src/runner/run_journal.{c,h}`. Backed by SQLite for indexed range/filter queries; library is small and already a common dep. Journal file lives at `<cache_root>/runs.sqlite`. Schema:
   ```sql
   CREATE TABLE runs (
     run_id          TEXT PRIMARY KEY,           -- session-scoped UUID
     pipeline_name   TEXT NOT NULL,
     input_json      TEXT NOT NULL,              -- canonicalised RUN.input
     input_hash      TEXT NOT NULL,              -- hex sha256 of input_json
     started_at      INTEGER NOT NULL,           -- epoch micros
     completed_at    INTEGER,                    -- NULL while in-flight
     duration_us     INTEGER,
     success         INTEGER,                    -- 0 / 1; NULL while in-flight
     error_kind      TEXT,                       -- short tag if !success
     error_message   TEXT,
     n_executed      INTEGER NOT NULL DEFAULT 0,
     n_cache_hits    INTEGER NOT NULL DEFAULT 0,
     subscribe_set   TEXT NOT NULL,              -- JSON array of artifact names
     produced_hashes TEXT                        -- JSON {artifact_name: content_hash}
   );
   CREATE INDEX runs_started_at_idx ON runs(started_at DESC);
   CREATE INDEX runs_pipeline_input_idx ON runs(pipeline_name, input_hash);
   ```
   Wired into the runner via three callbacks: `run_journal_begin(run_id, pipeline, input_json, subscribe_set)` from `cb_on_run_started`; `run_journal_step_complete(run_id, cache_hit)` from `cb_on_step_complete` (counter increments only — no per-step row to keep the table compact); `run_journal_end(run_id, success, error_*, produced_hashes)` from `cb_on_run_complete`. Single-writer (the run thread), reader is the LIST_RUNS handler — SQLite WAL mode handles the concurrency. Bounded retention: drop runs older than 30 days OR keep at most 10k rows, whichever lower. ~250 LOC.

2. **WS protocol additions.** Two new client→server messages (handler in `v3/core/src/server/server.c`):
   - `LIST_RUNS { limit?, before?, pipeline?, input_hash?, success? }` → `LIST_RUNS_RESPONSE { runs: [{run_id, pipeline, input_hash, started_at, duration_us, success, n_cache_hits, n_executed, subscribe_set}] }`. Default 50 rows newest-first; `before` cursor on `started_at` for pagination.
   - `REHYDRATE_RUN { run_id }` → server reads `produced_hashes` from the journal, looks each up in the store, emits `ARTIFACT_HEADER` + binary frames the same way a normal RUN does. If any content hash is no longer in the store (LRU-evicted), respond `REHYDRATE_PARTIAL { missing: [...] }` and emit only what's present — lab-ui shows the missing ones grayed out. ~150 LOC.

3. **Lab-ui panel (`apps/lab-ui/src/history-panel/`).** New right-column section above the run-panel (or behind a button — TBD when wireframing). Sends `LIST_RUNS` on app boot and on a manual refresh. Filters: pipeline (dropdown from CATALOG), text filter on input JSON (substring), date range, success/cache toggles. Row layout per the §4.7 brief: `2026-04-27 14:22  v6a-base  building-foo  fresh  18.4s  ✅`. Click row → sends `REHYDRATE_RUN { run_id }`, switches center-pane to inspect mode (reuses L11's inspector dispatch + the composite-views panel), inspector renders artifacts as they arrive. Selection state in URL hash so a refresh/restore lands on the same view. ~300 LOC + ~80 LOC CSS.

4. **Test case identity (Q2 carry-over).** Today the only "test case" the processor sees is the input bytes hash. The lab-ui knows the human-readable test case name (selected from the test-case list); add an optional `test_case_label` field to RUN, persist it in the journal as a denormalised column. Doesn't change cache behaviour (cache key stays input_hash). ~10 LOC.

**Validation.**
- Run a pipeline 3 times against test case A, twice against test case B. History panel shows 5 rows newest-first with correct labels and durations.
- Click row #2 (oldest A run) → inspector clears, the run's artifacts reappear, no `RUN` is sent (no fresh compute).
- Restart the processor; history persists across restarts.
- Manually evict one of the run's artifacts from the store; rehydrate that run → `REHYDRATE_PARTIAL` shipped, only present artifacts paint, missing ones show grayed in the inspector.
- Filter by pipeline=v6a-base → only matching rows.
- Add 10k+ runs (CI smoke); journal stays under retention bound, query latency < 50ms.

**Stitching with control-plane (out-of-scope for L6, tracked as A14).** When `dioramap-processor serve` runs in prod (with a control-plane URL configured), the same `cb_on_run_complete` that writes the local journal should also batch a usage event to `POST /internal/record-usage` (already implemented at `~/control-plane/src/internal/routes.js:100`). This depends on **A10 (control-plane client)** in `MAIN-IMPL-PLAN.md` shipping first — the C-side libcurl client wrapping `cp_verify_key` + `cp_record_usage` (sketched in `PLAN-FETCHERS.md` lines 70–88). L6 itself does not depend on A10; the local journal is the source of truth. The CP push is a denormalised aggregate.

**Required control-plane changes (tracked as A14 — Control-plane run telemetry).** The current `usage_events` schema (`sql/migrations/004-usage.sql`) was designed for the viewer SDK's **scene-load** events: `(key_id, org_id, event_type, l2_hit, compute_ms, lat, lon, created_at)`. To support both prod analytics on the lab/processor side and support-team reproduction, the following are needed:

1. **New migration `008-run-telemetry.sql`** — additive (no destructive changes to `usage_events` since the existing aggregations and dashboard query against it). New table:
   ```sql
   CREATE TABLE pipeline_runs (
     id                  BIGSERIAL PRIMARY KEY,
     run_id              TEXT NOT NULL,                -- session-scoped UUID from processor
     processor_node_id   TEXT NOT NULL,                -- processor instance identifier
     key_id              UUID REFERENCES api_keys(id) ON DELETE SET NULL,
     org_id              UUID REFERENCES organizations(id) ON DELETE SET NULL,
     pipeline_name       TEXT NOT NULL,
     input_hash          TEXT NOT NULL,                -- hex sha256, matches journal
     test_case_label     TEXT,                         -- nullable (prod has no label)
     started_at          TIMESTAMPTZ NOT NULL,
     duration_ms         INTEGER NOT NULL,
     success             BOOLEAN NOT NULL,
     error_kind          TEXT,
     n_executed          INTEGER NOT NULL,
     n_cache_hits        INTEGER NOT NULL,
     compute_ms          INTEGER NOT NULL,
     bytes_out           BIGINT NOT NULL,
     subscribe_set       TEXT NOT NULL,                -- JSON
     created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
   );
   CREATE INDEX pipeline_runs_org_started_idx     ON pipeline_runs(org_id, started_at DESC);
   CREATE INDEX pipeline_runs_processor_run_idx   ON pipeline_runs(processor_node_id, run_id);
   CREATE INDEX pipeline_runs_pipeline_input_idx  ON pipeline_runs(pipeline_name, input_hash);
   ```
   Why a separate table and not extra columns on `usage_events`: `usage_events` is high-cardinality scene-load telemetry with 90-day retention. Pipeline runs are coarser, kept longer (12 months) for support, and don't share dashboard queries. Different lifecycles, different retention, different access pattern.

2. **New ingest endpoint `POST /internal/record-run`** in `src/internal/routes.js`. Accepts a batch shape mirroring `/internal/record-usage`:
   ```js
   [{
     api_key, run_id, processor_node_id, pipeline_name,
     input_hash, test_case_label?, started_at, duration_ms, success,
     error_kind?, n_executed, n_cache_hits, compute_ms, bytes_out, subscribe_set
   }, ...]
   ```
   Resolves `api_key → key_id, org_id` (same `resolveApiKey` helper as `record-usage`), single multi-row INSERT. Returns 202. ~50 LOC including validation. Out of consistency with the existing endpoint, also accept lat/lon if present and write a paired row to `usage_events` so the viewer-tier dashboard keeps working when `serve` mode handles a prod RUN — avoids double-emit logic on the processor side.

3. **GDPR retention job** — extend the nightly aggregation (or add `scripts/prune-old-runs.js`) to drop `pipeline_runs` rows older than 365 days. The existing `usage_events` 90-day prune (per the comment header in `004-usage.sql`) is still correct for that table.

4. **Support-tooling endpoint `GET /api/admin/runs`** — admin-only (role check on the existing auth middleware in `src/auth/`), filterable by `?org_id=`, `?pipeline_name=`, `?run_id=`, `?since=`. Returns `pipeline_runs` rows newest-first with pagination. Used by support staff to find a problematic run, then they hop to the processor by `(processor_node_id, run_id)`. ~40 LOC.

5. **Dashboard panel `/dashboard/runs`** (optional, can come later) — same endpoint, EJS view, admin-tier only. Skip for the initial cut; the API endpoint alone unblocks support workflows via curl.

6. **Webhook event `pipeline_run.failed`** (optional) — fire on insert when `success = false`, append to `webhook_events` table (already exists per `005-webhook-events.sql`). Lets internal alerting hook into recurrent failures without polling. Skip for v1.

7. **Processor-side `cp_record_run(client, run_record)`** (in A10's client). Buffers + batches the same way `cp_record_usage` does, but flushes to `/internal/record-run` instead. Sketched in `PLAN-FETCHERS.md` § "Control-plane client API" — needs adding alongside `cp_record_usage`.

**A14 LOC estimate** (control-plane only): ~150 lines (50 ingest + 40 admin endpoint + 20 retention job + 30 migration + 10 wire-up). Plus ~80 LOC on the processor side (record-run helper). Depends on A10 shipping.

**Support flow once A14 is live.** Customer reports issue at 14:22 UTC → support queries `/api/admin/runs?org_id=X&since=14:00` → identifies `(processor_node_id, run_id)` → SSH or remote-RPC the processor → query its local journal by `run_id` → recover `input_json` + `subscribe_set` → reproduce locally with `dioramap-processor batch --pipeline X --input <recovered-json> --output-dir /tmp/repro`. The processor's local journal carries the full input payload (cheap on disk) which the control plane never sees — only the `input_hash` flows to the cloud, preserving privacy.

**LOC estimate.** ~800 lines: ~250 C journal, ~150 C server handlers, ~300 JS panel, ~80 CSS, ~20 protocol schema additions in `@dioramap/protocol`.

**L7 — Map picker integration.** ✅ done (commit `f379e37`). Embed the viewer SDK's picker; wire its pick event to RUN with `{lat, lon}`. Gated on PLAN-VIEWER-PORT B1 (IGN fetchers). Validation: click a Paris location, see its point cloud.

At L7 the old lab UI became redundant.

**L8 — Lab-ui consumes `@dioramap/scene-base` + `@dioramap/protocol`.** ✅ done.
Both shared packages now consumed via `file:` deps. P1 (commit `33acfb3`) moved `protocol/{ws-client,artifact-decoder}.js` out and rewired four lab-ui imports. S1 (commit `0f2c95a`) moved `viewer/{viewer,compass}.js` out and rewired `app.js` imports. L8 itself: instantiated `SceneState` in `app.js` (placeholder lat/lon = 48.8566/2.3522 — Paris/Notre-Dame; lab-ui has no sun slider yet, so the value is unused but must be valid for SunCalc); threaded `sceneState` through `ArtifactInspector` → `RenderContext`; extended lab-ui's `RenderContext.render3D` to also `sceneState.visibility.attach()` each spec's Three.js object as a leaf, with cleanup wiring `clearSubtree` on layer removal. `OverlayPanel` and `renderers/builtin/*` unchanged. Validation: `npm run build` green; bundle 1643 kB / 460 kB gzipped (+10 kB / +4 kB gzipped vs pre-L8 — SceneState + SunCalc); dev server starts in 147 ms; visibility tree populates as artifacts render (verified by manual smoke).

**L9 — Scene-state controls in the gear popover.** ✅ done.
Extended `viewer/viewer-toolbar.js` (rather than shipping a separate `3DSceneStatePanel` — the user preferred one panel for all viewer settings). The gear button's popover now contains three new sections beyond the original (projection / point-size / compass): **Sky** (on/off pill), **Fog** (on/off pill, off by default; near=200 / far=500 hardcoded — best for ~500m scenes), **Sun** (3 sliders matching the legacy SDK's controls panel exactly: Month 1–12, Day 1–28, Time 0–143 packed as `slot = hour*6 + minute/10`, displayed `HH:MM`), and **Layers** (visibility tree from `sceneState.visibility.tree()` with: live leaf count, Hide all / Show all / Defaults buttons, filter input that recursively matches across path + label, tri-state master checkboxes per group with cascade on click). Live updates via `sceneState` events: `visibility-registered`, `visibility-changed`, `subtree-changed`, `subtree-cleared`, `sun-changed`. Sky / fog prefs persist via the existing `VIEWER_PREFS` mechanism. Render-context bug fixed during this milestone: lab-ui's `RenderContext.render3D` now looks up `viewer.layers.get(id).object` after `addLayer` to grab the just-built Three.js object (renderers don't pass `.object` in the spec — it's constructed inside `addLayer` from declarative props) and passes it to `sceneState.visibility.attach`. Without this, the layer tree was empty after every run. **Validation**: lab-ui build green (1653 kB / 462 kB gzipped); manual smoke confirmed: viewer-pointcloud run populates the tree with one leaf per artifact, sun sliders rotate the directional light visibly, sky/fog toggles work, filter narrows the tree.

**L9 follow-up — `full-scene.viz.js` integration.** ❌ **cancelled** (S6 dropped — see PLAN-VIEWER-SDK-PORT §7 S6). Lab-ui's composite-views panel from L10 covers the use case: SDK composites all carry `meta.default: true`, so they auto-render on pipeline load and a Hide-all → cherry-pick gets the SDK scene exactly when needed.

**L10 — composite-views panel (composite-visualizer toggles).** ✅ done.
Shipped `apps/lab-ui/src/composite-views/` with `dispatcher.js` (~200 LOC) and `panel.js` (~100 LOC). Dispatcher tracks `_kindByName` from CATALOG_RESPONSE, owns its own decoded buffer (separate from inspector — both consume the same WS artifact stream and decode independently), and per enabled composite-visualizer creates a `BaseRenderContext` reusing lab-ui's `Viewer` + `SceneState`, then runs `mod.render(ctx, decoded)` when all `meta.requires` are satisfied. Streaming triplet (`beginStream`/`appendChunk`/`finalizeStream`) routed for composites with `beginStream`; trailing dual-delivery `ARTIFACT_HEADER` suppressed after `final=true` (same rule as SDK's orchestration). Toggle off → `ctx.dispose()` removes scene contributions and visibility leaves. Panel renders one row per eligible entry (filtered by `vizCatalog.filterByCatalog(availableArtifacts)`) with a toggle, label, and a `[requires…]` mono badge; pre-checks rows where `meta.default === true`. Wired into `app.js`'s artifact + stream-chunk events alongside the existing inspector dispatch. New right-pane grid row for the section. Subscribe-side: dispatcher does NOT send its own `SUBSCRIBE` messages — relies on the run-panel's default subscribe-all checklist; lifts the `NO_RUN` errors that fired when toggles enabled before the first run. Also: registered `canopy-flags`/`grass-flags` in `POINT_COLUMN_SCHEMA` so they decode without crashing the inspector's point-column builtin.

**Validation**: lab-ui at `viewer-full` shows the same three `default:true` composites paint as the SDK on 5183 — point cloud (15000 points spread across the disc), deshadowed ground ortho, cadastre lines. Toggle off `Cadastre outlines` in the composite-views panel → yellow lines disappear from the 3D viewer AND the leaf vanishes from the gear-popover Layers tree. Toggle back on → comes back. Architecture confirmation **passed**: same `.viz.js` modules in `scene-base/viz/` paint the same scene in both lab-ui and SDK, with no host-specific code inside the visualizers themselves.

Sub-fix shipped alongside L10 (separate commit `a1aace7`): the server's `cb_on_artifact_ready` was emitting `(text-header, binary)` as two non-atomic `session_enqueue` calls. Concurrent on_artifact_ready callbacks from the runner's thread pool interleaved on the session queue (header_A, header_B, binary_A, binary_B), and the WSClient pairs by adjacency, so the client mis-paired headers with the wrong binaries. Surfaced under lab-ui's 36-artifact load (lidar-positions arrived as 90008 bytes of denormal F64 garbage instead of 360008 bytes); SDK's 13-artifact subscribe set was sparse enough that it hadn't tripped. Fixed via `session_enqueue_pair` + `session_enqueue_json_with_binary` that hold the lock across both inserts.

**L11 — step-visualizer dynamic-import.** ✅ done. `apps/lab-ui/vite.config.js` now allows the `v3/` root and dedupes `chart.js` so step-visualizers under `v3/steps/<step>/viz/` can `import 'chart.js'` from outside the lab-ui workspace; `apps/lab-ui/src/renderers/loader.js` discovers them via `import.meta.glob('../../../../steps/*/viz/*.viz.js', { eager: true })` and tracks the producing step in a side `STEP_VIZ_PRODUCER` map (Vite hands out frozen Proxy module objects in dev, so we can't attach a `_step` property to the module itself). A step-visualizer match wins over the builtin in both `resolveRenderer` (async) and `resolveRendererSync` (streaming dispatch). The previous per-name special cases in `apps/lab-ui/src/renderers/builtin/point-column.js` (normals coloured points, classifications/intensities/num-returns/spacing histograms) were lifted into per-artifact `.viz.js` files: `v3/steps/copc/viz/lidar-classifications.viz.js`, `v3/steps/copc/viz/lidar-intensities.viz.js`, `v3/steps/copc/viz/lidar-num-returns.viz.js`, `v3/steps/normals/viz/lidar-normals.viz.js`, `v3/steps/normals/viz/lidar-spacing.viz.js`. `v3/steps/copc/viz/lidar-positions.viz.js` was added too but is intentionally **JSON-stats-only with a streaming-progress overlay** — no 3D layer, because the composite-visualizer at `apps/scene-base/src/viz/lidar-positions.viz.js` already paints the `LiDAR positions (preview)` cloud. Avoiding the duplicate is the canonical example of how step-visualizer and composite-visualizer divide work when both target the same artifact (composite owns the production scene, step-viz adds inspector-only metadata/charts) — see VISUALIZERS.md §9. The `point-column` builtin is now a silent JSON-stats-only fallback for `AK_POINT_COLUMN` artifacts without a step-visualizer (canopy-flags / grass-flags / parcel-lines deliberately have no inspector chart — they paint via composite-visualizers in the production scene). **Validation**: lab-ui builds green; the `viewer-full` pipeline shows the inspector renderings produced by the new step-visualizers; the streaming inspector path resolves sync because the glob is `eager: true`; the 3D layer list shows only `LiDAR positions (preview)` (composite) and not a duplicate `lidar-positions` (step-viz).

## 11. Open questions

1. **Q1 — Run-summary / scores as subscribable artifacts.** The scoring step needs a declared output kind (metadata-JSON or HTML) that the lab UI subscribes to and a `.viz.js` that renders it. Simple to add; decision is which step emits it. Flagged for PLAN-VIEWER-PORT (B4).

2. **Q2 — Test-case fixture catalog.** Today the bench server owns `test-cases-v2/` and exposes an index; v3 has no concept of a fixture store. Two options: (a) keep bench v2 hosting fixtures on a side port; (b) add a catalog endpoint to `dioramap-processor` that serves a JSON index of a configured fixture directory. (b) is cleaner and ~100 LOC. Decide before L1.

3. **Q3 — Live-run save-as-TC.** Gated on Q2. No v3 concept yet for persisting a live-fetched building as a reusable fixture.

4. **Q4 — `.viz.js` lazy-loading path at runtime.** Modules live at `v3/steps/<step>/viz/<artifact>.viz.js`. Vite needs to know to pick them up; decide between a glob import on boot and an on-demand `import()` from a known URL pattern. Both work; prefer the latter for minimal bundle size.

## 12. Risks

- **`app.js` split is the single biggest port cost.** 6819 LOC of intermingled concerns; ten of the modules in §7 carve out of this one file. Splitting is mechanical but tedious, and per-module tests don't exist today. Mitigation: keep the old UI on port 4200 running alongside throughout L1–L7 and spot-check behaviour on a handful of real-building TCs.

- **Renderer-module contract first-draft.** The five-surface API in §4.3 is intentionally informal. The first two or three `.viz.js` modules will shake out the real contract — budget a small refactor after L3.

- **v3 protocol gaps (§11.Q1, §11.Q2).** Run-summary artifact shape and test-case fixture catalog aren't yet in §8.1. Both are small; flagged above.

- **Binary artifact decoding in the browser.** §5 binary layout is defined for C; a JS decoder is trivial for pure arrays but needs care for structured layouts (planes, edges, metadata). Prototype early, in L1 alongside the WS client.

## 13. What this plan does NOT commit to

- Reuse of `ui/server.js` — it retires once L1 ships.
- **TypeScript.** Plain ESM + JSDoc only.
- A framework rewrite (React / Vue / Svelte).
- **A genealogy DAG pipeline picker.** Replaced by a filterable list (§4.7, §7.3).
- **A dedicated metrics / scores / results panel.** Scores ship as artifacts + HTML `.viz.js` modules (§4.5).
- **A list-view artifact mode.** Spatial DAG view is the only artifact-inspection surface (§4.3).
- **A lab-UI-owned map picker.** The viewer SDK's picker is embedded (§4.6).
- Cross-tab camera sync, BroadcastChannel or otherwise.
- Porting dropout-survey tooling, synthetic test cases, GT validation, the boundary-bench playground, or the standalone debug HTML pages.
- Bench SQLite integration — bench v2 keeps its own DB; v3 doesn't.
- Matching `/api/artifact` cache-abuse semantics — the WS `SUBSCRIBE` model replaces it.
