# @dioramap/viewer-sdk changelog

Versioning: SDK semver per UNIFIED-ARCHITECTURE.md §4.10. Changes that
add/remove curated `EXPOSED_VISIBILITY_TOGGLES` entries (PLAN-VIEWER-SDK-PORT
§5.5) require a documented minor version bump here.

## Unreleased

## 0.10.0 — 2026-11-25

**Additive**: failures are shown and reported instead of freezing the loading bar (docs/ERRORS.md).

- **Error card.** On any failure the loading card turns into an error card: a plain-language title and hint (French when the page is French, English otherwise), the technical one-liner, a collapsible "Technical details" block with the full report, and **Copy**, **Report on GitHub** (pre-filled issue on `FL-AntoineDurand/dioramap-lab`) and **Try again** buttons.
- **`onError` receives a `DioramapError`** with `code`, `message`, `step`, `detail` (upstream URL, HTTP status, response excerpt…), `runId`, `input`, versions, and `toReport()`. Fixes a bug where `onError` always got `"server error"` (the SDK read `message`; the processor sends `msg`). Fires once per failed run.
- **Covered failures**: `RUN_FAILED` (processor ≥ 0.3.0), step `ERROR` from older processors (after a 1.5 s grace), request-level rejections (`AUTH_*`, `RUN_BUSY`, …), WebSocket never opening (`CONNECTION_FAILED`) or closing mid-run (`CONNECTION_LOST`), and no frame for 5 minutes during a run (`STALLED`).
- **New `init` options**: `locale` (`'fr'` / `'en'`), `issueRepo` (`'owner/repo'` for the report button), `reportIssues` (`false` hides it). API keys and tokens are redacted from reports.
- **`auth: { param, value }`** sends the credential under another query parameter than `token` (for a processor extension, docs/EXTENSIONS.md); **`plugins`** / **`pluginConfig`** mount add-ons that get `onRunStarted`, `onRunComplete`, `onRunFailed` and `dispose`.
- **Build:** Vite 6. The stylesheet is now emitted as `dist/dioramap-viewer.css` (was `dist/style.css`); link that file.



## 0.9.0 — 2026-05-07

**Additive**: `addModel` and `startGhostPlace` accept raw GLB bytes; existing `url` callers unaffected.

- **`addModel({ data })` and `startGhostPlace({ data })`** — both now accept `ArrayBuffer | Blob | TypedArray` of GLB bytes in addition to the existing `url`. Provided bytes are parsed via `GLTFLoader.parse` (no network call). Mutually exclusive with `url`; the SDK throws on neither / both. Use cases that motivated this: (a) procedural / configurator-generated models (e.g. a swimming-pool whose geometry depends on a length slider — customer code builds the GLB in-page, hands it to the SDK without going back to the network); (b) keeping strict-CSP `connect-src` minimal for embedders who don't want to allowlist their model-CDN host. The `data` path is purely additive — `url` remains the standard route for static models on a CDN.
- **`startGhostPlace` forwards the same source to `addModel` on click-confirm.** Whichever of `url` / `data` the caller passed gets reused for the placed-model load — same bytes parsed twice (once for the ghost, once for the placed instance), no re-fetch. Cloning a parsed glTF scene is fragile (materials, textures, animations); paying for one extra parse is the simpler contract.
- **Internal**: shared `models/gltf-source.js` helper centralises the `url`-vs-`data` dispatch + the `Blob` → `ArrayBuffer` coercion. Used by both `model-manager.js` and `ghost-placement.js`. ~50 LOC, no public surface.
- **Doc updates**: `landing/docs/api/viewer-sdk/#models` documents the `data` field with a procedural-pool snippet; `#styling` gets a "Modèles 3D — deux options pour la CSP" subsection covering both the URL path (`connect-src` allowlist required) and the `data` path (no SDK-side fetch); `landing/docs/index.html` "Notes pratiques" cross-links to the new option.
- **Seller-demo updated** to exercise the `data` path: prefetches each product's GLB to an `ArrayBuffer` cache on boot, then passes `data:` to `viewer.startGhostPlace`. Demonstrates the workflow customers will use for procedural/configurator models without forcing one in the demo itself.

## 0.8.1 — 2026-05-06

- **Fresh-run streaming fixes** (`v3/apps/viewer-sdk/src/orchestration.js`, `viewer.js`; commit `3444ad8`). Two bugs that only manifested on **uncached** runs (when `lidar-positions` arrives as `STREAM_CHUNK` rather than a single `ARTIFACT_HEADER`). Cached runs took the one-shot path and worked accidentally:
    - `orchestration.js` now correctly threads scene-origin context through chunked artifact assembly so the canopy / disc layers are positioned in the same frame as the streamed point cloud (was: discs drifted to absolute coords on first run).
    - `viewer.js` gained a 20-line guard that defers the initial frame-fit until the first artifact header has been parsed, eliminating the empty-scene flash + auto-frame-on-empty-bbox glitch on fresh runs.
    - Validated against seller-demo on a cold-cache location: discs + canopy render, preview cloud is centered on the canonical scene origin.
- **`npm run build:dev`** — new build target that produces the same library bundle in `dist-dev/` but with the `dioramapObfuscate` Vite plugin **skipped**. Pipeline / artifact / kind names stay friendly (`viewer-full`, `lidar-positions`, `OPAQUE_JSON`, …) so the local dev processor — which always runs with `DIORAMAP_OBFUSCATE=OFF` per `dev-up.sh` — accepts them. `control-plane/scripts/dev-up.sh:link_sdk_bundles` now symlinks viewer-sdk from `dist-dev/`, never `dist/`. Operators run `npm run build` once for the CDN-bound prod bundle and `npm run build:dev` whenever they want to validate SDK changes against the local stack (seller-demo, dashboard, picker dev page). Both build trees coexist; neither overwrites the other. Developer-experience polish — no behaviour change for embedders.

## 0.8.0 — 2026-05-06

**Breaking** for embedders: a new `<link rel="stylesheet">` is required.

- **Externalised CSS.** The SDK no longer injects `<style>` tags at runtime. All rules previously emitted by `attribution.js`, `controls-panel.js`, `feedback.js` (viewer-sdk) and `compass-gizmo.js` (scene-base), plus the per-element `el.style.cssText = '…'` strings in `loading-bar.js` and `model-interaction.js`, were lifted into a single `dioramap-viewer.css` Vite-extracted from `src/styles.css`. Embedders MUST add `<link rel="stylesheet" href="https://static.dioramap.com/sdk/v0.8.0/dioramap-viewer.css">` (or import the file from their bundler). Without it the SDK mounts but renders unstyled — the JS path is unchanged.
- **`'unsafe-inline'` no longer required** in embedders' Content Security Policy. The bundle has zero `createElement('style')`, zero `setAttribute('style', …)`, zero `el.style.cssText = …`, zero `eval` / `new Function` / `setTimeout("string")` / runtime `<script>` injection. Embedders can ship `script-src 'self' https://static.dioramap.com; style-src 'self' https://static.dioramap.com;` — see [`docs/api/viewer-sdk/#styling`](https://dioramap.com/docs/api/viewer-sdk/#styling) for the recommended header.
- **`--dioramap-*` theming variables** (public API, stable within 0.8.x). 17 CSS custom properties exposed at `:root` in the bundled stylesheet — colours (surface bg/border, text primary/secondary/muted, accent + danger + success), shape (radius/radius-lg/radius-pill), and typography (font-ui/font-mono). Embedders override by loading their own `<link>` AFTER ours and redeclaring on `:root`. Documented in `landing/docs/api/viewer-sdk/#styling`.
- **Class prefix unified to `.dioramap-*`.** The compass HUD's `.vw-compass-*` classes (legacy from the lab-ui port) renamed to `.dioramap-compass-*`. `lab-ui/src/layout/style.css` updated in lockstep so internal lab-ui doesn't regress. Any embedder who was targeting `.vw-compass-*` directly (undocumented) needs to rename.
- **`MarineCompass.dispose()` added** (`scene-base/src/scene/compass-gizmo.js`). Cancels the rAF tick loop, removes the root `<div>` from its host, nulls internal refs. Previously the optional-chained call in `viewer.js:dispose` silently no-op'd because the method didn't exist — re-mounting the SDK accumulated DOM compasses + leaked rAF loops (visible regression: two compasses on second mount).
- **Vite `cssCodeSplit: false`** for the build target so the lib produces a single `dist/style.css`. `dev-up.sh:link_sdk_bundles` symlinks it to `web/static/sdk/dioramap-viewer.css` (matching the public CDN path).

## 0.7.1 — 2026-05-05

- **Feedback pill polish** (`viewer-sdk/feedback.js` + `controls-panel.js` + `viewer.js`). Three small UX fixes from the agri-map.fr embed beta:
    - Pill anchored bottom-left (was bottom-right, where it fought the compass + attribution overlay for space).
    - Star icon in the pill is now amber `#fbbf24` — same color as the filled stars in the modal — so the affordance reads as a rating control even before click.
    - Controls panel collapses when the rating modal opens and restores its prior state on close, so the panel doesn't peek through the dim backdrop. New `collapse()` / `expand()` / `isCollapsed()` methods on the controls-panel handle. `viewer.js` threads the panel into `mountFeedback` as `controlsPanel`. `feedback.js` snapshots prior state at open time so a user who had the panel collapsed before opening stays collapsed on close.
- **Stray diagnostic logs removed** (`scene-base/scene/create-scene.js`). The two `console.log` calls left in the 0.6.1 ship — `[scene-base] init camera …` and `[scene-base] dimensions dump …` — both flagged "After this is verified across viewports, drop or gate behind a debug flag." Verified, dropped. Bundle size unchanged (~720 KB UMD).

## 0.7.0 — 2026-05-04

- **Render-quality feedback widget** (`viewer-sdk/feedback.js`, new
  module ~400 LOC). New optional `feedback: { enabled, embedderUserHash }`
  init option mounts a "Évaluer ce rendu" pill 60s after `run-complete`
  in the bottom-right of the SDK container. Click → modal with 5 stars
  + "Merci pour votre retour 🙂" header. Two-step UX:
    - Rating ≥4: submits immediately, swaps to a 1.5s "Merci" toast,
      closes. Frictionless capture for high ratings.
    - Rating ≤3: textarea expands below the stars with soft framing
      ("Qu'est-ce qui pourrait être mieux ?") + 500-char cap + manual
      "Envoyer" button. Comment is optional — submit stays active when
      empty so the rating still lands.
  POSTs to `https://app.dioramap.com/api/feedback/render` (configurable
  via `feedback.backendUrl`). `embedderUserHash` is a 64-char sha256 hex
  the embedder peppers — opaque to dioramap, lets the CP dedupe per-user
  spam without holding identity. localStorage gate
  `dioramap_fb_<run_id>` prevents re-prompting after submit. Failure
  path: inline error + retry on first failure, silent close on second
  (gate UNSET so a future render gets a clean prompt). Disabled
  silently if `embedderUserHash` is missing or malformed — feedback is
  non-essential, never throws or blocks the viewer. `run_id` captured
  from the existing `run-started` WS event (`apps/protocol/src/ws-client.js:128`),
  no protocol change. `onSubmitted({rating, hasComment})` optional
  callback for embedders that want their own UI gesture; the SDK shows
  its own toast regardless. Spec: `v3/docs/plan/PLAN-RENDER-FEEDBACK.md`.
- **Default backend URL fix** (`viewer-sdk/sdk.js:22`). Was
  `wss://api.dioramap.com/ws` (`api.` subdomain doesn't exist in our
  infra); now `wss://app.dioramap.com/ws`. Embedders that pass
  `backendUrl` explicitly (everyone today) are unaffected; the fix
  matters for the lone seller-demo / standalone case.
- **Companion**: control-plane will pin `dioramap.sdkVersion` to 0.7.0
  in its own release + serves the new public route
  `POST /api/feedback/render` that this widget targets. SDK degrades
  cleanly on older CPs (the POST 404s → treated as 4xx → silent close
  after second failure).

## 0.6.1 — 2026-05-01 (canvas sizing + camera framing + sky-leak seal)

Visual-quality patch from the agri-map.fr embed validation pass — all
fixes to existing scene-base / viewer-sdk modules, no API change.

- **Canvas CSS size fix** (`scene-base/scene/create-scene.js`).
  `renderer.setSize(w, h, false)` was the third-arg-`false` form, which
  sets the canvas's `width`/`height` HTML attributes (= framebuffer in
  device pixels) but skips updating `canvas.style.{width,height}`. With
  no CSS sizing the browser then renders the canvas at its attribute
  size **interpreted as CSS pixels** — i.e. `w × DPR` CSS wide, double
  the container at DPR=2. The visible canvas overflowed the container,
  only the left half showed, and the rendered scene's centre landed at
  the container's right edge. Fixed by passing `true` (or omitting the
  arg). Both at construct + on every resize.

- **Camera framing rule rewritten** (`scene-base/scene/create-scene.js`).
  Previously the camera was at fixed `(120,120,120)` regardless of
  aspect, so on mobile portrait (vFOV 55° ≈ hFOV 28°) the tile
  overflowed the viewport horizontally. New rule:
    - Tilt fixed 35° down toward tile centre (origin).
    - Distance computed from `max(D_horiz, D_vert)` where each is
      derived from the proper perspective projection of the disc
      (not the parallel-projection ellipse approximation, which
      under-estimates the near-edge angle and clips the bottom of
      the tile).
    - 10 % padding margin around the projected tile.
    - Per-aspect: portrait viewports horizontal-bound, landscape
      vertical-bound, equal at aspect ≈ 1 for tilt 35° / vFOV 55°.
  `Viewer` constructor takes a new `targetRadius` option (default 75
  to match the SDK default); `viewer-sdk/viewer.js` passes `cfg.radius`
  through. `OrbitControls.target` explicitly set to origin +
  `controls.update()` so the camera looks where it should.

- **Sky-leak seal — pit wall + underside + outer skirt + bottom caps**
  (`scene-base/viz/elevation-surround.viz.js`). Three new black meshes
  prevent the camera from seeing the sky background through the scene:
    - **Pit wall**: cylinder seated INSIDE the surround's inner-radius
      cutout (the disc footprint). Top rim follows the elevation
      contour at radius=`innerRadius`, bottom 1000 m below the lowest
      seal point. Side walls only — the wider skirt floor (below)
      covers the same area, so a pit floor would just z-fight.
    - **Surround underside**: sibling mesh sharing the surround's
      geometry but `side: BackSide`, flat black, fog-disabled. Same
      `onBeforeCompile` inner-radius discard as the front so it
      doesn't fill the disc cutout from below either.
    - **Surround outer skirt**: 240-vertex perimeter wall hanging from
      the 1200×1200 m surround edge down to a flat rectangular bottom
      cap. Seals all four outer sides AND the bottom — vertical-down
      views no longer see sky through the open box.

- **No-data → dilation-filled** (same file). IGN's RGE ALTI HIGHRES
  layer is land-only; sea cells return a -99999 sentinel. Earlier
  attempts (z=0 in scene; -origin.z = MSL; tile-global minZ) all looked
  wrong somewhere — tsunami above land, deep cliff into the void, or
  flat slab at a far-away valley elevation. New approach: iterative
  dilation from nearest valid neighbour. Each pass, every still-invalid
  cell with ≥ 1 valid 4-neighbour adopts their average Z and is
  promoted to valid for the next pass. Sea cells touching the coast
  take coast Z; cells one out take that newly-filled Z; converges to
  flat sea at coast elevation. Inland gaps interpolate smoothly.
  `sampleSurroundZ` (used by pit + skirt seals) reads the dilated
  values too, so the rim never notches over no-data zones.

- **Companion build-config**: `dev-up.sh` (control-plane) now enforces
  ASan + UBSan + Debug + no-obfuscation for the local processor binary
  (separate `build-dev/` directory, config drift detected via
  CMakeCache.txt, runtime `ASAN_OPTIONS` set on launch + `ulimit -c
  unlimited`). Caught a real `use-after-free in run_journal_record` on
  all-cache-hit runs in the C processor — documented in
  `v3/docs/BUG-cache-hit-runner-cleanup.md` for follow-up. SDK-side
  unaffected.

Companion to control-plane v0.0.15 (pins `dioramap.sdkVersion` to 0.6.1).

## 0.6.0 — 2026-05-01 (mobile perf + dispose robustness + collapsible controls)

Three independent improvements observed on the agri-map.fr embed
testing pass — none of them break the public API.

- **Cap renderer pixel ratio at 2** (`scene-base/scene/create-scene.js`).
  Previously the WebGL canvas allocated `width × DPR` pixels uncapped:
  on a 400 × 800 mobile viewport with `devicePixelRatio = 4`, that's a
  1600 × 3200 ≈ 5 megapixel framebuffer. Capped to `min(DPR, 2)` brings
  it back down to ~1 megapixel — same visible quality for point clouds
  + meshes at this scale, big perf win on integrated mobile GPUs and
  ~25 % less GPU memory. Standard WebGL practice (Sketchfab, Mapbox GL,
  three.js editor all cap at 2).
- **Defensive dispose** (`viewer-sdk/models/model-interaction.js`). Each
  cleanup step in `dispose()` now wraps in `try/catch` so a failure in
  one (notably `gizmo.dispose()` from Three.js r169's restructured
  TransformControls — `this.traverse` no longer exists on the gizmo
  itself, throws `TypeError`) doesn't tank the rest of the chain. The
  scene-remove + reference-drop is enough; WebGL resources GC normally.
  Stack trace observed: `viewer.dispose → models._dispose → interaction.dispose
  → gizmo.dispose → this.traverse is not a function`. Now logs a warn
  and continues.
- **Collapsible controls panel** (`viewer-sdk/controls-panel.js`). Adds
  a clickable header with caret that toggles the panel body. Default-
  collapsed under 768 px (mobile + small tablet) so the panel doesn't
  eat half the viewport on phone-sized embeds; desktop stays open. No
  API change — purely DOM/CSS, slightly higher initial render in mobile
  mode (caret + header bar).

Companion to control-plane v0.0.14 (pins `dioramap.sdkVersion` to 0.6.0).
Old `/sdk/v0.5.0/` URLs continue to serve until pruned.

## 0.5.0 — 2026-05-01 (ship both UMD + ES formats; unify naming)

**Breaking** — `dioramap-viewer.js` is gone; replaced by two
explicitly-named files. Embedders pick whichever fits their stack:

| File | Use it if you… | API access |
|---|---|---|
| `dioramap-viewer.umd.js`               | Load via `<script src>` (no bundler) | `window.DioramapViewer.init({…})` |
| `dioramap-viewer.es.js`                | `import` from a bundler (Vite, webpack, esbuild) | `import { init } from '…/dioramap-viewer.es.js'` |
| `dioramap-location-picker.umd.js`      | Load via `<script src>` (no bundler) | `window.DioramapLocationPicker.init({…})` |
| `dioramap-location-picker.es.js`       | `import` from a bundler | `import { init } from '…/dioramap-location-picker.es.js'` |
| `dioramap-location-picker.css`         | Always | `<link rel="stylesheet" href="…">` |

Two cleanups in one bump:
1. **Format suffix in filenames** — viewer-sdk's previous
   `dioramap-viewer.js` was actually UMD-only (`fileName: () => 'dioramap-viewer.js'`
   with `formats: ['es', 'umd']` meant Vite wrote ES then overwrote with
   UMD). The ambiguous filename caused at least one embedder to try
   `import { init } from '…dioramap-viewer.js'` expecting an ES module
   and get `doesn't provide an export named 'init'`. Now the format is
   in the filename.
2. **ES + UMD shipped for both packages.** Previously only the
   `<script>`-tag bundles were uploaded to `static.dioramap.com` (UMD
   for viewer, IIFE for picker — different formats for no good reason).
   Now both packages ship both ES and UMD with consistent naming.
3. **IIFE → UMD for the location picker** so both packages use the same
   format pair. Functionally identical for `<script>`-tag consumers
   (both expose the window global); the only difference is the few
   bytes of UMD's env-detection wrapper.

Companion to control-plane v0.0.11 (pins `dioramap.sdkVersion` to 0.5.0
and switches `viewer.ejs` script tags to the new `.umd.js` filenames).
The landing-page docs at `dioramap.com/docs/api/{viewer-sdk,location-picker}/`
now show both loading patterns side-by-side.

Old `/sdk/v0.4.0/` URLs continue to serve until pruned. The dashboard
already switched. The only known third-party embedder (agri-map.fr's
`/3d.html`) gets bumped in lockstep.

## 0.4.0 — 2026-05-01 (rename lidar-* → dioramap-*)

**Breaking** — file names + UMD globals renamed. Embedders update both
the `<script src>` URL and the `window.…` global reference:

| Before                          | After                              |
|---------------------------------|------------------------------------|
| `lidar-viewer.js`               | `dioramap-viewer.js`               |
| `lidar-location-picker.iife.js` | `dioramap-location-picker.iife.js` |
| `lidar-location-picker.css`     | `dioramap-location-picker.css`     |
| `window.LidarViewer`            | `window.DioramapViewer`            |
| `window.LidarLocationPicker`    | `window.DioramapLocationPicker`    |

Companion to control-plane v0.0.10 (pins `dioramap.sdkVersion` to
0.4.0). Pinned `/sdk/v0.3.0/lidar-*` URLs continue to serve until the
v0.3.0 directory is pruned (but no new embedders should reference
them — the only remaining consumer is the dashboard, switched by CP
v0.0.10). Companion lidar-algos commit: `44ed22b`.

## 0.3.0 — 2026-05-01

D6 obfuscation Phase 2 — full ship (SDK side).
- Vite builds now run the `dioramapObfuscate()` plugin (wired in
  `vite.config.js` under `command === 'build'`), substituting `t("…")`
  call sites with `tt_<8hex>` tokens drawn from the committed
  `v3/scripts/obfuscation-map.json` — same map the processor binary
  uses, so step / artifact / pipeline names line up across the
  WS protocol. Dev mode stays a no-op so HMR + sourcemaps remain
  clean.
- Audit (`grep` against the prod-mode `dist/dioramap-viewer.js`):
  zero plaintext step / pipeline / artifact identifiers. Three
  internal `lidar-positions-preview` / `parcel-lines` strings remain
  — Three.js layer IDs / `Object3D.name` properties on the SDK side
  only, never hit the wire. Acceptable false positives matching the
  C-side audit's policy.
- Companion to processor v0.2.0 + control-plane v0.0.9: redeploys
  pre-D6 SDK versions (v0.1.0, v0.2.0) get pruned from the prod box's
  `<static-hosting>/sdk/` since they leak the algo graph
  via `strings(1)` on the bundle. See operator note in
  `v3/core/CHANGELOG.md` v0.2.0.
- No public API change. Embedders who pin `/sdk/v0.2.0/...` keep
  working off their cached version (CP no longer references it).

## 0.2.0 — 2026-04-30

D5a — pipeline-driven attribution overlay.
- New `src/attribution.js` mounts an always-visible bottom-right credit:
  "Powered by [dioramap.com] · Data: <pipeline-declared labels>". The
  "Powered by" half is hardcoded; the "Data:" half is rebuilt every
  CATALOG_RESPONSE from the `attributions[]` field added in D0
  (processor side). Cannot be disabled via SDK options — contractual
  attribution requirement, not a feature flag.
- Mobile-aware: at container widths < 480px the overlay collapses
  to a compact one-liner (drops "Powered by" / "Data:" prefixes).
  ResizeObserver-driven, fullscreen-safe.
- CSS scoped via `.dioramap-attr-…` class prefix; injected once via
  a single `<style>` tag at first mount.
- Wired into `viewer.js`: mounts after the loading bar, updates on
  every catalog event, disposed alongside other overlays.
- Bundle delta: ~3 kB minified / ~1.5 kB gzip.

## 0.1.0 — 2026-04-27 (V5 + V6 + V7 + V8: production-embed-ready cut)

Minor bump (0.0.x → 0.1.0): the public `DioramapViewer.init` surface is
now feature-complete enough for a production embed. All §5.7 frozen
methods that returned warn-once stubs are now wired to real
implementations. CHANGELOG bumps from here are still required for any
EXPOSED_VISIBILITY_TOGGLES change (§5.5).

V5 — sun-direction slider init.
- Subscribe to `sun-direction` (output of `find-best-sun-time`); on
  arrival call `sceneState.sun.setFromDate({...})` so the slider seeds
  at the photo-capture time. Cached re-runs hit the same path (a single
  ARTIFACT_HEADER frame still fires the artifact event). ~20 LOC in
  `viewer.js`'s ws.artifact handler.

V6 — GLB models system.
- `src/models/{model-manager, model-interaction, ghost-placement, index}.js`
  ported from the legacy v2 SDK (`lidar-3d-environment-viewer/frontend/
  src/models/`) and adapted to v3's Z-up frame (rotation.x = +PI/2 for
  Y-up GLBs; flip and you get models floating high above ground).
  GLTFLoader → wrap Group → ground-snap via top-down raycaster that
  excludes models, dev helpers, the bbox helper, and TransformControls'
  invisible drag-plane mesh (the latter was the cause of an "X/Y drag
  goes wrong direction" symptom).
- Public methods now wired: `addModel`, `startGhostPlace`, `cancelGhost`,
  `removeModel`, `getModels`, `getModel`, `selectModel`, `deselectModel`,
  `getSelectedModel`, `setGizmoMode`, `setModelTransform`. Selection
  shows a cyan Box3Helper + TransformControls + CSS2D action buttons
  (move / rotate / delete) parked at (cx, cy, max.z + 0.3).
- Per-frame ground-follow on X/Y drags (legacy v2 behaviour); pure-Z
  drag = manual height. Ground-dome ground discs receive shadows
  (separately fixed in `@dioramap/scene-base@0.1.0` —
  `getShadowMask()`-modulated dome RGB so the legacy "dark dome tip"
  doesn't return).
- Dev page (`apps/viewer-sdk/index.html` + `src/main.js`) gained a
  small lower-left "Models (V6 dev test)" panel with three GLB buttons
  (storage-shed / cabin-shed / swimming-pool, copied to
  `public/models/`) and a placed-list with click-to-select + delete +
  live xyz that updates during gizmo drag. Lives outside the SDK
  bundle (the seller-side demo with API-token integration is a separate
  later workstream).

V7 — loading overlay.
- Centred dark card with a green progress bar mounted inside the SDK's
  container (`src/loading-bar.js`). Driven by RUN_STARTED /
  STEP_COMPLETE / RUN_COMPLETE from the WSClient. Label stays
  "Loading…" — internal pipeline step names aren't shown to end users.
  Cap at 99 %; only RUN_COMPLETE fills to 100 %, fades, hides.

V8 — polish + bundle-size + CHANGELOG v0.1.0.
- New `npm run check:bundle-size` (and combined `npm run check`) — guards
  raw / gzip thresholds AND scans the bundle for distinctive labels of
  composers the SDK is supposed to NOT include (terrain.viz,
  deshadowed-ortho.viz, ortho-surround-jpeg.viz). Catches accidental
  imports of `@dioramap/scene-base/viz` (the index) or transitive leaks
  per §5.8.
- Current bundle (UMD): 699 kB raw / 185 kB gzipped. Budget: 1.1 MB
  raw / 220 kB gzipped. The +180 kB jump from 0.0.3 (519 kB UMD) is
  GLTFLoader + TransformControls + CSS2DRenderer for V6.

Open dispose-audit notes (deferred to S7 — `@dioramap/scene-base` cleanup):
the Viewer (scene-base) does not yet expose a `dispose()` for its
internal rAF; the SDK's own resources (rAFs, listeners, WSClient,
panels, models, loading-bar, compass) are properly cleaned up in
`mountViewer().dispose()` and `models._dispose()`.

## 0.0.3 — 2026-04-26 (S5 + V4 milestones, with polish)

- Three new composite-visualizers in `@dioramap/scene-base/viz/`:
  `terrain.viz` (heightmap from `lidar-ground-grid` textured with
  deshadowed RGB), `point-cloud-discs.viz` (oriented hex InstancedMesh,
  radial-fade silhouettes, 25%-scale shadow depth material),
  `canopy.viz` (leaf-textured billboards, 2 instances per canopy point
  with random tilt, alpha-discard via custom shader injection).
- SDK ORCHESTRATION: `[preview, parcels, discs, canopy, surround]`.
  `terrain` + `ground-ortho` excluded from SDK (discs paint per-vertex
  ortho — separate ground would z-fight). Lab-ui still has them via
  the composite-views panel.
- `meta.supersedes` mechanism — discs declares `supersedes:
  ['lidar-positions-preview']`, so the streaming preview hides itself
  the moment the high-quality discs render.
- `EXPOSED_VISIBILITY_TOGGLES` collapsed to one curated entry
  (`parcels` — Cadastre outlines). Discs/canopy/surround are part of
  the production scene and not user-toggleable per §5.5.
- Viewer constructor options: `devHelpers` (axes/grid only when true —
  SDK default false; lab-ui passes true), `maxFps` (frame cap, default
  30 for production embed), `edl` (EffectComposer + EDL pass — default
  true, wired via 32-bit depth texture matching legacy SDK to avoid
  moire from depth quantization on the regular hex disc grid).
- Shadow map enabled with a 600 m orthographic frustum on the sun
  light. Disc/canopy meshes already cast/receive — visible shadows
  on terrain.
- Subtle distance fog (near=500, far=1200) so the surround tile fades
  cleanly into the sky-blue background at its outer edge.
- Surround composite (renamed from `elevation-surround.viz`): now
  combines `elevation-surround-f32` + `ortho-surround-jpeg` into one
  textured displaced mesh covering ±600 m around canonical center.
  Index-buffer culls every triangle touching a no-data vertex (clean
  hard cutoff, no smear-zone dive). Inner-radius hole at the lidar
  disc footprint so the surround doesn't overpaint the discs.
- Server-side fix: ARTIFACT_HEADER + binary frame pair now atomic
  on emit (was racy under concurrent on_artifact_ready callbacks
  from the thread pool — surfaced as 90008-byte denormal-F64 garbage
  for `lidar-positions` in lab-ui).
- C step `fetch-elevation` + `fetch-elevation-surround` now write
  proper `raster_format`-compliant bytes (12-byte header + F32 grid).
  Versions bumped 1 → 2.

## 0.0.2 — 2026-04-25 (V3 milestone)

- Added `src/orchestration.js`: closed-allow-list dispatcher. Tracks
  artifact kinds from CATALOG_RESPONSE, decodes ARTIFACT bytes via
  `decodeArtifact(kind, name, bytes)`, calls each composer's `render`
  when its `meta.requires` are satisfied, and routes STREAM_CHUNK
  events through the composer's `beginStream`/`appendChunk`/
  `finalizeStream` triplet. Suppresses the trailing ARTIFACT_HEADER
  that the server resends after a streaming sequence (dual delivery).
- `viewer.js` now does the two-step handshake: SESSION_STARTED →
  CATALOG `viewer-full` → seed kinds → RUN. ORCHESTRATION explicit-imports
  three composers from `@dioramap/scene-base/viz/*` per §5.2:
  `lidar-positions` (streaming preview), `deshadowed-ortho` (ground
  plane), `parcel-lines` (cadastre).
- Plan adjustment vs §7: `terrain.viz` is an S5 multi-artifact composer
  that hasn't shipped, so V3 substitutes `parcel-lines` from the S4 set
  to ship the planned three composers. `terrain` lands at V4 alongside
  S5.
- `controls-panel.js` `EXPOSED_VISIBILITY_TOGGLES` populated with the
  three curated entries matching the leaf IDs the composers register:
  `lidar-positions-preview`, `ground-ortho`, `parcels`. Adding/removing
  entries here requires a documented CHANGELOG bump per §5.5.
- Bundle: 526 kB / 135 kB gzipped (+12 kB / +3 kB gzipped vs V2 — three
  viz modules + orchestration).

## 0.0.1 — 2026-04-25 (V2 milestone)

- Scaffolded `v3/apps/viewer-sdk/`. Vite library build → `dist/dioramap-viewer.js`
  (ES + UMD `DioramapViewer`); standalone dev page on port 5182.
- Frozen public `DioramapViewer.init({...})` surface (PLAN-VIEWER-SDK-PORT §5.7).
  Working today: `init`, `setSunTime`, `setSunEnabled`, `dispose`. All other
  methods are stubs that warn-once and return safe values until later
  milestones (V3+ for visibility, V6 for models).
- Mounts `Viewer` (Three.js scene) + `SceneState` (sun + visibility tree)
  from `@dioramap/scene-base`. Sun defaults to 2024-06-15 12:00 local.
- Mounts `controls-panel.js` — sun-only (3-slider Month/Day/Time, packed
  time slot 0..143 matching legacy SDK). `EXPOSED_VISIBILITY_TOGGLES = []`
  for V2; placeholder "(no layers configured)" rendered.
- Connects `WSClient` (`@dioramap/protocol`) and sends `RUN viewer-full`
  with the §5.4 frozen subscription set on `SESSION_STARTED`. Composer
  routing not wired yet (V3) — incoming artifacts decode + log only.
